#pragma once

#include "expression.h"

namespace NKikimr::NKqp::NFrontend {

// Decode one backslash escape from the text immediately after the backslash.
// Both parsers use this for quoted values and identifiers. The caller owns
// delimiter handling and source advancement; the result is the bytes consumed.
std::size_t AppendQuotedEscape(std::string_view text, std::string& output, TSourcePosition position);

} // namespace NKikimr::NKqp::NFrontend
