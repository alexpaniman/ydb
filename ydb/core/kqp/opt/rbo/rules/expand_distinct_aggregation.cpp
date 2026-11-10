#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

namespace NKikimr::NKqp {

namespace {
using namespace NYql::NNodes;

bool IsSuitableToExpandDistinctAggregation(const TIntrusivePtr<IOperator>& input) {
    if (input->GetKind() != EOperator::Aggregate) {
        return false;
    }

    const auto aggTraitsList = CastOperator<TOpAggregate>(input)->GetAggregationTraits().Items() | std::views::values;
    return std::any_of(aggTraitsList.begin(), aggTraitsList.end(), [](const TOpAggregationTraits& aggTraits) { return aggTraits.Distinct; });
}

std::pair<TString, TString> GetAggFunctions(const TString& aggFunc) {
    if (aggFunc == "min" || aggFunc == "max" || aggFunc == "sum" || aggFunc == "avg" || aggFunc == "variance_1_1" || aggFunc == "some") {
        return std::make_pair(aggFunc, aggFunc);
    }
    if (aggFunc == "count") {
        return std::make_pair("count", "sum");
    }
    Y_ENSURE(false, "Aggregation function is not supported for splitting.");
}

TIntrusivePtr<IOperator> ExpandSingleDistinct(const TIntrusivePtr<TOpAggregate>& aggregate) {
    const auto& [output, aggTraits] = *aggregate->GetAggregationTraits().Items().begin();
    TOrderedIUs<> distinctKeys = aggregate->GetKeyColumns();
    const auto pos = aggregate->Pos;

    // Split into distinct and original aggregation.
    // Group without traits: identity DISTINCT traits would redefine the input IDs.
    distinctKeys.AppendMissing(aggTraits.Input);

    const TIntrusivePtr<IOperator> distinctAggregation =
        MakeIntrusive<TOpAggregate>(aggregate->GetInput(), TAggregationIUs{}, distinctKeys, EOpPhase::Undefined,
                                    /*distinctAll=*/false, pos);
    TOpAggregationTraits aggregationTraits = aggTraits;
    aggregationTraits.Distinct = false;
    const TAggregationIUs newAggTraitsList{{output, aggregationTraits}};
    return MakeIntrusive<TOpAggregate>(distinctAggregation, newAggTraitsList, aggregate->GetKeyColumns(), EOpPhase::Undefined, /*distinctAll=*/false, pos);
}

TIntrusivePtr<IOperator> BuildDistinct(const TIntrusivePtr<IOperator>& input, TOrderedIUs<>&& distColumns) {
    // Group without traits: identity DISTINCT traits would redefine the input IDs.
    return MakeIntrusive<TOpAggregate>(input, TAggregationIUs{}, std::move(distColumns), EOpPhase::Undefined, /*distinctAll=*/false, input->Pos);
}





// realAggTraits maps results to partial IDs; mapColumns gets each result's branch column.
TIntrusivePtr<IOperator> BuildMapWithNullElements(const TIntrusivePtr<IOperator>& input, const IOperator& originalInput,
                                                  const TAggregationIUs& aggTraitsList, const TMappedIUs<TInfoUnitId>& realAggTraits,
                                                  TMappedIUs<TInfoUnitId>& mapColumns, TPlanProps& props, TExprContext& ctx) {
    Y_ENSURE(originalInput.HasSchema());
    Y_UNUSED(ctx);

    TMapIUs mapElements;
    for (const auto output : aggTraitsList.Keys()) {
        const auto& aggTraits = aggTraitsList.At(output);

            // Preserve the existing partial/union/final plan, including
            // typed tuple states for AVG.
            const auto& arena = props.NativeExpressions;
            const auto fieldType = originalInput.GetNativeIUType(aggTraits.Input);
            const auto state = NativeAggregateType(fieldType.Required(), aggTraits, EOpPhase::Intermediate, *arena);
            NFrontend::TExprId expression;
            if (const auto* partial = realAggTraits.Find(output)) {
                const bool optional = fieldType.OptionalDepth && aggTraits.AggFunction != "count";
                if (optional || state.OptionalDepth) {
                    mapColumns.Add(output, *partial);
                    continue;
                }
                expression = arena->Column(*partial, state);
                expression = arena->Convert(expression, state.Optional());
            } else {
                expression = arena->Literal(state.Optional(), std::monostate{});
            }
            const auto padding = props.InfoUnitRegistry.AddGenerated("distinct_padding");
            mapElements.Add(padding, TExpression::FromNative(arena, expression, &props, input->Pos));
            mapColumns.Add(output, padding);
            continue;

    }

    if (mapElements.Keys().Empty()) {
        return input;
    }

    return MakeIntrusive<TOpMap>(input, input->Pos, std::move(mapElements));
}



TIntrusivePtr<IOperator> ExpandMultiDistinct(const TIntrusivePtr<TOpAggregate>& aggregate, TPlanProps& props, TExprContext& ctx) {
    const auto& aggTraitsList = aggregate->GetAggregationTraits();
    const auto pos = aggregate->Pos;
    auto& registry = props.InfoUnitRegistry;

    // The union defines fresh IDs for the keys and the intermediate columns;
    // every partial aggregation adds its own input ID to each union row.
    TSubstitutions keyReplacements;
    TOrderedIUs<> finalKeys;
    TMappedIUs<TUnionInputRow> rows;
    for (const auto key : aggregate->GetKeyColumns().Items()) {
        if (!keyReplacements.Keys().Contains(key)) {
            const auto output = registry.AddCopy(key);
            keyReplacements.Add(key, output);
            rows.Add(output);
        }
        finalKeys.Append(keyReplacements.At(key));
    }
    TMappedIUs<TInfoUnitId> intermediates;
    for (const auto output : aggTraitsList.Keys()) {
        const auto intermediate = registry.AddGenerated("distinct_union");
        intermediates.Add(output, intermediate);
        rows.Add(intermediate);
    }

    // The input is consumed once per partial aggregation, each through its own
    // Replicate port. Non-primary ports expose fresh IDs.
    const auto hub = TReplicate::Create(aggregate->GetInput(), pos, registry);
    TVector<TIntrusivePtr<IOperator>> unionAllInputs;
    const auto addPartialResult = [&](const TUnorderedIUs& outputs, bool isDistinct) {
        const auto port = hub->AddOutput();
        const auto* rebindings = port->IsPrimary() ? nullptr : &port->GetRebindings();
        const auto rebind = [&](TInfoUnitId id) { return rebindings ? *rebindings->Find(id) : id; };
        TOrderedIUs<> keyColumns;
        for (const auto key : aggregate->GetKeyColumns().Items()) {
            keyColumns.Append(rebind(key));
        }

        TIntrusivePtr<IOperator> partialResult = port;
        if (isDistinct) {
            // Aggregation column + keys.
            TOrderedIUs<> distColumns = keyColumns;
            distColumns.AppendMissing(rebind(aggTraitsList.At(*outputs.begin()).Input));
            partialResult = BuildDistinct(partialResult, std::move(distColumns));
        }

        TAggregationIUs partialAggregationTraitsList;
        TMappedIUs<TInfoUnitId> partialColumns;
        for (const auto output : outputs) {
            const auto& aggTraits = aggTraitsList.At(output);
            const auto partialColName = registry.AddGenerated("distinct_partial");
            partialAggregationTraitsList.Add(partialColName, TOpAggregationTraits{rebind(aggTraits.Input), GetAggFunctions(aggTraits.AggFunction).first, false, false, aggTraits.ResultType});
            partialColumns.Add(output, partialColName);
        }
        partialResult = MakeIntrusive<TOpAggregate>(partialResult, partialAggregationTraitsList, keyColumns, EOpPhase::Intermediate,
                                                    /*distinctAll=*/false, pos);
        TMappedIUs<TInfoUnitId> mapColumns;
        partialResult =
            BuildMapWithNullElements(partialResult, *aggregate->GetInput(), aggTraitsList, partialColumns, mapColumns, props, ctx);

        for (const auto& [key, output] : keyReplacements.Items()) {
            rows.At(output).Inputs.push_back(rebind(key));
        }
        for (const auto& [output, intermediate] : intermediates.Items()) {
            rows.At(intermediate).Inputs.push_back(mapColumns.At(output));
        }
        unionAllInputs.push_back(partialResult);
    };

    TAggregationIUs finalAggTraitsList;
    TMapIUs nativeResults;
    TUnorderedIUs aggTraitsListNotDistinct;

    for (const auto output : aggTraitsList.Keys()) {
        const auto& aggTraits = aggTraitsList.At(output);
        const bool isDistinct = aggTraits.Distinct;
        const auto aggFunctions = GetAggFunctions(aggTraits.AggFunction);
        const auto intermediateColName = intermediates.At(output);
        auto finalAggTraits = TOpAggregationTraits{
            intermediateColName, aggFunctions.second, false,
            !aggregate->GetNativeIUType(output).OptionalDepth,
            aggTraits.ResultType};
        auto finalOutput = output;
        if (props.NativeExpressions && finalAggTraits.Unwrap) {
            // Native result types must be correct before terminal lowering.
            // Keep nullable union states in the aggregate and restore required
            // results with executable expressions, rather than a backend flag.
            const auto type = aggregate->GetNativeIUType(output).Required();
            const auto& arena = props.NativeExpressions;
            finalOutput = registry.AddGenerated("distinct_result");
            auto expression = arena->Column(finalOutput, type.Optional());
            if (aggTraits.AggFunction == "count") {
                const NFrontend::TExprId args[]{expression, arena->Literal(type, std::uint64_t{0})};
                expression = arena->Coalesce(args);
            } else {
                expression = arena->Unary(NFrontend::EOpcode::Unwrap, expression);
            }
            nativeResults.Add(output, TExpression::FromNative(arena, expression, &props, pos));
            finalAggTraits.Unwrap = false;
            if (finalAggTraits.ResultType) finalAggTraits.ResultType = finalAggTraits.ResultType->Optional();
        }
        finalAggTraitsList.Add(finalOutput, finalAggTraits);
        if (!isDistinct) {
            // For not distinct we keep traits and will put them in one aggregation.
            aggTraitsListNotDistinct.Add(output);
        } else {
            addPartialResult(TUnorderedIUs{output}, /*isDistinct=*/true);
        }
    }

    if (!aggTraitsListNotDistinct.Empty()) {
        addPartialResult(aggTraitsListNotDistinct, /*isDistinct=*/false);
    }

    TUnionAllIUs columns(TUnionInputPolicy{unionAllInputs.size()});
    for (const auto output : rows.Keys()) {
        columns.Add(output, std::move(rows.At(output)));
    }
    const TIntrusivePtr<IOperator> unionAllResult = MakeIntrusive<TOpUnionAll>(std::move(unionAllInputs), aggregate->Pos, std::move(columns));
    RebindConsumers(*aggregate, keyReplacements, props.Subplans);

    TIntrusivePtr<IOperator> result = MakeIntrusive<TOpAggregate>(unionAllResult, finalAggTraitsList, finalKeys, EOpPhase::Final, /*distinctAll=*/false, pos);
    if (!nativeResults.Keys().Empty()) result = MakeIntrusive<TOpMap>(result, pos, std::move(nativeResults));
    return result;
}

} // anonymous namespace

bool TExpandDistinctAggregationRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return IsSuitableToExpandDistinctAggregation(input);
}

TIntrusivePtr<IOperator> TExpandDistinctAggregationRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& rboCtx, TPlanProps& props) {
    if (!IsSuitableToExpandDistinctAggregation(input)) {
        return input;
    }

    const auto aggregate = CastOperator<TOpAggregate>(input);
    if (aggregate->GetAggregationTraits().Items().size() == 1) {
        // Fast path.
        return ExpandSingleDistinct(aggregate);
    }
    return ExpandMultiDistinct(aggregate, props, rboCtx.ExprCtx);
}

} // namespace NKikimr::NKqp
