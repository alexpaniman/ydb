#include "kqp_rules_include.h"


namespace {
using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

bool IsNullRejectingPredicate(const TExpression& filter) {

    using O = NFrontend::EOpcode;
    using K = NFrontend::ETypeKind;
    const auto& arena = *filter.GetNativeArena();
    const auto stripLift = [&](NFrontend::TExprId id) {
        while (arena[id].Op == O::Convert &&
            arena[id].Type.Required() == arena[arena[id].Arguments[0]].Type.Required())
            id = arena[id].Arguments[0];
        return id;
    };
    auto id = stripLift(filter.GetNativeRoot());
    if (arena[id].Op == O::Not) id = stripLift(arena[id].Arguments[0]);
    const auto& predicate = arena[id];
    switch (predicate.Op) {
        case O::Equal: case O::NotEqual: case O::Less: case O::LessEqual:
        case O::Greater: case O::GreaterEqual: case O::Contains:
        case O::StartsWith: case O::EndsWith: break;
        default: return false;
    }
    const auto isConstant = [&](NFrontend::TExprId id) {
        const auto& node = arena[stripLift(id)];
        if (node.Op != O::Literal || node.Type.OptionalDepth || std::holds_alternative<std::monostate>(node.Value)) return false;
        return node.Type.IsIntegral() || node.Type.IsString() || node.Type.Kind == K::Bool ||
            node.Type.Kind == K::Float || node.Type.Kind == K::Double || node.Type.IsDate() || node.Type.IsInterval();
    };
    const auto left = predicate.Arguments[0], right = predicate.Arguments[1];
    const auto isColumn = [&](NFrontend::TExprId value) {
        value = stripLift(value);
        if ((predicate.Op == O::Contains || predicate.Op == O::StartsWith || predicate.Op == O::EndsWith) &&
            arena[value].Op == O::Convert && arena[value].Type.Kind == K::String &&
            arena[arena[value].Arguments[0]].Type.Kind == K::Utf8)
            value = stripLift(arena[value].Arguments[0]);
        return arena[value].Op == O::Column;
    };
    // Retain the existing rule's column-versus-literal criterion. Native
    // representation does not broaden outer-join rewrite policy.
    return (isColumn(left) && isConstant(right)) || (isColumn(right) && isConstant(left));
}
}

namespace NKikimr {
namespace NKqp {

bool TPushFilterIntoJoinRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter &&
        input->GetChildren().front()->Kind == EOperator::Join;
}

// FIXME: We currently support pushing filter into Inner, Cross and Left Join
TIntrusivePtr<IOperator> TPushFilterIntoJoinRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    if (input->Kind != EOperator::Filter) {
        return input;
    }

    auto filter = CastOperator<TOpFilter>(input);
    if (filter->GetInput()->Kind != EOperator::Join) {
        return input;
    }

    auto join = CastOperator<TOpJoin>(filter->GetInput());

    if (join->JoinKind != "Inner" && join->JoinKind != "Cross" && join->JoinKind != "Left" && join->JoinKind != "LeftSemi" && join->JoinKind != "LeftOnly") {
        YQL_CLOG(TRACE, CoreDq) << "Wrong join type " << join->JoinKind << Endl;
        return input;
    }

    const bool usingBlockCrossJoin = ctx.KqpCtx.Config->GetUseBlockHashJoin() && ctx.KqpCtx.Config->GetUseBlockHashJoinForCross();
    const bool forceInlining = ctx.KqpCtx.Config->GetEnableInlineJoinFiltersAfterCBO();

    if (join->JoinKind == "Cross" && usingBlockCrossJoin && !forceInlining) {
        auto conjuncts = filter->GetFilterExpression().SplitConjunct();
        // If all not equal - we cannot rewrite cross to inner, just adding them as join filters.
        const bool containsEquiJoinCondition = AnyOf(conjuncts, [](const TExpression& conjunct) {
            return conjunct.MaybeEquiJoinCondition();
        });
        if (!containsEquiJoinCondition) {
            join->JoinFilters.insert(join->JoinFilters.end(), conjuncts.begin(), conjuncts.end());
            return join;
        }
    }

    auto leftIUs = join->GetLeftInput()->GetOutputIUs();
    auto rightIUs = join->GetRightInput()->GetOutputIUs();

    // Break the filter into join conditions and other conjuncts
    // Join conditions can be pushed into the join operator and conjucts can either be pushed
    // or left on top of the join
    auto conjuncts = filter->GetFilterExpression().SplitConjunct();

    // Check if we need a top level filter
    TVector<TExpression> topLevelPreds;
    TVector<TExpression> pushLeft;
    TVector<TExpression> pushRight;
    TJoinIUs joinConditions;

    bool canPushRight = join->JoinKind != "LeftSemi" && join->JoinKind != "LeftOnly";

