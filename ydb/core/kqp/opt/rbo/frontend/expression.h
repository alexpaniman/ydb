#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

// Scalar optionality records its depth so explicit nested optional casts retain
// the difference between Nothing and Just(Nothing). Internal tuples carry typed
// aggregate states and fixed IN operands; lists are scalar IN operands.
enum class ETypeKind : std::uint8_t {
    Null, Bool, Int8, Int16, Int32, Int64, Uint8, Uint16, Uint32, Uint64,
    Float, Double, String, Utf8, Decimal, Date, Datetime, Timestamp, Interval,
    Date32, Datetime64, Timestamp64, Interval64, Uuid, Json, JsonDocument,
    // Internal UTC calendar value, carried as timestamp microseconds. It is
    // executable but cannot be stored or cross a relational boundary.
    Calendar, Tuple, List,
};

struct TType {
    ETypeKind Kind = ETypeKind::Null;
    std::uint8_t OptionalDepth = 0;
    std::uint8_t Precision = 0;
    std::uint8_t Scale = 0;
    // Collection shapes are interned in the owning expression arena. Zero for scalars.
    // Like expression IDs, a shape ID must be imported when crossing arenas.
    std::uint32_t Shape = 0;

    bool operator==(const TType&) const = default;
    TType Optional() const;
    TType Required() const;
    TType Unwrapped() const;
    bool IsIntegral() const;
    bool IsNumeric() const;
    bool IsString() const;
    bool IsDate() const;
    bool IsInterval() const;
    bool IsExtendedTemporal() const;
    bool IsJson() const { return Kind == ETypeKind::Json || Kind == ETypeKind::JsonDocument; }
    bool IsSigned() const;
    bool IsCollection() const { return Kind == ETypeKind::Tuple || Kind == ETypeKind::List; }
    std::string ToString() const;
};

TType ParseType(std::string_view name, TSourcePosition position = {});
TType CommonType(TType left, TType right, TSourcePosition position = {});

struct TDecimal {
    __int128 Coefficient = 0;
    static constexpr __int128 Inf() { return __int128{100000000000000000ULL} * 1000000000000000000ULL; }
    static constexpr __int128 NaN() { return Inf() + 1; }
    bool operator==(const TDecimal&) const = default;
};

struct TTupleValue;
struct TValue;
using TOptionalValue = std::shared_ptr<const TValue>;
using TTuple = std::shared_ptr<const TTupleValue>;
using TScalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, TDecimal, TTuple, TOptionalValue>;

struct TValue {
    TType Type;
    TScalar Data;
    bool IsNull() const { return std::holds_alternative<std::monostate>(Data); }
};

struct TTupleValue {
    std::vector<TValue> Items;
};

// Every opcode is an execution operation. There are no unresolved calls,
// identifiers, lambda applications, type expressions, or optimizer macros here.
// Integral add/subtract/multiply/negate use two's-complement wrapping. Division
// and remainder return NULL for zero divisors and signed division overflow.
// Decimal scale reductions round to nearest, ties to even. Decimal arithmetic
// preserves infinity/NaN; overflow and division by zero do not introduce NULL.
// Division by a negative integer retains the runtime's truncation of non-ties.
enum class EOpcode : std::uint8_t {
    Literal, Column, Parameter, Convert, Cast,
    Negate, Not, Add, Subtract, Multiply, Divide, Modulo,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    And, Or, IsNull, IsNotNull, Coalesce, If,
    Abs, Length, Lower, Upper, Contains, StartsWith, EndsWith, Substring,
    Concat, Ensure, StablePickle, Find,
    CalendarSplit, CalendarDate, CalendarDatetime, CalendarTimestamp, CalendarYear,
    CalendarShiftMonths, CalendarShiftYears, CalendarMinute, TemporalSeconds, UrlHost, UrlCutWWW,
    Unwrap, UnicodeLower, UnicodeUpper, JsonValue, OptionalWrap, Collection, InCollection, RandomUuid, Like, TupleElement,
};

struct TExprNode {
    // Keep the column ID beside the opcode and the type after the name so the
    // tuple shape ID uses existing alignment space instead of growing nodes.
    EOpcode Op;
    bool Repeatable = true;
    TColumnId ColumnId;
    TSourcePosition Position;
    std::vector<TExprId> Arguments;
    TScalar Value;
    std::string Name;
    TType Type;

    TExprNode(EOpcode op, TType type, TSourcePosition position, std::vector<TExprId> arguments,
        TScalar value, std::string name, TColumnId columnId)
        : Op(op), ColumnId(columnId), Position(position), Arguments(std::move(arguments)),
          Value(std::move(value)), Name(std::move(name)), Type(type) {}
};

