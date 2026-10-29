#pragma once

#include "frontend/expression.h"

#include <yql/essentials/ast/yql_expr.h>

namespace NKikimr::NKqp {

// The execution compatibility boundary. The native frontend and its type
// checker never include this header or depend on YQL. Each native operation
// has a finite, explicit lowering; no SQL/YQL normalization is performed here.
const NYql::TTypeAnnotationNode* ExportNativeType(NFrontend::TType type, NYql::TExprContext& ctx);
NFrontend::TType ImportNativeType(const NYql::TTypeAnnotationNode& type);
NYql::TExprNode::TPtr LowerNativeLambda(const NFrontend::TExpressionArena& arena,
    NFrontend::TExprId root, NYql::TExprContext& ctx, NYql::TPositionHandle pos);

} // namespace NKikimr::NKqp