    for (const auto& conj : conjuncts) {
        if (conj.MaybeEquiJoinCondition() && !ReferencesUnresolvedSubplan(conj, props)) {
            TEquiJoinCondition cond(conj);

            // We cannot push filter into join conditions of a LeftOnly join - will break semantics
            if(join->JoinKind != "LeftOnly") {
                if (leftIUs.Contains(cond.GetLeftIU()) && rightIUs.Contains(cond.GetRightIU())) {
                    joinConditions.Add(cond.GetLeftIU(), cond.GetRightIU());
                    continue;
                } else if (rightIUs.Contains(cond.GetLeftIU()) && leftIUs.Contains(cond.GetRightIU())) {
                    joinConditions.Add(cond.GetRightIU(), cond.GetLeftIU());
                    continue;
                }
            }
        }

        if (conj.GetInputIUs(/*includeSubplanVars=*/true, /*includeCorrelatedDeps=*/true).IsSubsetOf(leftIUs)) {
            pushLeft.push_back(conj);
        } else if (conj.GetInputIUs(/*includeSubplanVars=*/true, /*includeCorrelatedDeps=*/true).IsSubsetOf(rightIUs) && canPushRight) {
            pushRight.push_back(conj);
        } else {
            topLevelPreds.push_back(conj);
        }
    
    }

    if (!pushLeft.size() && !pushRight.size() && !joinConditions.Items().size()) {
        YQL_CLOG(TRACE, CoreDq) << "Nothing to push";
        return input;
    }

    if ((join->JoinKind == "Cross" || join->JoinKind == "Left" ) && !joinConditions.Items().empty()) {
        join->JoinKind = "Inner";
    }

    for (const auto& key : joinConditions.Items()) {
        join->JoinKeys.Add(key);
    }

    // When join conditions have been set for the join, replicate constant conditions from left/right side
    // to the other side. This optimization is enabled for inner joins
    // FIXME: Check if we can expand this to Left Joins
   
    if (join->JoinKind == "Inner") {
        TVector<TExpression> pushConstantCondsRight;
        TVector<TExpression> pushConstantCondsLeft;

        // Check if left constant condition on the left key can be pushed to right side
        for (const auto& expr : pushLeft) {
            if (expr.MaybeConstantCondition()) {
                auto iu = *expr.GetInputIUs().begin();
                if (auto it = std::find_if(join->JoinKeys.Items().begin(), join->JoinKeys.Items().end(), [&iu](const auto& cond)
                    {return iu == cond.first;}); it != join->JoinKeys.Items().end()) {
                    auto rightExpr = expr.ApplyRenames({{iu, it->second}});
                    pushConstantCondsRight.push_back(rightExpr);
                }
            }
        }
        // Check if right constant condition on the right key can be pushed to left side
        for (const auto& expr : pushRight) {
            if (expr.MaybeConstantCondition()) {
                auto iu = *expr.GetInputIUs().begin();
                if (auto it = std::find_if(join->JoinKeys.Items().begin(), join->JoinKeys.Items().end(), [&iu](const auto& cond)
                    {return iu == cond.second;}); it != join->JoinKeys.Items().end()) {
                    auto leftExpr = expr.ApplyRenames({{iu, it->first}});
                    pushConstantCondsLeft.push_back(leftExpr);
                }
            }
        }

        pushRight.insert(pushRight.end(), pushConstantCondsRight.begin(), pushConstantCondsRight.end());
        pushLeft.insert(pushLeft.end(), pushConstantCondsLeft.begin(), pushConstantCondsLeft.end());
    }

    if (pushLeft.size()) {
        auto leftExpr = MakeConjunction(pushLeft);
        join->SetLeftInput(MakeIntrusive<TOpFilter>(join->GetLeftInput(), input->Pos, leftExpr));
    }

    if (pushRight.size()) {
        if (join->JoinKind == "Left") {
            TVector<TExpression> predicatesForRightSide;
            for (const auto &predicate : pushRight) {
                if (IsNullRejectingPredicate(predicate)) {
                    predicatesForRightSide.push_back(predicate);
                } else {
                    topLevelPreds.push_back(predicate);
                }
            }
            if (!predicatesForRightSide.empty()) {
                auto rightExpr = MakeConjunction(predicatesForRightSide);
                join->SetRightInput(MakeIntrusive<TOpFilter>(join->GetRightInput(), input->Pos, rightExpr));
                join->JoinKind = "Inner";
            } else if (!pushLeft.size()) {
                return input;
            }
        } else {
            auto rightExpr = MakeConjunction(pushRight);
            join->SetRightInput(MakeIntrusive<TOpFilter>(join->GetRightInput(), input->Pos, rightExpr));
        }
    }

    if (topLevelPreds.size()) {
        auto topFilterExpr = MakeConjunction(topLevelPreds);
        return MakeIntrusive<TOpFilter>(join, input->Pos, topFilterExpr);
    }

    return join;
}
}
}
