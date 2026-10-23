#include "kqp_rules_include.h"

#include "decorrelation/dependent_join_pushdown.h"
#include <ydb/core/kqp/opt/rbo/copy_logical_subtree.h>

namespace {

using namespace NKikimr::NKqp;

// Grouping equality and StablePickle agree for these scalar values. In
// particular, do not merge complete floating rows by canonicalized zero/NaN.
bool HasExactScalarIdentity(const TTypeAnnotationNode* type) {
    if (!type) {
        return false;
    }
    if (type->GetKind() == ETypeAnnotationKind::Optional) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    if (type->GetKind() != ETypeAnnotationKind::Data) {
        return false;
    }
    switch (type->Cast<TDataExprType>()->GetSlot()) {
        case NUdf::EDataSlot::Bool:
        case NUdf::EDataSlot::Int8: case NUdf::EDataSlot::Uint8:
        case NUdf::EDataSlot::Int16: case NUdf::EDataSlot::Uint16:
        case NUdf::EDataSlot::Int32: case NUdf::EDataSlot::Uint32:
        case NUdf::EDataSlot::Int64: case NUdf::EDataSlot::Uint64:
        case NUdf::EDataSlot::Date: case NUdf::EDataSlot::Datetime:
        case NUdf::EDataSlot::Timestamp: case NUdf::EDataSlot::Interval:
        case NUdf::EDataSlot::Date32: case NUdf::EDataSlot::Datetime64:
        case NUdf::EDataSlot::Timestamp64: case NUdf::EDataSlot::Interval64:
        case NUdf::EDataSlot::String: case NUdf::EDataSlot::Utf8:
        case NUdf::EDataSlot::Decimal:
            return true;
        default:
            return false;
    }
}

// The general copy policy permits reductions whose result can depend on input
// order. Reconstructing a left row requires the copied producer's values to
// agree, including values hidden behind later casts or referenced subplans.
bool HasStableJoinFilterReductions(const IOperator& root, const TSubplans& subplans, TExprContext& ctx) {
    THashSet<const IOperator*> visited;
    TVector<const IOperator*> pending{&root};
    while (!pending.empty()) {
        const auto* op = pending.back();
        pending.pop_back();
        if (!visited.insert(op).second) {
            continue;
        }
        if (op->Kind == EOperator::Aggregate) {
            const auto& aggregate = CastOperator<TOpAggregate>(*op);
            const auto* input = aggregate.GetInput().Get();
            if (!input->Type) {
                return false;
            }
            for (const auto key : aggregate.GetKeyColumns().Items()) {
                // A grouping key can expose an order-dependent representative
                // even when every reduction is COUNT.
                if (!HasExactScalarIdentity(input->GetIUType(key, ctx))) {
                    return false;
                }
            }
            for (const auto& [output, traits] : aggregate.GetAggregationTraits().Items()) {
                Y_UNUSED(output);
                if (traits.AggFunction == "count") {
                    continue;
                }
                const auto* type = input->GetIUType(traits.Input, ctx);
                if (!type) {
                    return false;
                }
                if (traits.AggFunction == "sum") {
                    const auto& valueType = RemoveOptionality(*type);
                    if (valueType.GetKind() != ETypeAnnotationKind::Data
                        || !IsDataTypeIntegral(valueType.Cast<TDataExprType>()->GetSlot())) {
                        return false;
                    }
                } else if ((traits.AggFunction != "min" && traits.AggFunction != "max" && traits.AggFunction != "distinct")
                    || !HasExactScalarIdentity(type)) {
                    return false;
                }
            }
        }
        for (const auto* child : op->GetChildren()) {
            pending.push_back(child);
        }
        for (const auto call : op->GetSubplanIUs(subplans)) {
            pending.push_back(subplans.At(call).Plan.Get());
        }
    }
    return true;
}

}

namespace NKikimr {
namespace NKqp {

bool TInlineJoinFiltersRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Join;
}

// Inline join filters. Temporarily inline join filters only of there are no equi-join conditions in the join

TIntrusivePtr<IOperator> TInlineJoinFiltersRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    if (input->Kind != EOperator::Join) {
        return input;
    }

