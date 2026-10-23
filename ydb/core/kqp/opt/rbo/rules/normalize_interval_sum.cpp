#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

namespace NKikimr::NKqp {

bool TNormalizeIntervalSumRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    if (input->GetKind() != EOperator::Aggregate) {
        return false;
    }
    const auto& aggregate = CastOperator<TOpAggregate>(*input);
    if (aggregate.GetAggregationPhase() != EOpPhase::Undefined || aggregate.IsDistinctAll()) {
        return false;
    }
    const auto traits = aggregate.GetAggregationTraits().Items() | std::views::values;
    return std::any_of(traits.begin(), traits.end(), [](const auto& trait) { return trait.AggFunction == "sum"; });
}

TIntrusivePtr<IOperator> TNormalizeIntervalSumRule::SimpleMatchAndApply(
    const TIntrusivePtr<IOperator>& input, TRBOContext& rboCtx, TPlanProps& props) {
    if (!QuickMatch(input)) {
        return input;
    }
    const auto aggregate = CastOperator<TOpAggregate>(input);
    auto& ctx = rboCtx.ExprCtx;
    const auto pos = aggregate->Pos;
    const auto* intType = ctx.MakeType<TDataExprType>(EDataSlot::Int64);
    const auto* decimalType = ctx.MakeType<TDataExprParamsType>(EDataSlot::Decimal, "35", "0");

    // Match the interval SUM traits: accumulate exactly in Decimal and check
    // the interval range only after every partial state has been merged.
    const auto castThroughInt64 = [&](TInfoUnitId source, const TTypeAnnotationNode& target) {
        auto arg = ctx.NewArgument(pos, "interval_sum");
        auto value = ctx.NewCallable(pos, "Member", {arg, ctx.NewAtom(pos, ctx.GetIndexAsString(source))});
        value = ctx.NewCallable(pos, "StrictCast", {std::move(value), ExpandType(pos, *intType, ctx)});
        value = ctx.NewCallable(pos, "StrictCast", {std::move(value), ExpandType(pos, target, ctx)});
        return TExpression(ctx.NewLambda(pos, ctx.NewArguments(pos, {arg}), std::move(value)), &ctx, &props);
    };

    TMappedIUs<TInfoUnitId> convertedInputs;
    TMapIUs inputs;
    TMapIUs results;
    TAggregationIUs traits;
    for (const auto& [output, original] : aggregate->GetAggregationTraits().Items()) {
        const auto* type = aggregate->GetInput()->GetIUType(original.Input, ctx);
        const auto& valueType = RemoveOptionality(*type);
        if (original.AggFunction != "sum" || valueType.GetKind() != ETypeAnnotationKind::Data
            || !IsDataTypeInterval(valueType.Cast<TDataExprType>()->GetSlot())) {
            traits.Add(output, original);
            continue;
        }

        auto normalized = original;
        if (const auto* existing = convertedInputs.Find(original.Input)) {
            normalized.Input = *existing;
        } else {
            normalized.Input = props.InfoUnitRegistry.AddGenerated("interval_sum_input");
            inputs.Add(normalized.Input, castThroughInt64(original.Input, *decimalType));
            convertedInputs.Add(original.Input, normalized.Input);
        }
        const auto state = props.InfoUnitRegistry.AddCopy(output);
        traits.Add(state, normalized);
        // Keep the original result binding and nullable overflow behavior.
        results.Add(output, castThroughInt64(state, valueType));
    }
    if (results.Keys().Empty()) {
        return input;
    }

    TIntrusivePtr<IOperator> result = MakeIntrusive<TOpMap>(aggregate->GetInput(), pos, std::move(inputs));
    result = MakeIntrusive<TOpAggregate>(result, std::move(traits), aggregate->GetKeyColumns(),
        EOpPhase::Undefined, /*distinctAll=*/false, pos);
    return MakeIntrusive<TOpMap>(result, pos, std::move(results));
}

} // namespace NKikimr::NKqp
