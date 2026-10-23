#include "kqp_rbo_utils.h"
#include "kqp_operator.h"

namespace NKikimr {
namespace NKqp {

using namespace NYql;

bool ReferencesUnresolvedSubplan(const TExpression& expr, const TPlanProps& props) {
    return expr.GetRawInputIUs().HasAny(props.Subplans.Bindings());
}

bool JoinOutputsLeft(const TString& joinKind) {
    return joinKind != "RightOnly" && joinKind != "RightSemi";
}

bool JoinOutputsRight(const TString& joinKind) {
    return joinKind != "LeftOnly" && joinKind != "LeftSemi";
}

TString GetValidJoinKind(const TString& joinKind) {
    const auto joinKindLowered = to_lower(joinKind);
    if (joinKindLowered == "left") {
        return "Left";
    } else if (joinKindLowered == "inner") {
        return "Inner";
    } else if (joinKindLowered == "cross") {
        return "Cross";
    }
    return joinKind;
}

TOrderedIUs<> GetAggregatePreservedShuffling(const TOpAggregate& aggregate, const TRBOContext& ctx) {
    if (aggregate.GetKeyColumns().Items().empty()) {
        return {};
    }

    const bool enableShuffleElimination = ctx.KqpCtx.Config->OptShuffleElimination.Get()
        .GetOrElse(ctx.KqpCtx.Config->GetDefaultEnableShuffleElimination());
    if (!enableShuffleElimination) {
        return {};
    }

    const auto& input = *aggregate.GetInput();
    if (!input.Props.Metadata || input.Props.Metadata->ShuffledByColumns.Items().empty()) {
        return {};
    }

    // Example: input partitioned by {id} needs no reshuffle for GROUP BY {id, date},
    // because every group has a single id and is already colocated.
    const auto& shuffledBy = input.Props.Metadata->ShuffledByColumns;
    if (!shuffledBy.Unordered().IsSubsetOf(aggregate.GetKeyColumns().Unordered())) {
        return {};
    }

    if (!aggregate.IsDistinctAll()) {
        return shuffledBy;
    }

    // DISTINCT returns the trait results, not the grouping keys. Preserve the
    // hash key order while translating through the intermediate/final aliases.
    TOrderedIUs<> result;
    for (const auto key : shuffledBy.Items()) {
        bool found = false;
        for (const auto& [output, trait] : aggregate.GetAggregationTraits().Items()) {
            if (trait.Input == key && trait.AggFunction == "distinct") {
                result.Append(output);
                found = true;
                break;
            }
        }
        if (!found) {
            return {};
        }
    }
    return result;
}

bool CanEliminateAggregateShuffle(const TOpAggregate& aggregate, const TRBOContext& ctx) {
    return !GetAggregatePreservedShuffling(aggregate, ctx).Items().empty();
}

TOrderedIUs<> GetWindowPreservedShuffling(const TOpWindow& window, const TRBOContext& ctx) {
    const bool enabled = ctx.KqpCtx.Config->OptShuffleElimination.Get()
        .GetOrElse(ctx.KqpCtx.Config->GetDefaultEnableShuffleElimination());
    if (!enabled || window.GetPartitionKeys().Items().empty()) {
        return {};
    }

    const auto& keys = window.GetPartitionKeys().Items();
    TVector<std::optional<TInfoUnitId>> translated;
    translated.reserve(keys.size());
    for (const auto key : keys) {
        translated.emplace_back(key);
    }
    const auto* input = window.GetInput().Get();
    while (input->Kind == EOperator::Map || input->Kind == EOperator::Filter) {
        const auto* child = input->GetChild(0).Get();
        if (input->Props.StageId && child->Props.StageId && input->Props.StageId != child->Props.StageId) {
            return {};
        }
        if (input->Kind == EOperator::Map) {
            const auto& map = CastOperator<TOpMap>(*input);
            for (auto& key : translated) {
                if (!key) {
                    continue;
                }
                if (const auto* element = map.GetMapElements().Find(*key)) {
                    if (element->IsColumnAccess()) {
                        key = element->GetColumnAccess();
                    } else {
                        key.reset();
                    }
                }
            }
        }
        input = child;
    }

    // Only these producers supply a local proof: Window publishes its actual
    // chosen partition below, and a column read resolves storage hash keys.
    // In particular, never inherit an unproved join/aggregate distribution.
    const bool columnSource = input->Kind == EOperator::Source
        && CastOperator<TOpRead>(*input).GetTableStorageType() == NYql::EStorageType::ColumnStorage;
    if ((input->Kind != EOperator::Window && !columnSource) || !input->Props.Metadata) {
        return {};
    }
    const auto& shuffledBy = input->Props.Metadata->ShuffledByColumns;
    if (shuffledBy.Items().empty() || !shuffledBy.Unordered().IsSubsetOf(input->GetOutputIUs())) {
        return {};
    }

    TOrderedIUs<> preserved;
    for (const auto key : shuffledBy.Items()) {
        bool found = false;
        for (size_t i = 0; i < translated.size(); ++i) {
            if (translated[i] && *translated[i] == key) {
                preserved.Append(keys[i]);
                found = true;
                break;
            }
        }
        if (!found) {
            return {};
        }
    }
    // Extra partition keys are harmless, but retain the input's hash order,
    // multiplicity and types rather than claiming a hash of the full key set.
    return preserved;
}

bool SortMatchesKeyOrder(const TVector<TString>& sortColumns, const TVector<TString>& keyColumns, size_t pointPrefixLen) {
    if (sortColumns.empty() || pointPrefixLen > keyColumns.size()) {
        return false;
    }

    const THashSet<TString> pointKeys(keyColumns.begin(), keyColumns.begin() + pointPrefixLen);
    size_t next = pointPrefixLen;
    for (const auto& sortColumn : sortColumns) {
        if (sortColumn.empty()) {
            return false;
        }
        if (pointKeys.contains(sortColumn)) {
            continue;
        }
        if (next >= keyColumns.size() || keyColumns[next] != sortColumn) {
            return false;
        }
        ++next;
    }
    return true;
}

}
}
