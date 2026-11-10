#pragma once

#include <yql/essentials/ast/yql_expr.h>

namespace NYql {
struct TTypeAnnotationContext;
}

namespace NKikimr::NKqp {

// Normalize physical adapters/projections and select block sort kernels under
// the existing execution policy. General scalar computations keep their scope.
// The input is annotated; newly constructed nodes require backend annotation.
// Optional stage-input substitutions preserve their unpacked logical types.
NYql::TExprNode::TPtr LowerPhysicalLayouts(const NYql::TExprNode::TPtr& root, NYql::TExprContext& ctx,
    const NYql::TTypeAnnotationContext& types, const NYql::TNodeOnNodeOwnedMap& inputs = {});

} // namespace NKikimr::NKqp
