#pragma once

#include "kqp_native_frontend.h"

#include <span>

namespace NKikimr::NKqp {

struct TNativeIndexRead {
    TIntrusivePtr<TOpRead> Read;
    TNativeIndex Index;
};

// Construct explicit SQL index views after the consumer column bindings are
// known. The catalog resolver has admitted the index; RBO sees ordinary reads
// and primary-key lookups with no new index-selection policy.
void PrepareNativeIndexReads(TOpRoot& root, std::span<const TNativeIndexRead> reads);

} // namespace NKikimr::NKqp
