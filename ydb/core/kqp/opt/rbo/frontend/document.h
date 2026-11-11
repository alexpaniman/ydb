#pragma once

#include "expression.h"

namespace NKikimr::NKqp::NFrontend {

// Validate literal bytes without building a document tree or a backend graph.
// Runtime column/parameter values are already validated by their input boundary.
void ValidateDocumentLiteral(ETypeKind kind, std::string_view text, TSourcePosition position);

} // namespace NKikimr::NKqp::NFrontend
