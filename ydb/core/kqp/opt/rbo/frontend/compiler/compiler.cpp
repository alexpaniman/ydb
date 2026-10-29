#include "compiler.h"
#include "../grammar/grammar.h"

#include <unordered_set>

namespace NKikimr::NKqp::NFrontend {
namespace {

std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

void CheckType(TType type, TSourcePosition pos) {
    if (type.Kind == ETypeKind::Decimal) {
        throw TUnsupportedFeature(EFeature::Type, pos,
            "Decimal is outside native-read-v1: finite-only arithmetic does not implement YQL special values");
    }
}

void CheckSyntax(const TParsedQuery& syntax) {
    for (const auto& node : syntax.Expressions) {
        if (node.Kind == ESyntaxKind::Case && node.Text == "implicit_else")
            throw TUnsupportedFeature(EFeature::Expression, node.Position, "CASE requires explicit ELSE in native-read-v1");
        if (node.Kind == ESyntaxKind::Real && (node.Text.ends_with('p') || node.Text.ends_with('P')))
            throw TUnsupportedFeature(EFeature::Type, node.Position, "PostgreSQL numeric literals are outside native-read-v1");
        if (node.Kind == ESyntaxKind::Column && node.Text.substr(node.Text.find_last_of('.') + 1).starts_with("_yql_"))
            throw TUnsupportedFeature(EFeature::Backend, node.Position, "system columns are outside native-read-v1");
        if (node.Kind == ESyntaxKind::Binary &&
            (node.Text == "+" || node.Text == "-" || node.Text == "*" || node.Text == "/" || node.Text == "%" || node.Text == "||")) {
            for (auto arg : node.Arguments) if (syntax.Expressions[arg].Kind == ESyntaxKind::Null)
                throw TUnsupportedFeature(EFeature::Expression, node.Position, "bare NULL arithmetic is outside native-read-v1");
        }
        if (node.Kind == ESyntaxKind::Call) {
            const auto name = Lower(node.Text);
            // These convenience aliases in the experimental expression API do
            // not have the same meaning (or do not exist) in YQL SQL.
            if (name == "lower" || name == "upper" || name == "substr" || name == "contains" ||
                name == "string::tolower" || name == "string::toupper" || name == "concat")
                throw TUnsupportedFeature(EFeature::Function, node.Position, "function is outside the compatible native profile: " + node.Text);
        }
        if (node.Kind == ESyntaxKind::Cast) {
            const auto target = ParseType(node.Text, node.Position);
            if (!target.IsIntegral())
                throw TUnsupportedFeature(EFeature::Expression, node.Position, "native-read-v1 currently permits explicit casts only to integer types");
        }
    }
}

void CheckProgram(const TBoundProgram& program) {
    for (const auto& column : program.Columns) CheckType(column.Type, {});
    for (const auto& [name, type] : program.Parameters) CheckType(type, {});
    for (const auto& node : program.Expressions.Nodes()) {
        CheckType(node.Type, node.Position);
        if ((node.Op >= EOpcode::Add && node.Op <= EOpcode::Modulo) || node.Op == EOpcode::Concat) {
            for (auto id : node.Arguments) {
                while (program.Expressions[id].Op == EOpcode::Convert) id = program.Expressions[id].Arguments[0];
                if (program.Expressions[id].Type.Kind == ETypeKind::Null)
                    throw TUnsupportedFeature(EFeature::Expression, node.Position, "bare NULL arithmetic is outside native-read-v1");
            }
        }
        if (node.Op == EOpcode::Substring ||
            (node.Op == EOpcode::Divide && (node.Type.Kind == ETypeKind::Float || node.Type.Kind == ETypeKind::Double))) {
            throw TUnsupportedFeature(EFeature::Expression, node.Position,
                "substring and floating division are outside native-read-v1 pending YQL semantic parity");
        }
        if (node.Op == EOpcode::Convert && !node.Arguments.empty()) {
            const auto from = program.Expressions[node.Arguments[0]].Type;
            if ((node.Type.Kind == ETypeKind::Double && from.Kind == ETypeKind::Float) ||
                (node.Type.Kind == ETypeKind::Float && from.Kind != ETypeKind::Float))
                throw TUnsupportedFeature(EFeature::Type, node.Position, "mixed Float promotion is outside native-read-v1");
            if (from.IsIntegral() && node.Type.IsIntegral() && from.IsSigned() != node.Type.IsSigned())
                throw TUnsupportedFeature(EFeature::Type, node.Position, "implicit signed/unsigned promotion is outside native-read-v1");
        }
        if (node.Op == EOpcode::Cast && !program.Expressions[node.Arguments[0]].Type.IsIntegral() &&
            program.Expressions[node.Arguments[0]].Type.Kind != ETypeKind::Null)
            throw TUnsupportedFeature(EFeature::Expression, node.Position, "runtime nonintegral casts are outside native-read-v1");
        if ((node.Op == EOpcode::Lower || node.Op == EOpcode::Upper) && node.Type.Kind != ETypeKind::String)
            throw TUnsupportedFeature(EFeature::Function, node.Position, "native ASCII case functions require String input");
        if (node.Op == EOpcode::Cast && node.Type.IsString()) {
            const auto input = program.Expressions[node.Arguments[0]].Type;
            if (!input.IsString() && !input.IsIntegral() && input.Kind != ETypeKind::Bool) {
                throw TUnsupportedFeature(EFeature::Expression, node.Position,
                    "native-read-v1 does not implement this type's YQL text formatting");
            }
        }
    }
    for (const auto& relation : program.Relations) {
        if (relation.Kind == ERelation::Aggregate && relation.Aggregates.size() > 1) {
            for (const auto& agg : relation.Aggregates) if (agg.Distinct) {
                throw TUnsupportedFeature(EFeature::AggregateCombination, relation.Position,
                    "native RBO supports a DISTINCT aggregate only when it is the sole aggregate");
            }
        }
    }
}

class TTableCollector {
public:
    explicit TTableCollector(const TBindSettings& settings) : Settings(settings), Prefix(settings.TablePathPrefix) {}
    std::vector<TTableRequest> Run(const TParsedQuery& syntax) {
        for (const auto& statement : syntax.Statements) {
            if (statement.Kind == EStatementKind::Binding && statement.Query) Queries["$" + statement.Name] = statement.Query;
            if (statement.Kind == EStatementKind::Pragma && Lower(statement.Name) == "tablepathprefix") Prefix = statement.Value;
            if (statement.Kind == EStatementKind::Query) Select(*statement.Query);
        }
        return std::move(Tables);
    }
private:
    void Source(const TSource& source) {
        auto query = source.Query;
        if (!query) if (const auto it = Queries.find(source.Name); it != Queries.end()) query = it->second;
        if (query) {
            if (!Active.insert(query.get()).second) throw TFrontendError(source.Position, "recursive query binding");
            Select(*query);
            Active.erase(query.get());
        } else if (!source.Name.starts_with('$')) {
            auto path = source.Name;
            if (!Prefix.empty() && !path.starts_with('/')) path = Prefix + (Prefix.ends_with('/') ? "" : "/") + path;
            if (Seen.insert(path).second) Tables.push_back({std::move(path), source.Position});
        }
    }
    void Select(const TSelect& query) {
        if (++Depth > Settings.MaxExpansionDepth || ++Nodes > Settings.MaxNodes)
            throw TResourceLimitError(query.Position, "table discovery expansion limit exceeded");
        auto saved = Queries;
        for (const auto& [name, select] : query.Ctes) Queries[name] = select;
        if (query.Source) Source(*query.Source);
        for (const auto& join : query.Joins) Source(join.Source);
        for (const auto& branch : query.UnionAll) Select(*branch);
        Queries = std::move(saved);
        --Depth;
    }
    const TBindSettings& Settings;
    std::string Prefix;
    std::unordered_map<std::string, std::shared_ptr<TSelect>> Queries;
    std::unordered_set<const TSelect*> Active;
    std::unordered_set<std::string> Seen;
    std::vector<TTableRequest> Tables;
    std::size_t Depth = 0, Nodes = 0;
};

} // namespace

TParsedQuery ParseNative(std::string_view sql, const TCompilerSettings& settings) {
    auto parsing = settings.Parsing;
    // Translation settings apply only to the leading directive-comment lines.
    // Ordinary comments or the first SQL token end this prefix. A string value
    // containing "--!" never changes language settings.
    auto prefix = sql;
    std::uint32_t line = 1;
    while (!prefix.empty()) {
        const auto end = prefix.find_first_of("\r\n");
        auto text = prefix.substr(0, end);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
        if (!text.empty()) {
            if (!text.starts_with("--!")) break;
            text.remove_prefix(3);
            const TSourcePosition pos{sql.size() - prefix.size(), line, 1};
            if (text.find_first_of(" \t") != text.npos) throw TFrontendError(pos, "bad translation settings format");
            if (text == "ansi_lexer") parsing.AnsiQuotes = true;
            else if (text.empty() || text == "syntax_v1" || text == "antlr4_parser") {}
            else if (text.starts_with("syntax_"))
                throw TUnsupportedFeature(EFeature::LanguageVersion, pos, "native frontend supports SQL v1");
            else throw TFrontendError(pos, "unknown SQL translation setting: " + std::string(text));
        }
        if (end == prefix.npos) break;
        prefix.remove_prefix(end + 1);
        ++line;
    }
    if (settings.Parser == EParser::Handwritten) return Parse(sql, parsing);
    if (settings.Parser == EParser::Fast) {
        try {
            return Parse(sql, parsing);
        } catch (const TResourceLimitError&) {
            throw;
        } catch (const TFrontendError&) {
            // This is parser dispatch, not optimizer fallback. The grammar
            // parser supplies authoritative rejection/unsupported diagnostics.
            return ParseGrammar(sql, parsing);
        }
    }
    auto grammar = ParseGrammar(sql, parsing);
    if (settings.Parser == EParser::Verify) {
        auto handwritten = Parse(sql, parsing);
        if (SyntaxFingerprint(grammar) != SyntaxFingerprint(handwritten))
            throw std::logic_error("native parser differential mismatch");
        return handwritten;
    }
    return grammar;
}

std::vector<TTableRequest> CollectTables(const TParsedQuery& syntax, const TBindSettings& settings) {
    return TTableCollector(settings).Run(syntax);
}

TBoundProgram BindNative(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings) {
    CheckSyntax(syntax);
    auto program = Bind(syntax, [&](std::string_view name, TSourcePosition pos) {
        auto table = catalog(name, pos);
        for (const auto& column : table.Columns) CheckType(column.Type, pos);
        return table;
    }, settings);
    CheckProgram(program);
    return program;
}

TCompileResult CompileNative(std::string_view sql, const TTableResolver& catalog, const TCompilerSettings& settings) {
    if (settings.Mode == ECompilerMode::Legacy) return {};
    try {
        return {BindNative(ParseNative(sql, settings), catalog, settings.Binding), {}};
    } catch (const TUnsupportedFeature& error) {
        if (settings.Mode == ECompilerMode::NativeRequired) throw;
        return {{}, TUnsupported{error.Feature, error.Position, error.what()}};
    }
}

std::string_view SupportManifest() {
    return R"({"profile":"native-read-v1","default_parser":"fast","parser_dispatch":"handwritten subset; shared-grammar-antlr4 for unhandled syntax",
"statements":["select","declare","scalar-binding","query-binding","scalar-lambda"],
"relations":["read","project","filter","on-join","aggregate","sort","limit","union-all"],
"types":["Bool","Int8","Int16","Int32","Int64","Uint8","Uint16","Uint32","Uint64","Float","Double","String","Utf8","Date","Datetime","Timestamp","Interval","Null","single-optional"],
"aggregates":["count","sum","avg","min","max","some"],
"pragmas":["TablePathPrefix","YqlSelect=force","AnsiImplicitCrossJoin"],
"restrictions":["one DISTINCT aggregate, alone","computed GROUP BY keys require aliases","CASE requires ELSE","no stars over joins","constant simple LIKE pattern","explicit casts only to integer; runtime casts require integral input","unsigned dynamic LIMIT/OFFSET","no implicit signed/unsigned or mixed Float promotion","no floating division, substring or bare NULL arithmetic","no UNION ordering","no windows, subqueries, JOIN USING, grouping sets, DML, DDL, UDFs, containers, PostgreSQL or Decimal","no floating or temporal text formatting","no string literals as identifiers, system columns, typed string suffixes or extended string escapes"],
"fallback":"whole query to legacy YQL optimizer, only on structured unsupported feature",
"status":"experimental; grammar recognition exceeds execution coverage"})";
}

} // namespace NKikimr::NKqp::NFrontend
