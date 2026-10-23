#pragma once

#include "kqp_info_unit.h"
#include "kqp_rbo_context.h"
#include "kqp_plan_props.h"

#include <algorithm>

namespace NKikimr {
namespace NKqp {

using namespace NYql;

class IOperator;
class TOpAggregate;
class TOpWindow;
class TExpression;

bool ReferencesUnresolvedSubplan(const TExpression& expr, const TPlanProps& props);
TOrderedIUs<> GetAggregatePreservedShuffling(const TOpAggregate& aggregate, const TRBOContext& ctx);
bool CanEliminateAggregateShuffle(const TOpAggregate& aggregate, const TRBOContext& ctx);
// The actual ordered hash key, translated to window partition IUs, or no proof.
TOrderedIUs<> GetWindowPreservedShuffling(const TOpWindow& window, const TRBOContext& ctx);

bool JoinOutputsLeft(const TString& joinKind);
bool JoinOutputsRight(const TString& joinKind);
TString GetValidJoinKind(const TString& joinKind);

bool SortMatchesKeyOrder(const TVector<TString>& sortColumns, const TVector<TString>& keyColumns, size_t pointPrefixLen);

}
}
