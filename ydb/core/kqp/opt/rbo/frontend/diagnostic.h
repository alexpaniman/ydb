#pragma once

#include "expression.h"

#include <string_view>

namespace NKikimr::NKqp::NFrontend {

// Only this exception permits automatic delegation to the legacy compiler.
// Invalid queries, resource limits and implementation defects are not fallback.
enum class EFeature {
    Statement, Syntax, Type, Function, Pragma, Subquery, JoinUsing, Window,
    GroupingSets, SetOperation, AggregateCombination, Expression, Backend,
    LanguageVersion,
};

inline std::string_view FeatureName(EFeature feature) {
    switch (feature) {
        case EFeature::Statement: return "statement";
        case EFeature::Syntax: return "syntax";
        case EFeature::Type: return "type";
        case EFeature::Function: return "function";
        case EFeature::Pragma: return "pragma";
        case EFeature::Subquery: return "subquery";
        case EFeature::JoinUsing: return "join_using";
        case EFeature::Window: return "window";
        case EFeature::GroupingSets: return "grouping_sets";
        case EFeature::SetOperation: return "set_operation";
        case EFeature::AggregateCombination: return "aggregate_combination";
        case EFeature::Expression: return "expression";
        case EFeature::Backend: return "backend";
        case EFeature::LanguageVersion: return "language_version";
    }
    throw std::logic_error("unknown native frontend feature");
}

class TUnsupportedFeature : public TFrontendError {
public:
    TUnsupportedFeature(EFeature feature, TSourcePosition position, std::string message)
        : TFrontendError(position, std::move(message)), Feature(feature) {}
    EFeature Feature;
};

class TResourceLimitError : public TFrontendError {
public:
    using TFrontendError::TFrontendError;
};

} // namespace NKikimr::NKqp::NFrontend
