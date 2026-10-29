#pragma once

#include "parser.h"

#include <unordered_map>

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

enum class ERelation : std::uint8_t { Unit, Read, Map, Filter, Join, Aggregate, Sort, Limit, UnionAll };

struct TRelation {
    TRelation(ERelation kind, TSourcePosition position) : Kind(kind), Position(position) {}
    ERelation Kind;
    TSourcePosition Position;
    std::vector<TRelId> Inputs;
    std::vector<TColumnId> Columns;
    std::string Table;
    std::vector<TDefinition> Definitions;
    std::optional<TExprId> Predicate;
    EJoinKind JoinKind = EJoinKind::Inner;
    std::vector<TColumnId> GroupKeys;
    std::vector<TAggregate> Aggregates;
    std::vector<TBoundSortKey> SortKeys;
    std::optional<TExprId> Count;
    std::optional<TExprId> Offset;
    // Union outputs bind positionally to each input's columns.
    std::vector<std::vector<TColumnId>> UnionInputs;
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

struct TBoundProgram {
    TExpressionArena Expressions;
    std::vector<TColumn> Columns;
    std::vector<TRelation> Relations;
    std::vector<TQueryResult> Results;
    std::unordered_map<std::string, TType> Parameters;
};

struct TBindSettings {
    std::string TablePathPrefix;
    std::unordered_map<std::string, TType> ParameterTypes;
    std::size_t MaxNodes = 1'000'000;
    std::size_t MaxExpansionDepth = 128;
};

TBoundProgram Bind(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings = {});
TBoundProgram Compile(std::string_view sql, const TTableResolver& catalog,
    const TBindSettings& bindSettings = {}, const TParseSettings& parseSettings = {});

// Stable, readable native explain output, useful without a YQL environment.
std::string Explain(const TBoundProgram& program);

} // namespace NKikimr::NKqp::NFrontend
