#pragma once

#include "kqp_operator.h"

namespace NKikimr::NKqp {

// Construct the existing mark-join representation of an IN/EXISTS value.
// The result preserves input columns and defines subplanIU; its consumed entry
// is removed from props.Subplans. Inputs and subplans must have native schemas.
// Shared by frontend value construction and the filter-only RBO rule.
TIntrusivePtr<IOperator> BuildInExistsValue(const TIntrusivePtr<IOperator>& input,
    TInfoUnitId subplanIU, TPositionHandle pos, TPlanProps& props, bool nativeEqualNulls);

} // namespace NKikimr::NKqp
