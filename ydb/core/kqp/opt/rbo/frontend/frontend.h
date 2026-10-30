#pragma once

#include "parser.h"

#include <unordered_map>
#include <utility>

namespace NKikimr::NKqp::NFrontend {

struct TCatalogColumn {
    std::string Name;
    TType Type;
};

struct TCatalogTable {
    std::string Name;
    std::vector<TCatalogColumn> Columns;
};

using TTableResolver = std::function<TCatalogTable(std::string_view, TSourcePosition)>;
using TRelId = std::uint32_t;
inline constexpr TRelId NoRelation = std::numeric_limits<TRelId>::max();

struct TColumn {
    std::string Name;
    std::string Qualifier;
    TType Type;
};

struct TDefinition {
    TColumnId Column;
    TExprId Expression;
};

struct TAggregate {
    TColumnId Output;
    TColumnId Input;
    std::string Function;
    bool Distinct = false;
};

struct TBoundSortKey {
    TColumnId Column;
    bool Descending;
};

struct TWindowFunction {
    TColumnId Output;
    std::optional<TColumnId> Input;
    std::string Function;
};

enum class ERelation : std::uint8_t { Unit, Read, Map, Filter, Join, Aggregate, Sort, Limit, UnionAll, Capture, Window };

struct TCapture {
    TColumnId Local;
    TColumnId Outer;
};

struct TRelation {
    TRelation(ERelation kind, TSourcePosition position) : Kind(kind), Position(position) {}
    ERelation Kind;
    TSourcePosition Position;
    std::vector<TRelId> Inputs;
    std::vector<TColumnId> Columns;
    std::string Table;
    std::vector<TDefinition> Definitions;
    std::vector<TCapture> Captures;
    std::optional<TExprId> Predicate;
    EJoinKind JoinKind = EJoinKind::Inner;
    std::vector<TColumnId> GroupKeys;
    std::vector<TAggregate> Aggregates;
    // An aggregate with grouping sets has fresh outward key/result bindings.
    // Indicator bindings map their output to the input key they describe.
    TGroupingSets GroupingSets;
    std::vector<std::pair<TColumnId, TColumnId>> GroupingColumns;
    std::vector<std::pair<TColumnId, TColumnId>> GroupingIndicators;
    std::vector<TBoundSortKey> SortKeys;
    std::optional<TExprId> Count;
    std::optional<TExprId> Offset;
    // Union outputs bind positionally to each input's columns.
    std::vector<std::vector<TColumnId>> UnionInputs;
    // Window partition/order keys use GroupKeys/SortKeys; arguments are ordinary columns.
    std::vector<TWindowFunction> WindowFunctions;
    TWindowFrame WindowFrame;
};

struct TResultColumn {
    TColumnId Column;
    std::string Name;
    TType Type;
};

struct TQueryResult {
    TRelId Root;
    std::vector<TResultColumn> Columns;
};

// Subquery results have ordinary typed column references in scalar DAGs.
// Their producers stay relational and use RBO's existing subplan rewrites.
enum class ESubquery : std::uint8_t { Scalar, Exists, In };
struct TSubplan {
    TRelId Root;
    TColumnId Result;
    ESubquery Kind = ESubquery::Scalar;
    std::optional<TColumnId> Lookup;
};

struct TBoundProgram {
    TExpressionArena Expressions;
    std::vector<TColumn> Columns;
    std::vector<TRelation> Relations;
    std::vector<TQueryResult> Results;
    std::unordered_map<TColumnId, TSubplan> Subplans;
    std::unordered_map<std::string, TType> Parameters;
};

struct TBindSettings {
    // Generic-query semantics; data/scan APIs select the legacy IN default.
    bool AnsiIn = true;
    bool AnsiCurrentRow = true;
    std::string TablePathPrefix;
    std::unordered_map<std::string, TType> ParameterTypes;
    std::size_t MaxNodes = 1'000'000;
    std::size_t MaxExpansionDepth = 128;
};

namespace NDetail {
TBoundProgram Bind(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings = {});
TBoundProgram Compile(std::string_view sql, const TTableResolver& catalog,
    const TBindSettings& bindSettings = {}, const TParseSettings& parseSettings = {});

} // namespace NDetail

// Stable, readable native explain output, useful without a YQL environment.
std::string Explain(const TBoundProgram& program);

enum class ECompilerMode { NativeRequired, Auto, Legacy };
enum class EParser { Grammar, Handwritten, Verify, Fast };

struct TCompilerSettings {
    ECompilerMode Mode = ECompilerMode::NativeRequired;
    EParser Parser = EParser::Fast;
    TParseSettings Parsing;
    TBindSettings Binding;
};

struct TUnsupported {
    EFeature Feature;
    TSourcePosition Position;
    std::string Message;
};

// Only the checked compiler can construct this handoff. The RBO converter
// consumes it, so unchecked syntax/binder experiments cannot enter production.
class TCompiledProgram {
public:
    TCompiledProgram(TCompiledProgram&&) = default;
    TCompiledProgram& operator=(TCompiledProgram&&) = default;
    const TBoundProgram& Get() const { return Program; }
    TBoundProgram Release() && { return std::move(Program); }

private:
    explicit TCompiledProgram(TBoundProgram program) : Program(std::move(program)) {}
    friend TCompiledProgram BindNative(const TParsedQuery&, const TTableResolver&, const TBindSettings&);
    TBoundProgram Program;
};

struct TCompileResult {
    std::optional<TCompiledProgram> Program;
    std::optional<TUnsupported> Unsupported;
    bool UsesLegacy() const { return !Program; }
};

struct TTableRequest {
    std::string Name;
    TSourcePosition Position;
};

// Parsing and binding can be separated by asynchronous catalog I/O. Both use
// only native types. Fast uses the handwritten subset and delegates unhandled
// syntax to the independent generated parser. Verify checks correspondence.
TParsedQuery ParseNative(std::string_view sql, const TCompilerSettings& settings = {});
std::vector<TTableRequest> CollectTables(const TParsedQuery& syntax, const TBindSettings& settings = {});
TCompiledProgram BindNative(const TParsedQuery& syntax, const TTableResolver& catalog,
    const TBindSettings& settings = {});
TCompileResult CompileNative(std::string_view sql, const TTableResolver& catalog,
    const TCompilerSettings& settings = {});

// This is the execution profile, not a promise of support for every construct
// recognized by the full grammar. New support requires semantic parity tests.
std::string_view SupportManifest();

} // namespace NKikimr::NKqp::NFrontend
