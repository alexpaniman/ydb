#pragma once

#include "expression.h"

namespace NKikimr::NKqp::NFrontend::NDetail {

// Bind SQL LIKE, preserving literal simplifications and their result optionality.
// Dynamic/complex patterns use the runtime matching opcode.
TExprId BuildLike(TExpressionArena& arena, TExprId input, TExprId pattern,
    std::optional<TExprId> escape, TSourcePosition position);

} // namespace NKikimr::NKqp::NFrontend::NDetail
