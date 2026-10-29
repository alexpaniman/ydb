#pragma once

#include "expression.h"

#include <limits>
#include <memory>
#include <optional>

namespace NKikimr::NKqp::NFrontend {

using TSyntaxId = std::uint32_t;
inline constexpr TSyntaxId NoSyntax = std::numeric_limits<TSyntaxId>::max();

struct TSelect;

enum class ESyntaxKind : std::uint8_t {
    Integer, Real, String, Boolean, Null, Column, Variable, Star,
    Unary, Binary, Call, Cast, Case, In, Between, ScalarSubquery, Exists,
};

struct TSyntaxExpr {
    ESyntaxKind Kind;
    TSourcePosition Position;
    std::string Text;
    std::vector<TSyntaxId> Arguments;
    std::shared_ptr<TSelect> Query;
    bool Distinct = false;
};

struct TNamedExpr {
    TSyntaxId Expression = NoSyntax;
    std::string Alias = {};
};

struct TSource {
    TSourcePosition Position;
    std::string Name;
    std::string Alias;
    std::shared_ptr<TSelect> Query;
};

enum class EJoinKind : std::uint8_t { Inner, Cross, Left, Right, Full, LeftSemi, RightSemi, LeftOnly, RightOnly };

struct TJoin {
    EJoinKind Kind;
    TSource Source;
    TSyntaxId Predicate = NoSyntax;
    std::vector<std::string> Using = {};
};

struct TSortKey {
    TSyntaxId Expression;
    bool Descending = false;
};

struct TSelect {
    TSourcePosition Position;
    std::vector<std::pair<std::string, std::shared_ptr<TSelect>>> Ctes;
    std::optional<TSource> Source;
    std::vector<TJoin> Joins;
    std::vector<TNamedExpr> Projection;
    TSyntaxId Where = NoSyntax;
    std::vector<TNamedExpr> GroupBy;
    TSyntaxId Having = NoSyntax;
    std::vector<TSortKey> OrderBy;
    TSyntaxId Limit = NoSyntax;
    TSyntaxId Offset = NoSyntax;
    bool Distinct = false;
    std::vector<std::shared_ptr<TSelect>> UnionAll;
};

enum class EStatementKind : std::uint8_t { Query, Binding, Declaration, Pragma, Use };

struct TStatement {
    TStatement(EStatementKind kind, TSourcePosition position) : Kind(kind), Position(position) {}
    EStatementKind Kind;
    TSourcePosition Position;
    std::string Name;
    std::string Value;
    TSyntaxId Expression = NoSyntax;
    std::shared_ptr<TSelect> Query;
    std::vector<std::string> Parameters;
    bool Lambda = false;
};

struct TParsedQuery {
    std::vector<TSyntaxExpr> Expressions;
    std::vector<TStatement> Statements;
    // Diagnostic only; deliberately excluded from syntax correspondence.
    bool UsedGeneratedParser = false;
};

struct TParseSettings {
    bool AnsiQuotes = false;
    std::size_t MaxQueryBytes = 16 * 1024 * 1024;
    std::size_t MaxNodes = 1'000'000;
    std::size_t MaxDepth = 256;
};

// Direct lexer + recursive descent with precedence climbing. There is no
// backtracking, parser runtime, protobuf tree, or YQL dependency. The grammar
// reference is yql/essentials/sql/v1/SQLv1Antlr4.g.in. Unsupported productions
// fail at their source position; they are never skipped or guessed.
TParsedQuery Parse(std::string_view sql, const TParseSettings& settings = {});

} // namespace NKikimr::NKqp::NFrontend
