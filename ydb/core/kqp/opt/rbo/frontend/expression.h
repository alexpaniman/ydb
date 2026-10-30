#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace NKikimr::NKqp::NFrontend {

using TExprId = std::uint32_t;
using TColumnId = std::uint32_t;

struct TSourcePosition {
    std::size_t Offset = 0;
    std::uint32_t Line = 1;
    std::uint32_t Column = 1;
};

class TFrontendError : public std::runtime_error {
public:
    TFrontendError(TSourcePosition position, std::string message);
    TSourcePosition Position;
};

// Optionality is one SQL NULL bit. Nested optionals and general YQL containers
// deliberately do not enter the scalar execution model.
enum class ETypeKind : std::uint8_t {
    Null, Bool, Int8, Int16, Int32, Int64, Uint8, Uint16, Uint32, Uint64,
    Float, Double, String, Utf8, Decimal, Date, Datetime, Timestamp, Interval,
};

struct TType {
    ETypeKind Kind = ETypeKind::Null;
    bool Nullable = false;
    std::uint8_t Precision = 0;
    std::uint8_t Scale = 0;

    bool operator==(const TType&) const = default;
    TType Optional() const;
    TType Required() const;
    bool IsIntegral() const;
    bool IsNumeric() const;
    bool IsString() const;
    bool IsSigned() const;
    std::string ToString() const;
};

TType ParseType(std::string_view name, TSourcePosition position = {});
TType CommonType(TType left, TType right, TSourcePosition position = {});

struct TDecimal {
    __int128 Coefficient = 0;
    bool operator==(const TDecimal&) const = default;
};

using TScalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, TDecimal>;

struct TValue {
    TType Type;
    TScalar Data;
    bool IsNull() const { return std::holds_alternative<std::monostate>(Data); }
};

// Every opcode is an execution operation. There are no unresolved calls,
// identifiers, lambda applications, type expressions, or optimizer macros here.
// Integral add/subtract/multiply/negate use two's-complement wrapping. Division
// and remainder return NULL for zero divisors and signed division overflow.
// Decimal scale reductions round to nearest, ties to even; finite precision
// overflow is an execution error, and division by zero still returns NULL.
enum class EOpcode : std::uint8_t {
    Literal, Column, Parameter, Convert, Cast,
    Negate, Not, Add, Subtract, Multiply, Divide, Modulo,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    And, Or, IsNull, IsNotNull, Coalesce, If,
    Abs, Length, Lower, Upper, Contains, StartsWith, EndsWith, Substring,
    Concat,
};

struct TExprNode {
    EOpcode Op;
    TType Type;
    TSourcePosition Position;
    std::vector<TExprId> Arguments;
    TScalar Value;
    std::string Name;
    TColumnId ColumnId = 0;
};

// Nodes are immutable after construction; all edges point to smaller IDs.
// Construction resolves types locally and inserts explicit casts. No annotation
// pass, fixed point, or whole-arena traversal is needed when appending a node.
// IDs remain valid after appends; references into Nodes() may be invalidated.
// A query may share this append-only arena, with mutation externally serialized.
class TExpressionArena {
public:
    TExprId Literal(TType type, TScalar value, TSourcePosition position = {});
    TExprId Null(TSourcePosition position = {});
    TExprId Column(TColumnId column, TType type, TSourcePosition position = {});
    TExprId Parameter(std::string name, TType type, TSourcePosition position = {});
    TExprId Unary(EOpcode op, TExprId input, TSourcePosition position = {});
    TExprId Binary(EOpcode op, TExprId left, TExprId right, TSourcePosition position = {});
    // Total numeric conversion/optional lifting; rejects conversions that can fail.
    TExprId Convert(TExprId input, TType target, TSourcePosition position = {});
    // Text-to-number/Boolean/temporal casts require literals and fold here.
    // String/Utf8 conversion remains a checked runtime operation.
    TExprId Cast(TExprId input, TType target, TSourcePosition position = {});
    TExprId If(TExprId condition, TExprId thenExpr, TExprId elseExpr, TSourcePosition position = {});
    TExprId Coalesce(std::span<const TExprId> arguments, TSourcePosition position = {});
    TExprId Call(std::string_view name, std::span<const TExprId> arguments, TSourcePosition position = {});

    const TExprNode& operator[](TExprId id) const;
    std::size_t Size() const { return Nodes_.size(); }
    const std::vector<TExprNode>& Nodes() const { return Nodes_; }

    // Copies only the reachable DAG, sharing repeated nodes. Substitution is
    // simultaneous and the resulting operations are checked against new types.
    TExprId Import(const TExpressionArena& source, TExprId root,
        const std::function<TExprId(TColumnId, TType, TSourcePosition)>& bindColumn);

    // Simultaneous column substitution in this arena. Return the original ID
    // to retain a column. Only changed nodes and their ancestors are appended;
    // unchanged subgraphs retain their IDs, and existing roots stay immutable.
    TExprId RewriteColumns(TExprId root,
        const std::function<TExprId(TExprId, TColumnId, TType, TSourcePosition)>& bindColumn);

private:
    TExprId Append(TExprNode node);
    TExprId Rebuild(TExprNode node);
    std::vector<TExprNode> Nodes_;
};

// Small reference executor for the native scalar contract. If/Coalesce/And/Or
// evaluate lazily, and each reached node is evaluated at most once per call.
// Missing bindings are errors, distinct from a binding containing NULL.
struct TEvaluationContext {
    std::function<TValue(TColumnId)> Column;
    std::function<TValue(std::string_view)> Parameter;
};

TValue Evaluate(const TExpressionArena& arena, TExprId root, const TEvaluationContext& context = {});
std::string FormatValue(const TValue& value);

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