    auto join = CastOperator<TOpJoin>(input);
    if (join->JoinFilters.empty()) {
        return input;
    }

    // Inner with empty keys - cross.
    const bool isRealCrossJoin = join->JoinKind == "Cross" || (join->JoinKind == "Inner" && join->JoinKeys.Items().empty());
    const bool usingBlockJoin = ctx.KqpCtx.Config->GetUseBlockHashJoin();
    const bool usingBlockCrossJoin = usingBlockJoin && ctx.KqpCtx.Config->GetUseBlockHashJoinForCross();
    const bool forceInlining = ctx.KqpCtx.Config->GetEnableInlineJoinFiltersAfterCBO();

    // Do not inline filters for cross join, unless we force the inlining with a flag
    if (isRealCrossJoin && usingBlockCrossJoin && !forceInlining) {
        join->JoinKind = "Cross";
        return join;
    }

    // We inline join filters in the following cases:
    // - There implementation is a lookup join or reverse lookup join
    // - There are no equi-join conditions in the join
    // - We're not using BlockJoin, which supports join filters

    // Lookup join is not supported for join filters.
    const bool isLookupJoin = join->Props.JoinAlgo == EJoinAlgoType::LookupJoin || join->Props.JoinAlgo == EJoinAlgoType::LookupJoinReverse;
    bool containsEquiJoinConditions = !join->JoinKeys.Items().empty();
    for (const auto& f : join->JoinFilters) {
        if (f.MaybeEquiJoinCondition()) {
            containsEquiJoinConditions = true;
        }
    }

    if (!isRealCrossJoin && usingBlockJoin && !isLookupJoin && containsEquiJoinConditions && !forceInlining) {
        return input;
    }

    // In case of inner or cross join, we push the join filters above the join
    if (join->JoinKind == "Inner" || join->JoinKind == "Cross") {
        auto filterExpr = MakeConjunction(join->JoinFilters);
        auto newFilter = MakeIntrusive<TOpFilter>(join, input->Pos, filterExpr);

        join->JoinFilters = {};

        // Now that we pushed the filters out of the join, the join might turn into a cross-join
        if (join->JoinKeys.Items().empty()) {
            join->JoinKind = "Cross";
        }

        return newFilter;
    }

    // We only support various left joins now
    if (join->JoinKind != "Left" && join->JoinKind != "LeftSemi" && join->JoinKind != "LeftOnly") {
        Y_ENSURE(false, TStringBuilder() << "Join filter in unsupported join type: " << join->JoinKind);
        return input;
    }

    auto leftInput = join->GetLeftInput();
    // Keep the same sharing decision as the optimizer and physical compiler.
    const bool allowChannelSpilling = ctx.KqpCtx.Config->GetEnableQueryServiceSpilling()
        && (ctx.KqpCtx.IsGenericQuery() || ctx.KqpCtx.IsScanQuery()) && ctx.KqpCtx.Config->SpillingEnabled();
    if (!allowChannelSpilling) {
        Y_ENSURE(HasStableJoinFilterReductions(*leftInput, props.Subplans, ctx.ExprCtx),
            "Join filter reconstruction requires stable copied values");
    }
    auto keyColumns = leftInput->Props.Metadata->KeyColumns;
    const bool needsDomain = keyColumns.Items().empty();
    TUnorderedIUs nullableKeys;
    if (needsDomain) {
        // One inner evaluation per complete left value, then reconstruct
        // against the original bag. Volatile predicates need occurrence IDs.
        Y_ENSURE(CanDuplicateOperator(*join), "Join filter domain requires repeatable predicates");
        for (const auto call : join->GetSubplanIUs(props.Subplans)) {
            Y_ENSURE(CanDuplicateSubtree(*props.Subplans.At(call).Plan, props.Subplans),
                "Join filter domain requires repeatable subplans");
            Y_ENSURE(HasStableJoinFilterReductions(*props.Subplans.At(call).Plan, props.Subplans, ctx.ExprCtx),
                "Join filter domain requires stable subplan values");
        }
        const auto& columns = leftInput->GetOutputIUs();
        keyColumns = TOrderedIUs<>(columns.begin(), columns.end());
    }
    for (const auto column : keyColumns.Items()) {
        const auto* type = leftInput->GetIUType(column, ctx.ExprCtx);
        Y_ENSURE(type, "Missing join filter identity type");
        if (needsDomain || type->IsOptionalOrNull()) {
            Y_ENSURE(HasExactScalarIdentity(type), "Unsupported join filter identity type");
        }
        if (type->IsOptionalOrNull()) {
            nullableKeys.Add(column);
        }
    }
    if (keyColumns.Items().empty()) {
        // A zero-column bag still has multiplicity; use a one-value domain.
        const auto unit = props.InfoUnitRegistry.AddGenerated("join_filter_unit");
        TMapIUs elements;
        elements.Add(unit, MakeConstant("Uint64", "0", join->Pos, &ctx.ExprCtx));
        leftInput = MakeIntrusive<TOpMap>(leftInput, join->Pos, std::move(elements));
        keyColumns.Append(unit);
    }

