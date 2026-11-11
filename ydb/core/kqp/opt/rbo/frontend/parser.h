#pragma once

#include "expression.h"

#include <limits>
#include <memory>
#include <optional>
#include <iterator>
#include <variant>

namespace NKikimr::NKqp::NFrontend {

using TSyntaxId = std::uint32_t;
inline constexpr TSyntaxId NoSyntax = std::numeric_limits<TSyntaxId>::max();

struct TSelect;
struct TSyntaxWindow;
struct TSyntaxLambda;

enum class EWindowFrame : std::uint8_t { Default, Rows, Range, Groups };
enum class EWindowBound : std::uint8_t { UnboundedPreceding, Preceding, CurrentRow, Following, UnboundedFollowing };
enum class EWindowExclusion : std::uint8_t { Default, CurrentRow, Group, Ties, NoOthers };
enum class ENullTreatment : std::uint8_t { Default, Ignore, Respect };

struct TWindowFrame {
    EWindowFrame Type = EWindowFrame::Default;
    EWindowBound Begin = EWindowBound::UnboundedPreceding;
    EWindowBound End = EWindowBound::CurrentRow;
    std::uint64_t BeginOffset = 0;
    std::uint64_t EndOffset = 0;
};

enum class ESyntaxKind : std::uint8_t {
    Integer, Real, String, Boolean, Null, Column, Variable, Star,
    Unary, Binary, Call, Cast, Case, In, Between, ScalarSubquery, Exists, JsonValue, Index, Invoke, Lambda,
    ByteString, Utf8String, BitCast, JsonString, YsonString, Member, Named,
};

inline bool IsStringLiteral(ESyntaxKind kind) {
    return kind == ESyntaxKind::String || kind == ESyntaxKind::ByteString || kind == ESyntaxKind::Utf8String;
}

struct TSyntaxExpr {
    ESyntaxKind Kind;
    // Offset into the query's packed qualification separators. Zero denotes
    // one identifier; dots inside quoted identifiers are never separators.
    std::uint32_t NamePath = 0;
    TSourcePosition Position;
    std::string Text;
    std::vector<TSyntaxId> Arguments;
    std::shared_ptr<TSelect> Query;
    bool Distinct = false;
    ENullTreatment NullTreatment = ENullTreatment::Default;
    TSyntaxId Filter = NoSyntax;
    std::shared_ptr<TSyntaxWindow> Window = {};
    std::shared_ptr<TSyntaxLambda> Lambda = {};
};

// Lambda operands are local initializers in declaration order, then the body.
// Parameter and local names belong to lexical binding, not the scalar DAG.
struct TSyntaxLambda {
    std::vector<std::string> Parameters;
    std::vector<std::string> Locals;
};

struct TNamedExpr {
    TSyntaxId Expression = NoSyntax;
    std::string Alias = {};
};

enum class EHintValueKind : std::uint8_t { Identifier, String, Binding };
enum class EHintValueForm : std::uint8_t { None, Scalar, List };
struct TSourceHintValue {
    EHintValueKind Kind;
    std::string Text;
    TSourcePosition Position;
};
struct TSyntaxTypeMember;
struct TSyntaxType {
    TSourcePosition Position;
    std::string Name;
    bool Binding = false;
    std::uint32_t OptionalDepth = 0;
    std::vector<TSyntaxType> Arguments;
    std::vector<TSyntaxTypeMember> Members;
    std::vector<TSourceHintValue> Tags;
    std::vector<std::string> Parameters;
};
struct TSyntaxTypeMember {
    std::optional<TSourceHintValue> Name;
    TSyntaxType Type;
};
enum class EColumnNullability : std::uint8_t { Default, Nullable, NotNull };
struct TSchemaColumn {
    TSourceHintValue Name;
    TSyntaxType Type;
    EColumnNullability Nullability = EColumnNullability::Default;
};
struct TGenericSourceHint {
    std::string Name;
    EHintValueForm Form = EHintValueForm::None;
    std::vector<TSourceHintValue> Values;
};
struct TSchemaHint {
    bool Columns = false;
    std::variant<TSyntaxType, std::vector<TSchemaColumn>> Value;
};
struct TWatermarkHint {
    TSyntaxId Expression;
};
struct TSourceHint {
    TSourcePosition Position;
    std::variant<TGenericSourceHint, TSchemaHint, TWatermarkHint> Value;
};
enum class ESamplingMode : std::uint8_t { Sample, Bernoulli, System };
struct TSyntaxSampling {
    ESamplingMode Mode = ESamplingMode::Sample;
    TSyntaxId Rate = NoSyntax;
    TSyntaxId Seed = NoSyntax;
};
// Source-local syntax, allocated only for decorated sources. A vector retains
// repeated hint names and their order; binding owns execution admission.
struct TSourceOptions {
    std::vector<TSourceHint> Hints;
    std::optional<TSyntaxSampling> Sampling;
};

struct TSource {
    TSourcePosition Position;
    std::string Name;
    std::string Alias;
    std::shared_ptr<TSelect> Query;
    bool ParameterTable = false;
    bool Any = false;
    std::vector<std::string> ColumnAliases;
    std::optional<std::string> Index;
    std::shared_ptr<TSourceOptions> Options;
};

enum class EJoinKind : std::uint8_t { Inner, Cross, Left, Right, Full, LeftSemi, RightSemi, LeftOnly, RightOnly, Exclusion };

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

struct TSyntaxWindow {
    TSourcePosition Position;
    // OVER name references a complete window; OVER (name ...) inherits it.
    // Parsing retains that distinction. Binding owns name lookup and admission.
    std::string Name;
    bool ReferenceOnly = false;
    bool CompactPartition = false;
    bool ExplicitFrameEnd = false;
    EWindowExclusion Exclusion = EWindowExclusion::Default;
    std::vector<TNamedExpr> Partition;
    std::vector<TSortKey> Order;
    TWindowFrame Frame;
    TSyntaxId BeginOffset = NoSyntax;
    TSyntaxId EndOffset = NoSyntax;
};

struct TNamedWindow {
    std::string Name;
    std::shared_ptr<TSyntaxWindow> Specification;
};

enum class ESetOperation : std::uint8_t { Push, UnionAll, Union, IntersectAll, Intersect, ExceptAll, Except };

using TGroupingSets = std::vector<std::vector<std::uint32_t>>;
enum class EGroupingOperation : std::uint8_t { Product, Sets, Rollup, Cube };
// Key indices refer to TSelect::GroupBy. Bound expansion size before allocating
// products, including the exponential output of CUBE.
TGroupingSets ExpandGroupingSets(const std::vector<TGroupingSets>& elements,
    EGroupingOperation operation, std::size_t limit, TSourcePosition position);

struct TCteBinding {
    std::string Name;
    std::vector<std::string> Columns;
    std::shared_ptr<TSelect> Query;
};

struct TSelect {
    TSourcePosition Position;
    std::vector<TCteBinding> Ctes;
    std::optional<TSource> Source;
    std::vector<TJoin> Joins;
    std::vector<TNamedExpr> Projection;
    TSyntaxId Where = NoSyntax;
    std::vector<TNamedExpr> GroupBy;
    // Empty means ordinary grouping (or no GROUP BY). {{}} is an explicit
    // empty grouping set, which still requires an aggregate relation.
    TGroupingSets GroupingSets;
    TSyntaxId Having = NoSyntax;
    std::vector<TNamedWindow> Windows;
    std::vector<TSortKey> OrderBy;
    TSyntaxId Limit = NoSyntax;
    TSyntaxId Offset = NoSyntax;
    bool Distinct = false;
    // Postfix operations preserve precedence without recursively nesting a
    // long set-expression chain. All inputs share one name/type alignment.
    std::vector<std::shared_ptr<TSelect>> SetInputs;
    std::vector<ESetOperation> SetOperations;

