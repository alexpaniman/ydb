#include "kqp_rules_include.h"

namespace NKikimr {
namespace NKqp {

namespace {

// ON predicates are evaluated against the inputs before outer-join padding.
// A computed equality whose operands each belong to one input can therefore
// become a key on that input, including for outer, semi and anti joins.
bool ExtractNativeJoinKeys(TOpJoin& join, TRBOContext& ctx, TPlanProps& props) {
    if (!props.NativeExpressions) return false;
    const auto left = join.GetLeftInput()->GetOutputIUs(), right = join.GetRightInput()->GetOutputIUs();
    TMapIUs leftDefinitions, rightDefinitions;
    TVector<TExpression> remaining;
    bool changed = false;
    for (const auto& predicate : join.JoinFilters) {
        if (!predicate.MaybeExprEquiJoinCondition() || ReferencesUnresolvedSubplan(predicate, props)) {
            remaining.push_back(predicate);
            continue;
        }
        TEquiJoinCondition condition(predicate);
        const bool forward = condition.LeftIUs.IsSubsetOf(left) && condition.RightIUs.IsSubsetOf(right);
        const bool reverse = condition.LeftIUs.IsSubsetOf(right) && condition.RightIUs.IsSubsetOf(left);
        if (!forward && !reverse) {
            remaining.push_back(predicate);
            continue;
        }
        TMappedIUs<TExpression> definitions;
        const auto extracted = condition.ExtractExpressions(definitions, ctx);
        const TEquiJoinCondition keys(extracted ? *extracted : predicate);
        join.JoinKeys.Add(forward ? keys.GetLeftIU() : keys.GetRightIU(), forward ? keys.GetRightIU() : keys.GetLeftIU());
        for (const auto& [id, value] : definitions.Items()) {
            auto& destination = value.GetInputIUs(false, true).IsSubsetOf(left) ? leftDefinitions : rightDefinitions;
            destination.Add(id, value);
        }
        changed = true;
    }
    if (!changed) return false;
    if (join.JoinKind == "Cross") join.JoinKind = "Inner";
    if (!leftDefinitions.Keys().Empty())
        join.SetLeftInput(MakeIntrusive<TOpMap>(join.GetLeftInput(), join.Pos, std::move(leftDefinitions), true));
    if (!rightDefinitions.Keys().Empty())
        join.SetRightInput(MakeIntrusive<TOpMap>(join.GetRightInput(), join.Pos, std::move(rightDefinitions), true));
    join.JoinFilters = std::move(remaining);
    return true;
}

} // namespace

bool TExtractJoinExpressionsRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    if (input->Kind == EOperator::Join) {
        const auto& filters = CastOperator<TOpJoin>(*input).JoinFilters;
        return !filters.empty() && filters.front().IsNative();
    }
    return input->Kind == EOperator::Filter && CastOperator<TOpFilter>(input)->GetInput()->Kind == EOperator::Join;
}

bool TExtractJoinExpressionsRule::MatchAndApply(TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    if (input->Kind == EOperator::Join) return ExtractNativeJoinKeys(CastOperator<TOpJoin>(*input), ctx, props);
    if (input->Kind != EOperator::Filter) {
        return false;
    }

    auto filter = CastOperator<TOpFilter>(input);
    if (filter->GetInput()->Kind != EOperator::Join) {
        return false;
    }

    auto join = CastOperator<TOpJoin>(filter->GetInput());
    auto leftInput = join->GetLeftInput();
    auto rightInput = join->GetRightInput();

    auto conjuncts = filter->GetFilterExpression().SplitConjunct();

    TVector<TExpression> newConjuncts;
    TMapIUs mapElements;

    for (auto & c : conjuncts) {
        if (c.MaybeEquiJoinCondition()) {
            newConjuncts.push_back(c);
        }
        else if (c.MaybeExprEquiJoinCondition()) {
            // Check that the condition won't be pushed to either side of the join
            auto exprIUs = c.GetInputIUs();
            if (exprIUs.IsSubsetOf(leftInput->GetOutputIUs()) || exprIUs.IsSubsetOf(rightInput->GetOutputIUs())){
                newConjuncts.push_back(c);
                continue;
            }

            TEquiJoinCondition cond(c);
            TMappedIUs<TExpression> definitions;
            if (auto rewritten = cond.ExtractExpressions(definitions, ctx)) {
                for (const auto& [iu, expr] : definitions.Items()) {
                    mapElements.Add(iu, expr);
                }
                newConjuncts.push_back(std::move(*rewritten));
            } else {
                newConjuncts.push_back(c);
            }
        } else {
            newConjuncts.push_back(c);
        }
    }

    if (mapElements.Keys().Empty()) {
        return false;
    }

    filter->SetFilterExpression(MakeConjunction(newConjuncts, props.PgSyntax));
    auto newMap = MakeIntrusive<TOpMap>(join, input->Pos, std::move(mapElements), true);
    filter->SetInput(std::move(newMap));
    return true;
}
}
}
