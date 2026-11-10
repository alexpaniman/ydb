#pragma once

#include "kqp_operator.h"

namespace NKikimr::NKqp {

struct TNativeTable;

// Construct statement semantics before optimization. The returned input keeps
// the original bindings and is empty when any insert constraint conflicts.
TIntrusivePtr<IOperator> PrepareInsertRevert(TIntrusivePtr<IOperator> input,
    const TOrderedIUs<TString>& columns, const TNativeTable& table,
    TPositionHandle pos, TPlanProps& props);

} // namespace NKikimr::NKqp
