#pragma once

#include "frontend/expression.h"
#include <util/generic/strbuf.h>

namespace NKikimr::NKqp {

// Storage capabilities consume native nodes and types only. The optimizer and
// emitter share this contract without constructing backend expressions.
bool CanLowerNativeOlapPredicate(const NFrontend::TExpressionArena& arena,
    NFrontend::TExprId root, bool allowScalarApply = false);
bool CanLowerNativeOlapProjection(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root);

namespace NDetail {
TStringBuf OlapBinaryName(NFrontend::EOpcode op);
bool OlapIdentityConversion(const NFrontend::TExpressionArena& arena, const NFrontend::TExprNode& node);
}

} // namespace NKikimr::NKqp
