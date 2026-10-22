#include "kqp_rules_include.h"

#include "decorrelation/dependent_join_pushdown.h"

namespace NKikimr {
namespace NKqp {

namespace {

// Unlike some, min is deterministic, so a shared plan with the subquery can be copied for each consumer.
TString OnlyValueAggregation(const TTypeAnnotationNode* valueType) {
    if (!valueType) {
        return "some";
    }
    const auto compare = NYql::CanCompare<false>(valueType, valueType);
    return compare == NYql::ECompareOptions::Comparable || compare == NYql::ECompareOptions::Optional ? "min" : "some";
}

struct TScalarState {
    TIntrusivePtr<IOperator> Input;
    TInfoUnitId Value;
    TInfoUnitId Count;
};

// Collect one value and its cardinality per binding. Validation belongs at
// the scalar expression's use, inside any conditional that guards that use.
TScalarState MakeScalarState(const TIntrusivePtr<IOperator>& input, const TOrderedIUs<>& groupKeys,
                             TInfoUnitId valueIU, const TTypeAnnotationNode* valueType,
                             TPositionHandle pos, TRBOContext& ctx, TPlanProps& props) {
    auto rowIU = props.InfoUnitRegistry.AddGenerated("row");
    TMapIUs rowElements;
    rowElements.Add(rowIU, MakeConstant("Uint64", "1", pos, &ctx.ExprCtx));
    auto rowMap = MakeIntrusive<TOpMap>(input, pos, rowElements);

    auto countIU = props.InfoUnitRegistry.AddGenerated("row_count");
    auto valueStateIU = props.InfoUnitRegistry.AddGenerated("scalar_value");

    TAggregationIUs traits;
    traits.Add(countIU, TOpAggregationTraits{rowIU, "count"});
    // This is need to get the actual value, we emit ensure that we get only one row, so its minimum is the value.
    traits.Add(valueStateIU, TOpAggregationTraits{valueIU, OnlyValueAggregation(valueType)});
    auto aggregate = MakeIntrusive<TOpAggregate>(rowMap, traits, groupKeys, EOpPhase::Undefined, /*distinctAll=*/false, pos);

    return {std::move(aggregate), valueStateIU, countIU};
}

void CheckScalarAtUses(IOperator& consumer, TInfoUnitId scalarIU, TInfoUnitId valueIU, TInfoUnitId countIU, TRBOContext& ctx) {
    TNodeOnNodeOwnedMap replacements;
    for (const auto& expression : consumer.GetExpressions()) {
        const auto& lambda = expression.get().GetLambda();
        const auto& row = lambda->Head().Head();
        VisitExpr(expression.get().GetExpressionBody(), [&](const TExprNode::TPtr& node) {
            if (!node->IsCallable("Member") || &node->Head() != &row || GetMemberId(*node) != scalarIU) {
                return true;
            }
            const auto pos = node->Pos();
            const auto member = [&](TInfoUnitId id) {
                return ctx.ExprCtx.ChangeChild(*node, 1, ctx.ExprCtx.NewAtom(pos, ctx.ExprCtx.GetIndexAsString(id)));
            };
            const auto zero = ctx.ExprCtx.NewCallable(pos, "Uint64", {ctx.ExprCtx.NewAtom(pos, "0")});
            const auto one = ctx.ExprCtx.NewCallable(pos, "Uint64", {ctx.ExprCtx.NewAtom(pos, "1")});
            // A correlated Left join has no right row for an empty subquery.
            const auto count = ctx.ExprCtx.NewCallable(pos, "Coalesce", {member(countIU), zero});
            const auto atMostOne = ctx.ExprCtx.NewCallable(pos, "<=", {count, one});
            const auto message = ctx.ExprCtx.NewCallable(pos, "String", {
                ctx.ExprCtx.NewAtom(pos, "Scalar subquery returned more than one row")});
            replacements.emplace(node.Get(), ctx.ExprCtx.NewCallable(pos, "Ensure", {member(valueIU), atMostOne, message}));
            return false;
        });
    }
    Y_ENSURE(!replacements.empty(), "Missing scalar subquery use");
    consumer.ApplyReplaceMap(replacements, ctx);
}

} // anonymous namespace

bool TInlineScalarSubplanRule::QuickMatch(const TIntrusivePtr<IOperator>& input, const TPlanProps& props) const {
    if (props.Subplans.Empty()) {
        return false;
    }

    for (const auto iu : input->GetSubplanIUs(props.Subplans)) {
        if (props.Subplans.At(iu).Type == ESubplanType::EXPR) {
            return true;
        }
    }

    return false;
}

// Rewrite a single scalar subplan into a cross-join for uncorrelated queries
// or into a left join for correlated (assuming at most one tuple in the output of each subquery)
// FIXME: Need to do correct general case decorellation in the future

bool TInlineScalarSubplanRule::MatchAndApply(TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    TVector<TInfoUnitId> scalarIUs;
    for (const auto iu : input->GetSubplanIUs(props.Subplans)) {
        if (props.Subplans.At(iu).Type == ESubplanType::EXPR) {
            scalarIUs.push_back(iu);
            break;
        }
    }

    if (scalarIUs.empty()) {
        return false;
    }

    auto scalarIU = scalarIUs[0];
    const auto& subplanEntry = props.Subplans.At(scalarIU);
    auto subplan = CastOperator<IOperator>(subplanEntry.Plan);
    Y_ENSURE(subplanEntry.ResultIU, "Missing scalar result binding");
    auto subplanResIU = *subplanEntry.ResultIU;
    const auto* subplanResType = subplan->Type ? subplan->GetIUType(subplanResIU, ctx.ExprCtx) : nullptr;

    Y_ENSURE(MatchOperator<IUnaryOperator>(input));
    auto unaryOp = CastOperator<IUnaryOperator>(input);

    auto child = unaryOp->GetInput();

    if (HasFreeCorrelation(subplan, subplanEntry.DependentIUs)) {
        const auto& dependencies = subplanEntry.DependentIUs;
        auto leftIUs = child->GetOutputIUs();
        for (const auto iu : dependencies) {
            Y_ENSURE(leftIUs.Contains(iu), TStringBuilder() << "Correlation column " << props.InfoUnitRegistry.GetDebugName(iu) << " is not produced by the outer plan");
        }

        // The outer plan and the domain read the child through a Replicate, the domain under fresh IDs.
        auto domain = MakeSubplanDomain(child, dependencies, subplan->Pos, props);
        const TOrderedIUs<> domainColumns(domain.Keys.Right().begin(), domain.Keys.Right().end());
        TJoinIUs joinKeys = domain.Keys;
        auto dependentJoin = std::move(domain).Bind(subplan, subplan->Pos);

        auto state = MakeScalarState(dependentJoin, domainColumns, subplanResIU, subplanResType, subplan->Pos, ctx, props);

        TIntrusivePtr<IOperator> joinLeftInput = child;
        TIntrusivePtr<IOperator> joinRightInput = state.Input;
        joinKeys = MakeNullSafeJoinKeys(joinLeftInput, joinRightInput, joinKeys, subplan->Pos, ctx, props);

        auto leftJoin = MakeIntrusive<TOpJoin>(joinLeftInput, joinRightInput, subplan->Pos, "Left", joinKeys);

        unaryOp->SetInput(leftJoin);
        CheckScalarAtUses(*input, scalarIU, state.Value, state.Count, ctx);
    }
    // Otherwise we assume an uncorrelated supbplan
    else {
        auto state = MakeScalarState(subplan, {}, subplanResIU, subplanResType, subplan->Pos, ctx, props);

        TJoinIUs joinKeys;
        auto cross = MakeIntrusive<TOpJoin>(child, state.Input, subplan->Pos, "Cross", joinKeys);
        unaryOp->SetInput(cross);
        CheckScalarAtUses(*input, scalarIU, state.Value, state.Count, ctx);
    }

    props.Subplans.Remove(scalarIU);

    return true;
}
}
}
