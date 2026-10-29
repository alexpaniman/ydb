#pragma once

#include "kqp_expression.h"

namespace NKikimr {
namespace NKqp {

// Render an expression for explain output in YQL-like syntax. Every node is printed,
// unknown callables as plain calls, and parentheses are added wherever the text could
// be read as a different tree. Subtrees past the size budget are printed as f(a, b),
// a function of the columns and free lambda arguments they depend on.
TString PrintExplainExpression(const TExpression& expr, const TInfoUnitRegistry& registry);

}
}