    // The inner join reads the left input again: a Replicate port gives it its own IDs,
    // so we can join on the same columns again without conflicts
    auto hub = TReplicate::Create(leftInput, join->Pos, props.InfoUnitRegistry);
    TIntrusivePtr<IOperator> outerLeftInput = hub->AddOutput();
    auto innerPort = hub->AddOutput();
    const auto renameMap = innerPort->GetRebindings();
    TIntrusivePtr<IOperator> innerLeftInput = innerPort;
    if (needsDomain) {
        TUnorderedIUs columns;
        for (const auto column : keyColumns.Items()) {
            columns.Add(renameMap.At(column));
        }
        innerLeftInput = MakeDomainProjection(innerLeftInput, columns, join->Pos);
    }

    // Build an inner join
    const auto joinKind = join->JoinKeys.Items().empty() ? "Cross" : "Inner";
    TJoinIUs innerJoinKeys;
    for (const auto& [leftKey, rightKey, equalNulls] : join->JoinKeys.Items()) {
        innerJoinKeys.Add({renameMap.At(leftKey), rightKey, equalNulls});
    }
    auto innerJoin = MakeIntrusive<TOpJoin>(innerLeftInput, join->GetRightInput(), join->Pos, joinKind, innerJoinKeys);
    auto filterExpr = MakeConjunction(join->JoinFilters).ApplyRenames(renameMap);

    TIntrusivePtr<IOperator> newFilter = MakeIntrusive<TOpFilter>(innerJoin, input->Pos, filterExpr);

    TJoinIUs newJoinKeys;
    TMapIUs outerKeys;
    TMapIUs innerKeys;
    auto encode = [&](TInfoUnitId column, TMapIUs& elements) {
        const auto encoded = props.InfoUnitRegistry.AddGenerated("join_filter_key");
        elements.Add(encoded, MakeUnaryCallable("StablePickle",
            MakeColumnAccess(column, join->Pos, &ctx.ExprCtx, &props)));
        return encoded;
    };
    for (const auto column : keyColumns.Items()) {
        if (nullableKeys.Contains(column)) {
            // String keys are nonoptional and compare NULL identity exactly,
            // independently of native EqualNulls support in the chosen join.
            newJoinKeys.Add(encode(column, outerKeys), encode(renameMap.At(column), innerKeys));
        } else {
            newJoinKeys.Add(column, renameMap.At(column));
        }
    }
    if (!outerKeys.Keys().Empty()) {
        outerLeftInput = MakeIntrusive<TOpMap>(outerLeftInput, join->Pos, std::move(outerKeys));
        newFilter = MakeIntrusive<TOpMap>(newFilter, join->Pos, std::move(innerKeys));
    }

    // Subplans called from the join filters now read the inner left input.
    for (const auto call : join->GetSubplanIUs(props.Subplans)) {
        props.Subplans.RebindInputs(call, renameMap);
    }

    auto result = MakeIntrusive<TOpJoin>(outerLeftInput, newFilter, join->Pos, join->JoinKind, newJoinKeys);

    return result;
}
}
}