// Nodes are immutable after construction; all edges point to smaller IDs.
// Construction resolves types locally and inserts explicit casts. No annotation
// pass, fixed point, or whole-arena traversal is needed when appending a node.
// IDs remain valid after appends; references into Nodes() may be invalidated.
// A query may share this append-only arena, with mutation externally serialized.
class TExpressionArena {
public:
    // Flat tuples contain scalar fields, sufficient for aggregate states.
    // Construction validates and interns each shape once; scalar types need no lookup.
    TType TupleType(std::span<const TType> items, TSourcePosition position = {});
    std::span<const TType> TupleItems(TType type) const;
    TType ListType(TType item, TSourcePosition position = {});
    TType ListItem(TType type) const;
    TType ImportType(const TExpressionArena& source, TType type);
    void ValidateType(TType type, TSourcePosition position = {}) const;

    TExprId Collection(std::span<const TExprId> items, bool list, TSourcePosition position = {});
    TExprId TupleElement(TExprId tuple, std::uint32_t index, TSourcePosition position = {});
    TExprId InCollection(TExprId lookup, TExprId collection, bool ansi, TSourcePosition position = {});
    TExprId Literal(TType type, TScalar value, TSourcePosition position = {});
    TExprId Null(TSourcePosition position = {});
    TExprId Column(TColumnId column, TType type, TSourcePosition position = {});
    TExprId Parameter(std::string name, TType type, TSourcePosition position = {});
    TExprId Unary(EOpcode op, TExprId input, TSourcePosition position = {});
    TExprId Binary(EOpcode op, TExprId left, TExprId right, TSourcePosition position = {});
    // Total numeric conversion/optional lifting; rejects conversions that can fail.
    TExprId Convert(TExprId input, TType target, TSourcePosition position = {});
    // Text-to-Datetime/Timestamp casts require literals and fold here.
    // Decimal/numeric/Boolean casts, Date parsing and String/Utf8 conversion
    // support runtime input, returning NULL when parsing fails. Explicit nested
    // optional targets preserve each optional layer.
    TExprId Cast(TExprId input, TType target, TSourcePosition position = {});
    TExprId If(TExprId condition, TExprId thenExpr, TExprId elseExpr, TSourcePosition position = {});
    TExprId Coalesce(std::span<const TExprId> arguments, TSourcePosition position = {});
    // Runtime assertion: check a required Boolean before evaluating its value.
    TExprId Ensure(TExprId value, TExprId predicate, std::string message, TSourcePosition position = {});
    // Byte search: required needle and Uint32 offset, optional Uint32 result.
    TExprId Find(TExprId input, TExprId needle, TExprId offset, TSourcePosition position = {});
    // SQL JSON_VALUE's default Utf8? result and NULL ON EMPTY / NULL ON ERROR.
    // JSON and binary JSON inputs are opaque runtime values. The literal path
    // is compiled by the existing JSON runtime after the RBO boundary.
    TExprId JsonValue(TExprId input, std::string path, TSourcePosition position = {});
    // Runtime LIKE matching. Input may be NULL (false); pattern is required.
    // SQL literal-pattern simplifications belong to the binding layer.
    TExprId Like(TExprId input, TExprId pattern, std::string escape = {}, TSourcePosition position = {});
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

    // Simultaneous replacement of whole subexpressions in this arena. Return
    // the original ID to traverse its children; replacements are not traversed.
    // Changed ancestors are rebuilt with local type checking.
    TExprId Rewrite(TExprId root, const std::function<TExprId(TExprId)>& replace);

    // Factor structurally equal conjuncts across OR branches. Only reachable
    // nodes are inspected; runtime assertions retain their evaluation order.
    // The original root is returned when no factorization applies.
    TExprId FactorCommonConjuncts(TExprId root);

    // Fold shared constant DAGs once across all roots. Keep runtime assertions
    // deferred, preserve types, and bound newly materialized string data.
    std::vector<TExprId> FoldConstants(std::span<const TExprId> roots, std::size_t maxStringBytes = 1024 * 1024);

private:
    TExprId Append(TExprNode node);
    TExprId Rebuild(TExprNode node);
    std::vector<TExprNode> Nodes_;
    std::vector<std::vector<TType>> TupleTypes_;
    std::unordered_multimap<std::size_t, std::uint32_t> TupleHashes_;
};

// Small reference executor for the native scalar contract. If/Coalesce/And/Or
// evaluate lazily, and each reached node is evaluated at most once per call.
// Missing bindings are errors, distinct from a binding containing NULL.
struct TEvaluationContext {
    std::function<TValue(TColumnId)> Column;
    std::function<TValue(std::string_view)> Parameter;
    // Optional reference-execution service; the compiler has no JSON runtime
    // dependency. JSON_VALUE remains directly lowerable without this callback.
    std::function<TValue(const TValue&, std::string_view)> JsonValue;
    // Runtime service for nondeterministic UUID generation. Dependency values
    // order evaluation; they are not random seeds. One reached node runs once.
    std::function<std::string()> RandomUuid = {};
    std::function<bool(const TValue&, std::string_view, std::string_view)> Like = {};
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
