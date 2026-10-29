#pragma once

#include "../parser.h"

namespace NKikimr::NKqp::NFrontend {

// Strict recognition of the complete shared SQL v1 grammar. Recovered parse
// trees are never used for compilation. Neither API invokes any YQL library.
void ValidateGrammar(std::string_view sql, const TParseSettings& settings = {});
TParsedQuery ParseGrammar(std::string_view sql, const TParseSettings& settings = {});

// Independent of parser allocation order and source positions; includes
// expression structure, statements, aliases, types and relational clauses.
std::string SyntaxFingerprint(const TParsedQuery& query);

} // namespace NKikimr::NKqp::NFrontend