    void AppendSetInput(std::shared_ptr<TSelect> input) {
        // Sorting, limits and local CTEs belong to this operand. Flatten only
        // a pure set expression, otherwise those semantics would be lost.
        if (input->SetInputs.empty() || !input->OrderBy.empty() || input->Limit != NoSyntax || !input->Ctes.empty()) {
            SetInputs.push_back(std::move(input));
            SetOperations.push_back(ESetOperation::Push);
        } else {
            SetInputs.insert(SetInputs.end(), std::make_move_iterator(input->SetInputs.begin()), std::make_move_iterator(input->SetInputs.end()));
            SetOperations.insert(SetOperations.end(), input->SetOperations.begin(), input->SetOperations.end());
        }
    }
};

enum class EWriteKind : std::uint8_t { Insert, Upsert, Replace, Update, Delete };

// Writes keep their input query/values and target separate. Binding resolves
// storage names, coercions and defaults before constructing an effect relation.
struct TWriteStatement {
    EWriteKind Kind;
    TSource Target;
    std::vector<std::string> Columns;
    std::shared_ptr<TSelect> Input;
    std::vector<std::vector<TSyntaxId>> Rows;
    TSyntaxId Where = NoSyntax;
    std::vector<TNamedExpr> Assignments;
    std::vector<std::string> Returning;
    bool ReturningAll = false;
    bool Batch = false;
    std::string OnConflict;
};

enum class EStatementKind : std::uint8_t { Query, Binding, Declaration, Pragma, Use, Write };
enum class EExplainMode : std::uint8_t { None, Explain, QueryPlan };
enum class EPragmaValueKind : std::uint8_t { String, Integer, Real, Boolean, Identifier, Binding };
enum class EPragmaValueForm : std::uint8_t { None, Assignment, List };

struct TPragmaValue {
    EPragmaValueKind Kind;
    std::string Text;
    TSourcePosition Position;
};

struct TStatement {
    TStatement(EStatementKind kind, TSourcePosition position) : Kind(kind), Position(position) {}
    EStatementKind Kind;
    EExplainMode Explain = EExplainMode::None;
    TSourcePosition Position;
    std::string Name;
    // DECLARE type spelling. Pragmas retain decoded arguments separately so
    // a missing value, an empty string and a list cannot be conflated.
    std::string Value;
    std::vector<TPragmaValue> PragmaValues;
    EPragmaValueForm PragmaForm = EPragmaValueForm::None;
    TSyntaxId Expression = NoSyntax;
    std::shared_ptr<TSelect> Query;
    std::shared_ptr<TWriteStatement> Write;
    bool ScalarList = false;
    // DECLARE List<Struct<...>> describes an external row source. Its fields
    // remain scalar type spellings; containers never enter scalar expressions.
    std::optional<std::vector<std::pair<std::string, std::string>>> RowFields;
};

struct TParsedQuery {
    std::vector<TSyntaxExpr> Expressions;
    std::vector<TStatement> Statements;
    // Diagnostic only; deliberately excluded from syntax correspondence.
    bool UsedGeneratedParser = false;
    std::vector<std::uint32_t> NamePaths = {};

    void AppendNamePart(std::string& name, std::uint32_t& path, std::string_view part, TSourcePosition position);
    std::span<const std::uint32_t> NameSeparators(const TSyntaxExpr& expression) const;
};

struct TParsedExpression {
    std::vector<TSyntaxExpr> Expressions;
    TSyntaxId Root;
    std::vector<std::uint32_t> NamePaths = {};
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
// Standalone scalar syntax, with the same lexer, precedence and limits as SQL.
// The entire input must be one expression; statement suffixes are rejected.
TParsedExpression ParseExpression(std::string_view sql, const TParseSettings& settings = {});

// Strict recognition of the complete shared SQL v1 grammar. Recovered parse
// trees are never used for compilation. Neither API invokes any YQL library.
void ValidateGrammar(std::string_view sql, const TParseSettings& settings = {});
TParsedQuery ParseGrammar(std::string_view sql, const TParseSettings& settings = {});
TParsedExpression ParseGrammarExpression(std::string_view sql, const TParseSettings& settings = {});

// Independent of parser allocation order and source positions; includes
// expression structure, statements, aliases, types and relational clauses.
std::string SyntaxFingerprint(const TParsedQuery& query);

} // namespace NKikimr::NKqp::NFrontend
