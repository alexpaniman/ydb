#pragma once

#include <yql/essentials/core/yql_type_annotation.h>

namespace NKikimr::NKqp {

// This reserved execution name cannot collide with SQL $parameters.
inline constexpr TStringBuf QueryRandomUuidParameter = "%kqp%random_uuid";

bool NeedsRboCompatibilityLowering(const NYql::TExprNode::TPtr& root);

NYql::TExprNode::TPtr RewriteRboCompatibilityNode(
    const NYql::TExprNode::TPtr& node,
    NYql::TExprContext& ctx,
    const NYql::TTypeAnnotationContext& types);

void EnsureRboCompatibilityLowered(const NYql::TExprNode::TPtr& root);

} // namespace NKikimr::NKqp
