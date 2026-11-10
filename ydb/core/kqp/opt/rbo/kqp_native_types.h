#pragma once

#include "frontend/expression.h"
#include "kqp_info_unit.h"

#include <library/cpp/containers/absl/flat_hash_map.h>

namespace NKikimr::NKqp {

class IOperator;
struct TPlanProps;
struct TOpAggregationTraits;
enum class EOpPhase;

using TNativeRow = absl::flat_hash_map<TInfoUnitId, NFrontend::TType>;

struct TNativeSchema {
    TNativeRow Row;
    // Stream lookup joins carry (left row, optional fetched row, cookie).
    std::optional<TNativeRow> LookupLeft;
    std::shared_ptr<NFrontend::TExpressionArena> Arena;
};

// Native relational typing has no expression context or backend annotations.
void ComputeNativeTypes(IOperator& op, TPlanProps& props);
NFrontend::TType NativeAggregateType(NFrontend::TType input, const TOpAggregationTraits& traits,
    EOpPhase phase, NFrontend::TExpressionArena& arena);

} // namespace NKikimr::NKqp
