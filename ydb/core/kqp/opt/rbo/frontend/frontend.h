#pragma once

#include "parser.h"

#include <unordered_map>
#include <utility>
#include <variant>

namespace NKikimr::NKqp::NFrontend {

struct TCatalogColumn {
    std::string Name;
    TType Type;
    bool operator==(const TCatalogColumn&) const = default;
};

// A checked scalar over an explicit row schema. Column IDs are schema ordinals,
// independent of query IUs. Catalog expressions use this boundary before RBO.
struct TRowExpression {
    TExpressionArena Expressions;
    TExprId Root;
    std::vector<std::string> Columns;
    // Schema ordinals used by the folded root, in schema order.
    std::vector<TColumnId> Dependencies;
};

TRowExpression CompileRowExpression(std::string_view sql, std::span<const TCatalogColumn> columns,
    TType target, const TParseSettings& settings = {});

// Parameters are either executable scalars or external lists of flat rows.
// Row schemas are canonical by field name and do not require an expression arena.
class TParameterType {
public:
    using TRows = std::vector<TCatalogColumn>;
    struct TList { TType Item; bool operator==(const TList&) const = default; };
    TParameterType(TType scalar = {}) : Value(scalar) {}
    TParameterType(TRows rows);
    TParameterType(TList list) : Value(list) {}
    const TList* ScalarList() const { return std::get_if<TList>(&Value); }
    const TType* Scalar() const { return std::get_if<TType>(&Value); }
    const TRows* Rows() const { return std::get_if<TRows>(&Value); }
    bool operator==(const TParameterType&) const = default;
private:
    std::variant<TType, TRows, TList> Value;
};

// A missing write schema identifies a catalog object that cannot be written
// in this configuration. Storage keys and defaults belong to table metadata,
// not scalar/parameter field types.
enum class EColumnDefault : std::uint8_t { Literal, Sequence };

struct TGeneratedColumn {
    std::string Name;
    std::string Expression;
    bool Stored = true;
};

using TGeneratedValues = std::vector<std::pair<std::string, std::shared_ptr<const TRowExpression>>>;

struct TWriteSchema {
    std::vector<std::string> Keys;
    std::vector<std::pair<std::string, EColumnDefault>> Defaults;
    bool HasIndexes = false;
    std::vector<TGeneratedColumn> GeneratedColumns = {};
};

struct TCatalogTable {
    std::string Name;
    std::vector<TCatalogColumn> Columns;
    std::optional<TWriteSchema> WriteSchema = {};
    // Catalog-specific explanation, consumed only when binding a write to an
    // object without WriteSchema. Reads do not depend on write capabilities.
    std::string WriteUnsupportedReason = {};
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
    // Nullable keys retain SQL's NULL ordering. Required keys use the
    // canonical first marker, since their NULL placement has no effect.
    bool NullsFirst;
};

struct TWindowFunction {
    TColumnId Output;
    std::optional<TColumnId> Input;
    std::string Function;
};

enum class ERelation : std::uint8_t { Unit, Read, Map, Filter, Join, Aggregate, Sort, Limit, UnionAll, Capture, Window, Parameter, Reuse, Effect };

struct TBoundEffect {
    EWriteKind Kind;
    std::vector<std::pair<TColumnId, std::string>> Columns;
    std::vector<std::pair<TColumnId, std::string>> Returning;
    std::vector<std::string> DefaultColumns;
    TGeneratedValues GeneratedColumns;
    bool HasIndexes = false;
    bool InputRows = false;
    bool Batch = false;
    std::string OnConflict;
};

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
    std::string Alias;
    std::optional<std::string> Index;
    std::vector<TDefinition> Definitions;
    std::vector<TCapture> Captures;
    std::optional<TExprId> Predicate;
    EJoinKind JoinKind = EJoinKind::Inner;
    std::vector<TColumnId> GroupKeys;
    std::vector<TAggregate> Aggregates;
    // DISTINCT exports fresh aliases of its keys, matching the RBO aggregate contract.
    bool DistinctAll = false;
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
    std::shared_ptr<const TBoundEffect> Effect;
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

// Static query options cross the same checked boundary as the relational plan.
// Keep source order: every occurrence is validated, including overwritten hints.
enum class EPlannerSetting : std::uint8_t {
    OptDisableAutoIndexSelection, EnableAutoIndexSelectionForIndexLookupJoin,
    OptDisableTopSort, OptEnablePredicateExtract, OptEnableInplaceUpdate,
    OptShuffleElimination, OptShuffleEliminationWithMap, OptShuffleEliminationForAggregation,
    EnableNewRBOPhysicalStagePeephole, WindowFunctionsV2,
    OptEnableOlapPushdown, OptEnableOlapPushdownAggregate, OptEnableOlapPushdownProjections, OptEnableOlapPushdownRegexp,
    DqHashOperatorsUseBlocks, UseBlockHashJoin, DisableBlockExecution, OptFallbackToLegacyOptimizer,
    CostBasedOptimizationLevel, DqChannelVersion, CostBasedOptimization, HashJoinMode, OptimizerHints,
};

struct TPlannerSetting {
    EPlannerSetting Kind;
    std::variant<bool, std::uint32_t, std::string> Value;
    TSourcePosition Position;
};

struct TBoundProgram {
    TExpressionArena Expressions;
    std::vector<TColumn> Columns;
    std::vector<TRelation> Relations;
    std::vector<TQueryResult> Results;
    std::unordered_map<TColumnId, TSubplan> Subplans;
    std::unordered_map<std::string, TParameterType> Parameters;
    std::vector<TPlannerSetting> PlannerSettings;
};

struct TBindSettings {
    // Generic-query semantics; data/scan APIs select the legacy IN default.
    bool AnsiIn = true;
    bool AnsiCurrentRow = true;
    bool UnicodeLiterals = false;
    bool ClassicDivision = true;
    bool CheckedOps = false;
    std::string TablePathPrefix;
    std::unordered_map<std::string, TParameterType> ParameterTypes;
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
class TDiscoveredQuery;

class TCompiledProgram {
public:
    TCompiledProgram(TCompiledProgram&&) = default;
    TCompiledProgram& operator=(TCompiledProgram&&) = default;
    const TBoundProgram& Get() const { return Program; }
    TBoundProgram Release() && { return std::move(Program); }

private:
    explicit TCompiledProgram(TBoundProgram program) : Program(std::move(program)) {}
    friend TCompiledProgram BindNative(const TParsedQuery&, const TTableResolver&, const TBindSettings&);
    friend TCompiledProgram BindNative(const TDiscoveredQuery&, const TTableResolver&);
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

// Own the syntax, settings and resolved lexical bindings across catalog I/O.
// Copies share immutable discovery state; each binding uses a fresh catalog.
class TDiscoveredQuery {
public:
    std::span<const TTableRequest> Tables() const;
    bool UsedGeneratedParser() const;

private:
    struct TImpl;
    explicit TDiscoveredQuery(std::shared_ptr<const TImpl> impl) : Impl(std::move(impl)) {}
    friend TDiscoveredQuery DiscoverNative(TParsedQuery, TBindSettings);
    friend TCompiledProgram BindNative(const TDiscoveredQuery&, const TTableResolver&);
    std::shared_ptr<const TImpl> Impl;
};

// Parsing and binding can be separated by asynchronous catalog I/O. Both use
// only native types. Fast uses the handwritten subset and delegates unhandled
// syntax to the independent generated parser. Verify checks correspondence.
TParsedQuery ParseNative(std::string_view sql, const TCompilerSettings& settings = {});
TDiscoveredQuery DiscoverNative(TParsedQuery syntax, TBindSettings settings = {});
TCompiledProgram BindNative(const TDiscoveredQuery& query, const TTableResolver& catalog);
std::vector<TTableRequest> CollectTables(const TParsedQuery& syntax, const TBindSettings& settings = {});
TCompiledProgram BindNative(const TParsedQuery& syntax, const TTableResolver& catalog,
    const TBindSettings& settings = {});
TCompileResult CompileNative(std::string_view sql, const TTableResolver& catalog,
    const TCompilerSettings& settings = {});

// This is the execution profile, not a promise of support for every construct
// recognized by the full grammar. New support requires semantic parity tests.
std::string_view SupportManifest();

} // namespace NKikimr::NKqp::NFrontend
