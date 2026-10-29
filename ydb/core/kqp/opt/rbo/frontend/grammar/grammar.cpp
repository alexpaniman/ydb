#include "grammar.h"
#include "../diagnostic.h"

#include <ydb/core/kqp/opt/rbo/frontend/grammar/default/SQLv1Antlr4Lexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/default/SQLv1Antlr4Parser.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/ansi/SQLv1Antlr4Lexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/ansi/SQLv1Antlr4Parser.h>

#include <algorithm>
#include <sstream>

namespace NKikimr::NKqp::NFrontend {
namespace {
using TTree = antlr4::tree::ParseTree;
using TRule = antlr4::ParserRuleContext;

std::string Upper(std::string text) {
    for (auto& c : text) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return text;
}

class TPositions {
public:
    explicit TPositions(std::string_view sql) {
        for (std::size_t i = sql.starts_with("\xef\xbb\xbf") ? 3 : 0; i < sql.size(); ++i) {
            if ((static_cast<unsigned char>(sql[i]) & 0xc0) != 0x80) Offsets.push_back(i);
        }
        Offsets.push_back(sql.size());
    }
    TSourcePosition Get(antlr4::Token* token) const {
        if (!token) return {};
        return {Offsets[std::min(token->getStartIndex(), Offsets.size() - 1)],
            static_cast<std::uint32_t>(token->getLine()),
            static_cast<std::uint32_t>(token->getCharPositionInLine() + 1)};
    }
private:
    std::vector<std::size_t> Offsets;
};

class TErrors : public antlr4::BaseErrorListener {
public:
    explicit TErrors(const TPositions& positions) : Positions(positions) {}
    void syntaxError(antlr4::Recognizer*, antlr4::Token* token, size_t line, size_t column,
        const std::string& message, std::exception_ptr) override {
        auto position = Positions.Get(token);
        position.Line = line;
        position.Column = column + 1;
        throw TFrontendError(position, message);
    }
private:
    const TPositions& Positions;
};

class TLimits : public antlr4::tree::ParseTreeListener {
public:
    TLimits(const TParseSettings& settings, const TPositions& positions)
        : Settings(settings), Positions(positions) {}
    void enterEveryRule(TRule* rule) override {
        // The grammar has several precedence wrappers per SQL nesting level.
        if (++Depth > Settings.MaxDepth * 32 || ++Nodes > Settings.MaxNodes * 32)
            throw TResourceLimitError(Positions.Get(rule->getStart()), "grammar resource limit exceeded");
    }
    void exitEveryRule(TRule*) override { --Depth; }
    void visitTerminal(antlr4::tree::TerminalNode*) override {}
    void visitErrorNode(antlr4::tree::ErrorNode*) override {}
private:
    const TParseSettings& Settings;
    const TPositions& Positions;
    std::size_t Depth = 0, Nodes = 0;
};

class TTranslate {
public:
    TTranslate(const std::vector<std::string>& names, const TPositions& positions, const TParseSettings& settings)
        : Names(names), Positions(positions), Settings(settings) {}

    TParsedQuery Run(TRule* root) {
        Result.UsedGeneratedParser = true;
        auto* list = Child(root, "sql_stmt_list");
        if (!list) Unsupported(root, EFeature::LanguageVersion, "unsupported SQL language mode");
        for (auto* statement : Children(list, "sql_stmt")) {
            if (Has(statement, "EXPLAIN")) Unsupported(statement, EFeature::Statement, "EXPLAIN SQL syntax");
            auto* core = Child(statement, "sql_stmt_core");
            Result.Statements.push_back(Statement(Only(core)));
        }
        return std::move(Result);
    }

private:
    std::string_view Name(TTree* node) const {
        if (auto* rule = dynamic_cast<TRule*>(node)) return Names.at(rule->getRuleIndex());
        return {};
    }
    TSourcePosition Position(TTree* node) const {
        if (auto* rule = dynamic_cast<TRule*>(node)) return Positions.Get(rule->getStart());
        return Positions.Get(dynamic_cast<antlr4::tree::TerminalNode*>(node)->getSymbol());
    }
    std::vector<TRule*> Rules(TTree* node) const {
        std::vector<TRule*> result;
        for (auto* child : node->children) if (auto* rule = dynamic_cast<TRule*>(child)) result.push_back(rule);
        return result;
    }
    std::vector<TRule*> Children(TTree* node, std::string_view name) const {
        std::vector<TRule*> result;
        for (auto* rule : Rules(node)) if (Name(rule) == name) result.push_back(rule);
        return result;
    }
    TRule* Child(TTree* node, std::string_view name) const {
        for (auto* rule : Rules(node)) if (Name(rule) == name) return rule;
        return nullptr;
    }
    bool Has(TTree* node, std::string_view token) const {
        if (!node) return false;
        for (auto* child : node->children)
            if (Name(child).empty() && Upper(child->getText()) == token) return true;
        return false;
    }
    TRule* Only(TTree* node) const {
        const auto rules = Rules(node);
        if (rules.size() != 1) Unsupported(node, EFeature::Syntax, "unsupported " + std::string(Name(node)));
        return rules.front();
    }
    [[noreturn]] void Unsupported(TTree* node, EFeature feature, std::string message) const {
        throw TUnsupportedFeature(feature, Position(node), std::move(message));
    }
    TSyntaxId Add(ESyntaxKind kind, TTree* node, std::string text = {}, std::vector<TSyntaxId> args = {}) {
        if (Result.Expressions.size() >= Settings.MaxNodes || Result.Expressions.size() >= NoSyntax)
            throw TResourceLimitError(Position(node), "syntax node limit exceeded");
        const auto id = static_cast<TSyntaxId>(Result.Expressions.size());
        Result.Expressions.push_back({kind, Position(node), std::move(text), std::move(args), {}, false});
        return id;
    }
    std::string Decode(std::string text, TTree* node, bool identifier = false) const {
        if (text.empty()) return text;
        if (text.starts_with("@@")) {
            if (!text.ends_with("@@")) Unsupported(node, EFeature::Syntax, "multiline string suffix");
            return text.substr(2, text.size() - 4);
        }
        const char quote = text.front();
        if (quote != '\'' && quote != '"' && quote != char(96)) return text;
        if (text.back() != quote) Unsupported(node, EFeature::Syntax, "typed string literal");
        std::string result;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            char c = text[i];
            if (c == quote && i + 2 < text.size() && text[i + 1] == quote) ++i;
            else if (c == '\\' && (!Settings.AnsiQuotes || (identifier && quote == char(96)))) {
                c = text[++i];
                switch (c) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case '0': c = '\0'; break;
                    case '\\': case '\'': case '"': case char(96): break;
                    default: Unsupported(node, EFeature::Syntax, "string escape outside native profile");
                }
            }
            result += c;
        }
        return result;
    }
    std::string Identifier(std::string text, TTree* node) const {
        if (!text.empty() && (text.front() == '\'' || text.front() == '"' || text.starts_with("@@")))
            Unsupported(node, EFeature::Syntax, "string literals as identifiers are outside native-read-v1; use backticks");
        return Decode(std::move(text), node, true);
    }
    std::string Id(TTree* node) const { return Identifier(node->getText(), node); }
    std::string Variable(TTree* node) const {
        auto text = node->getText();
        return Identifier(text.substr(1), node);
    }
    std::string Type(TTree* node) const {
        if (Name(node) == "type_name_or_bind") {
            if (auto* type = Child(node, "type_name")) return Type(type);
            Unsupported(node, EFeature::Type, "type-valued bindings");
        }
        if (Name(node) == "type_name_optional") return Type(Child(node, "type_name_or_bind")) + "?";
        if (Name(node) == "type_name_composite") {
            auto* inner = Only(node);
            if (Name(inner) != "type_name_optional") Unsupported(node, EFeature::Type, "container types");
            auto result = Type(inner);
            for (auto* child : node->children) if (child->getText() == "?") result += '?';
            return result;
        }
        if (Name(node) == "type_name") {
            auto result = Type(Only(node));
            for (auto* child : node->children) if (child->getText() == "?") result += '?';
            return result;
        }
        if (Name(node) == "type_name_decimal") {
            for (auto* arg : Children(node, "integer_or_bind"))
                if (Child(arg, "bind_parameter")) Unsupported(node, EFeature::Type, "dynamic Decimal type");
        }
        return node->getText();
    }
    TRule* Unwrap(TTree* node) const {
        while (Name(node) != "tuple_or_expr" && Name(node) != "lambda" && Name(node) != "select_kind_partial") {
            TTree* next = nullptr;
            for (auto* child : node->children) {
                if (child->getText().empty()) continue;
                if (next) return dynamic_cast<TRule*>(node);
                next = child;
            }
            if (!next || Name(next).empty()) break;
            node = next;
        }
        return dynamic_cast<TRule*>(node);
    }
    TRule* Tuple(TTree* node) const {
        auto* current = Unwrap(node);
        if (!current || Name(current) != "tuple_or_expr") Unsupported(node, EFeature::Subquery, "subquery or tuple");
        return current;
    }
    std::vector<TSyntaxId> TupleItems(TTree* node) {
        auto* tuple = Tuple(node);
        if (Child(tuple, "an_id_or_type")) Unsupported(tuple, EFeature::Expression, "named tuple");
        std::vector<TSyntaxId> args{Expr(Child(tuple, "expr"))};
        for (auto* named : Children(tuple, "named_expr")) {
            if (Child(named, "an_id_or_type")) Unsupported(named, EFeature::Expression, "named tuple");
            args.push_back(Expr(Child(named, "expr")));
        }
        return args;
    }
    TSyntaxId Parenthesis(TTree* node) {
        auto* sub = Child(node, "select_subexpr");
        if (!sub) Unsupported(node, EFeature::Type, "empty tuple");
        auto* unwrapped = Unwrap(sub);
        if (Name(unwrapped) == "tuple_or_expr") {
            auto args = TupleItems(sub);
            if (args.size() != 1 || Has(unwrapped, ",")) Unsupported(node, EFeature::Type, "tuple value");
            return args.front();
        }
        auto query = Select(sub);
        const auto id = Add(ESyntaxKind::ScalarSubquery, node);
        Result.Expressions[id].Query = std::move(query);
        return id;
    }
    TSyntaxId Expr(TTree* node) {
        if (!node) throw std::logic_error("missing grammar expression");
        const auto name = Name(node);
        const auto rules = Rules(node);
        if (name == "literal_value") {
            if (Child(node, "integer")) return Add(ESyntaxKind::Integer, node, node->getText());
            if (Child(node, "real")) return Add(ESyntaxKind::Real, node, node->getText());
            if (Has(node, "NULL")) return Add(ESyntaxKind::Null, node);
            if (Child(node, "bool_value")) return Add(ESyntaxKind::Boolean, node, Upper(node->getText()) == "TRUE" ? "true" : "false");
            auto text = node->getText();
            if (!text.empty() && (text[0] == '\'' || text[0] == '"' || text.starts_with("@@")))
                return Add(ESyntaxKind::String, node, Decode(text, node));
            Unsupported(node, EFeature::Expression, "literal " + text);
        }
        if (name == "bind_parameter") return Add(ESyntaxKind::Variable, node, Variable(node));
        if (name == "id_expr" || name == "id_expr_in") return Add(ESyntaxKind::Column, node, Id(node));
        if (name == "cast_expr") return Add(ESyntaxKind::Cast, node, Type(Child(node, "type_name_or_bind")), {Expr(Child(node, "expr"))});
        if (name == "exists_expr") {
            const auto id = Add(ESyntaxKind::Exists, node);
            Result.Expressions[id].Query = Select(Child(node, "select_stmt"));
            return id;
        }
        if (name == "case_expr") {
            const auto expressions = Children(node, "expr");
            const bool simple = !node->children.empty() && node->children.size() > 1 && Name(node->children[1]) == "expr";
            const auto value = simple ? Expr(expressions.front()) : NoSyntax;
            std::vector<TSyntaxId> args;
            for (auto* when : Children(node, "when_expr")) {
                const auto pair = Children(when, "expr");
                auto condition = Expr(pair[0]);
                if (simple) condition = Add(ESyntaxKind::Binary, when, "==", {value, condition});
                args.push_back(condition); args.push_back(Expr(pair[1]));
            }
            const bool hasElse = Has(node, "ELSE");
            args.push_back(hasElse ? Expr(expressions.back()) : Add(ESyntaxKind::Null, node));
            return Add(ESyntaxKind::Case, node, hasElse ? "" : "implicit_else", std::move(args));
        }
        if (name == "lambda") {
            if (Has(node, "->")) Unsupported(node, EFeature::Expression, "lambda outside named binding");
            return Parenthesis(Child(node, "smart_parenthesis"));
        }
        if (name == "atom_expr" || name == "in_atom_expr") {
            if (Has(node, "::")) {
                std::string function;
                for (auto* child : node->children) function += Id(child);
                return Add(ESyntaxKind::Column, node, std::move(function));
            }
            return Expr(Only(node));
        }
        if (name == "unary_casual_subexpr" || name == "in_unary_casual_subexpr") {
            auto value = Expr(rules.front());
            auto* suffix = Child(node, "unary_subexpr_suffix");
            if (Has(suffix, "COLLATE")) Unsupported(suffix, EFeature::Expression, "COLLATE");
            bool member = false;
            for (auto* child : suffix->children) {
                if (child->getText() == ".") { member = true; continue; }
                if (Name(child) == "invoke_expr") {
                    if (Result.Expressions[value].Kind != ESyntaxKind::Column && Result.Expressions[value].Kind != ESyntaxKind::Variable)
                        Unsupported(child, EFeature::Expression, "higher-order invocation");
                    auto* tail = Child(child, "invoke_expr_tail");
                    if (tail && !tail->getText().empty()) Unsupported(tail, EFeature::Window, "window or filtered function");
                    std::vector<TSyntaxId> args;
                    if (Has(child, "*")) args.push_back(Add(ESyntaxKind::Star, child));
                    if (auto* list = Child(child, "named_expr_list")) {
                        for (auto* arg : Children(list, "named_expr")) {
                            if (Child(arg, "an_id_or_type")) Unsupported(arg, EFeature::Expression, "named argument");
                            args.push_back(Expr(Child(arg, "expr")));
                        }
                    }
                    auto function = Result.Expressions[value].Text;
                    if (Result.Expressions[value].Kind == ESyntaxKind::Variable) function = "$" + function;
                    value = Add(ESyntaxKind::Call, node, std::move(function), std::move(args));
                    if (auto* q = Child(child, "opt_set_quantifier")) Result.Expressions[value].Distinct = Has(q, "DISTINCT");
                } else if (member) {
                    if (Result.Expressions[value].Kind != ESyntaxKind::Column) Unsupported(child, EFeature::Type, "member access on a value");
                    Result.Expressions[value].Text += "." + Id(child);
                    member = false;
                } else Unsupported(child, EFeature::Expression, "subscript or invocation suffix");
            }
            return value;
        }
        if (name == "con_subexpr" && rules.size() == 2)
            return Add(ESyntaxKind::Unary, node, Upper(rules[0]->getText()), {Expr(rules[1])});
        if (name == "xor_subexpr" && rules.size() == 2) {
            auto left = Expr(rules[0]);
            auto* condition = rules[1];
            const auto arguments = Children(condition, "eq_subexpr");
            const bool negate = Has(condition, "NOT");
            if (Has(condition, "ISNULL") || Has(condition, "NOTNULL") || Has(condition, "NULL")) {
                return Add(ESyntaxKind::Unary, condition, negate || Has(condition, "NOTNULL") ? "IS NOT NULL" : "IS NULL", {left});
            }
            if (Has(condition, "BETWEEN")) {
                if (Has(condition, "SYMMETRIC")) Unsupported(condition, EFeature::Expression, "BETWEEN SYMMETRIC");
                left = Add(ESyntaxKind::Between, condition, {}, {left, Expr(arguments[0]), Expr(arguments[1])});
            } else if (auto* in = Child(condition, "in_expr")) {
                auto* lambda = Unwrap(in);
                if (Name(lambda) != "lambda" || Has(lambda, "->")) Unsupported(in, EFeature::Expression, "IN requires a literal expression list");
                auto* paren = Child(lambda, "smart_parenthesis");
                auto* sub = Child(paren, "select_subexpr");
                if (!sub) Unsupported(in, EFeature::Expression, "empty IN list");
                std::vector<TSyntaxId> args{left};
                if (Name(Unwrap(sub)) == "tuple_or_expr") {
                    auto list = TupleItems(sub); args.insert(args.end(), list.begin(), list.end());
                    left = Add(ESyntaxKind::In, condition, {}, std::move(args));
                } else {
                    auto query = Select(sub);
                    left = Add(ESyntaxKind::In, condition, {}, std::move(args));
                    Result.Expressions[left].Query = std::move(query);
                }
            } else if (auto* match = Child(condition, "match_op")) {
                if (Has(condition, "ESCAPE")) Unsupported(condition, EFeature::Expression, "LIKE ESCAPE");
                left = Add(ESyntaxKind::Binary, condition, Upper(match->getText()), {left, Expr(arguments[0])});
            } else {
                if (arguments.size() != 1 || Child(condition, "distinct_from_op"))
                    Unsupported(condition, EFeature::Expression, "chained or null-safe equality");
                left = Add(ESyntaxKind::Binary, condition, Upper(condition->children.front()->getText()), {left, Expr(arguments[0])});
                return left;
            }
            return negate ? Add(ESyntaxKind::Unary, condition, "NOT", {left}) : left;
        }
        if (name == "expr" || name == "or_subexpr" || name == "and_subexpr" || name == "xor_subexpr" ||
            name == "eq_subexpr" || name == "neq_subexpr" || name == "bit_subexpr" ||
            name == "add_subexpr" || name == "mul_subexpr" || name == "con_subexpr" ||
            name == "unary_subexpr" || name == "in_unary_subexpr") {
            if (rules.size() == 1 && node->children.size() == 1) return Expr(rules[0]);
            if (name == "eq_subexpr" && rules.size() > 2)
                Unsupported(node, EFeature::Expression, "chained ordering comparison");
            TSyntaxId value = NoSyntax;
            std::string op;
            for (auto* child : node->children) {
                const auto rule = Name(child);
                if (rule.empty() || rule == "double_question" || rule == "shift_right" || rule == "rot_right") {
                    op += Upper(child->getText()); continue;
                }
                auto arg = Expr(child);
                value = value == NoSyntax ? arg : Add(ESyntaxKind::Binary, node, std::move(op), {value, arg});
                op.clear();
            }
            if (!op.empty()) Unsupported(node, EFeature::Type, "optional type expression");
            return value;
        }
        Unsupported(node, EFeature::Expression, "expression form " + std::string(name));
    }

    TNamedExpr Named(TTree* node) {
        auto* expression = Child(node, "expr");
        if (!expression) {
            auto* prefix = Child(node, "opt_id_prefix");
            auto text = prefix ? prefix->getText() : std::string{};
            if (!text.empty()) text = Id(Only(prefix)) + ".";
            return {Add(ESyntaxKind::Star, node, text.empty() ? "" : text.substr(0, text.size() - 1)), {}};
        }
        auto* alias = Child(node, "an_id_or_type");
        if (!alias) alias = Child(node, "an_id_as_compat");
        return {Expr(expression), alias ? Id(alias) : ""};
    }
    TSource Source(TTree* node) {
        if (Name(node) == "flatten_source") {
            if (Has(node, "FLATTEN")) Unsupported(node, EFeature::Type, "FLATTEN");
            return Source(Child(node, "named_single_source"));
        }
        for (auto name : {"row_pattern_recognition_clause", "pure_column_list", "sample_clause", "tablesample_clause"})
            if (Child(node, name)) Unsupported(node, EFeature::Backend, name);
        auto* hinted = Child(node, "hinted_single_source");
        if (Child(hinted, "table_hints")) Unsupported(hinted, EFeature::Backend, "table hints");
        auto* single = Child(hinted, "single_source");
        TSource result; result.Position = Position(node);
        auto* alias = Child(node, "an_id"); if (!alias) alias = Child(node, "an_id_as_compat");
        if (alias) result.Alias = Id(alias);
        if (auto* query = Child(single, "select_stmt")) result.Query = Select(query);
        else if (auto* table = Child(single, "table_ref")) {
            if (Child(table, "cluster_expr") || Has(table, "@") || Has(table, "(") || Has(table, "VIEW"))
                Unsupported(table, EFeature::Backend, "qualified, temporary, or callable source");
            if (auto* binding = Child(table, "bind_parameter")) result.Name = "$" + Variable(binding);
            else if (auto* key = Child(table, "table_key")) {
                if (Has(key, "VIEW")) Unsupported(key, EFeature::Backend, "index view");
                result.Name = Id(Child(key, "id_table_or_type"));
            } else Unsupported(table, EFeature::Backend, "table source");
        } else Unsupported(single, EFeature::Statement, "VALUES source");
        return result;
    }
    void From(TTree* node, TSelect& result) {
        if (Has(node, "ANY")) Unsupported(node, EFeature::Backend, "ANY join");
        std::optional<TJoin> join;
        for (auto* child : Rules(node)) {
            if (Name(child) == "join_op") {
                if (join) result.Joins.push_back(std::move(*join));
                const auto text = Upper(child->getText());
                EJoinKind kind = EJoinKind::Inner;
                if (Has(child, "NATURAL") || Has(child, "EXCLUSION")) Unsupported(child, EFeature::Backend, text);
                if (Has(child, ",") || Has(child, "CROSS")) kind = EJoinKind::Cross;
                else if (Has(child, "LEFT")) kind = Has(child, "SEMI") ? EJoinKind::LeftSemi : Has(child, "ONLY") ? EJoinKind::LeftOnly : EJoinKind::Left;
                else if (Has(child, "RIGHT")) kind = Has(child, "SEMI") ? EJoinKind::RightSemi : Has(child, "ONLY") ? EJoinKind::RightOnly : EJoinKind::Right;
                else if (Has(child, "FULL")) kind = EJoinKind::Full;
                join.emplace(TJoin{kind, {}});
            } else if (Name(child) == "flatten_source") {
                if (join) join->Source = Source(child); else result.Source = Source(child);
            } else if (Name(child) == "join_constraint") {
                if (Has(child, "USING")) Unsupported(child, EFeature::JoinUsing, "JOIN USING");
                join->Predicate = Expr(Child(child, "expr"));
            }
        }
        if (join) result.Joins.push_back(std::move(*join));
    }
    std::shared_ptr<TSelect> Select(TTree* node) {
        if (!node) throw std::logic_error("missing select grammar node");
        const auto name = Name(node);
        if (name == "select_stmt" || name == "select_unparenthesized_stmt" || name == "select_subexpr") {
            auto* core = Rules(node).back();
            auto result = Select(core);
            if (auto* with = Child(node, "cte_with_clause")) {
                for (auto* binding : Children(with, "cte_binding")) {
                    auto* key = Child(binding, "cte_key");
                    if (Has(binding, "RECURSIVE") || Child(key, "pure_column_list")) Unsupported(binding, EFeature::Subquery, "recursive or explicitly named CTE");
                    auto* value = Child(binding, "cte_value");
                    if (!Child(value, "select_stmt")) Unsupported(value, EFeature::Statement, "VALUES CTE");
                    result->Ctes.emplace_back(Id(Child(key, "id_table_or_type")), Select(Child(value, "select_stmt")));
                }
            }
            return result;
        }
        if (name.ends_with("_core") && name != "select_core") {
            auto children = Rules(node);
            auto result = Select(children.front());
            for (std::size_t i = 1; i < children.size(); i += 2) {
                if (Name(children[i]) != "union_op" || !Has(children[i], "UNION") || !Has(children[i], "ALL"))
                    Unsupported(children[i], EFeature::SetOperation, "only UNION ALL is native");
                result->UnionAll.push_back(Select(children[i + 1]));
            }
            if (!result->UnionAll.empty()) {
                if (!result->OrderBy.empty() || result->Limit != NoSyntax) Unsupported(node, EFeature::SetOperation, "ordered or limited UNION branch");
                for (const auto& branch : result->UnionAll)
                    if (!branch->OrderBy.empty() || branch->Limit != NoSyntax) Unsupported(node, EFeature::SetOperation, "ordered or limited UNION branch");
            }
            return result;
        }
        if (name.ends_with("_intersect")) {
            if (Rules(node).size() != 1) Unsupported(node, EFeature::SetOperation, "INTERSECT");
            return Select(Only(node));
        }
        if (name == "select_kind_parenthesis" || name == "select_or_expr") return Select(Only(node));
        if (name == "select_kind_partial") {
            auto result = Select(Child(node, "select_kind"));
            auto counts = Children(node, "expr");
            if (!counts.empty()) result->Limit = Expr(counts[0]);
            if (counts.size() > 1) {
                result->Offset = Expr(counts[1]);
                if (Has(node, ",")) std::swap(result->Limit, result->Offset);
            }
            return result;
        }
        if (name == "select_kind") {
            if (Has(node, "DISCARD") || Has(node, "INTO")) Unsupported(node, EFeature::Statement, "DISCARD or INTO RESULT");
            if (auto* core = Child(node, "select_core")) return Select(core);
            Unsupported(node, EFeature::Statement, "PROCESS, REDUCE, or COMBINE");
        }
        if (name != "select_core") Unsupported(node, EFeature::Subquery, "select form " + std::string(name));
        if (Has(node, "STREAM") || Has(node, "WITHOUT")) Unsupported(node, EFeature::Statement, "STREAM or WITHOUT");
        if (Child(node, "window_clause")) Unsupported(node, EFeature::Window, "WINDOW clause");
        auto result = std::make_shared<TSelect>(); result->Position = Position(node);
        result->Distinct = Has(Child(node, "opt_set_quantifier"), "DISTINCT");
        for (auto* column : Children(node, "result_column")) result->Projection.push_back(Named(column));
        const auto sources = Children(node, "join_source");
        if (sources.size() > 1) throw TFrontendError(Position(node), "duplicate FROM clause");
        if (!sources.empty()) From(sources[0], *result);
        std::string clause;
        for (auto* child : node->children) {
            if (Name(child).empty()) clause = Upper(child->getText());
            else if (Name(child) == "expr") {
                if (clause == "WHERE") result->Where = Expr(child);
                else if (clause == "HAVING") result->Having = Expr(child);
                else throw std::logic_error("unknown SELECT expression clause");
            }
        }
        if (auto* group = Child(node, "group_by_clause")) {
            if (Has(group, "COMPACT") || Has(group, "WITH") || !Child(group, "opt_set_quantifier")->getText().empty())
                Unsupported(group, EFeature::GroupingSets, "GROUP BY modifier");
            for (auto* element : Children(Child(group, "grouping_element_list"), "grouping_element")) {
                if (auto* ordinary = Child(element, "ordinary_grouping_set")) result->GroupBy.push_back(Named(Child(ordinary, "named_expr")));
                else Unsupported(element, EFeature::GroupingSets, "grouping sets, rollup, cube, or hopping window");
            }
        }
        if (auto* order = Child(node, "ext_order_by_clause")) {
            if (Has(order, "ASSUME")) Unsupported(order, EFeature::Statement, "ASSUME ORDER BY");
            auto* list = Child(Child(order, "order_by_clause"), "sort_specification_list");
            for (auto* key : Children(list, "sort_specification"))
                result->OrderBy.push_back({Expr(Child(key, "expr")), Has(key, "DESC")});
        }
        return result;
    }
    TStatement Statement(TTree* node) {
        const auto name = Name(node);
        if (name == "select_stmt") {
            TStatement result{EStatementKind::Query, Position(node)}; result.Query = Select(node); return result;
        }
        if (name == "declare_stmt") {
            if (Has(node, "=")) Unsupported(node, EFeature::Statement, "DECLARE default");
            TStatement result{EStatementKind::Declaration, Position(node)};
            result.Name = Variable(Child(node, "bind_parameter")); result.Value = Type(Child(node, "type_name")); return result;
        }
        if (name == "pragma_stmt") {
            TStatement result{EStatementKind::Pragma, Position(node)};
            result.Name = Child(node, "opt_id_prefix_or_type")->getText() + Id(Child(node, "an_id"));
            auto values = Children(node, "pragma_value");
            if (values.size() > 1 || Has(node, "(")) Unsupported(node, EFeature::Pragma, "pragma argument list");
            if (!values.empty()) result.Value = Decode(values[0]->getText(), values[0]);
            return result;
        }
        if (name == "named_nodes_stmt") {
            auto names = Children(Child(node, "bind_parameter_list"), "bind_parameter");
            if (names.size() != 1) Unsupported(node, EFeature::Statement, "multiple binding assignment");
            TStatement result{EStatementKind::Binding, Position(node)}; result.Name = Variable(names[0]);
            if (auto* select = Child(node, "select_unparenthesized_stmt")) result.Query = Select(select);
            else {
                auto* expression = Child(node, "expr");
                auto* lambda = Unwrap(expression);
                if (Name(lambda) == "lambda" && Has(lambda, "->")) {
                    result.Lambda = true;
                    auto* paren = Child(lambda, "smart_parenthesis");
                    if (auto* sub = Child(paren, "select_subexpr")) {
                        auto params = TupleItems(sub);
                        for (auto param : params) {
                            if (Result.Expressions[param].Kind != ESyntaxKind::Variable) Unsupported(paren, EFeature::Expression, "lambda parameter");
                            result.Parameters.push_back(Result.Expressions[param].Text);
                        }
                    }
                    if (auto* body = Child(lambda, "lambda_body")) {
                        if (!Children(body, "lambda_stmt").empty()) Unsupported(body, EFeature::Expression, "lambda local statements");
                        result.Expression = Expr(Child(body, "expr"));
                    } else result.Expression = Expr(Child(lambda, "expr"));
                } else {
                    result.Expression = Expr(expression);
                    if (Result.Expressions[result.Expression].Kind == ESyntaxKind::ScalarSubquery) {
                        result.Query = Result.Expressions[result.Expression].Query;
                        result.Expression = NoSyntax;
                    }
                }
            }
            return result;
        }
        Unsupported(node, EFeature::Statement, "statement " + std::string(name));
    }
    const std::vector<std::string>& Names;
    const TPositions& Positions;
    const TParseSettings& Settings;
    TParsedQuery Result;
};

template<class TLexer>
class TBoundedLexer : public TLexer {
public:
    TBoundedLexer(antlr4::CharStream* input, const TParseSettings& settings, const TPositions& positions)
        : TLexer(input), Settings(settings), Positions(positions) {}
    std::unique_ptr<antlr4::Token> nextToken() override {
        auto token = TLexer::nextToken();
        if (++Tokens / 8 > Settings.MaxNodes)
            throw TResourceLimitError(Positions.Get(token.get()), "token limit exceeded");
        return token;
    }
private:
    const TParseSettings& Settings;
    const TPositions& Positions;
    std::size_t Tokens = 0;
};

template<class TLexer, class TParser>
TParsedQuery Run(std::string_view sql, const TParseSettings& settings, bool translate) {
    if (sql.size() > settings.MaxQueryBytes) throw TResourceLimitError({}, "query size limit exceeded");
    TPositions positions(sql);
    TErrors errors(positions);
    antlr4::ANTLRInputStream input;
    try {
        input.load(sql.data(), sql.size(), false);
    } catch (const antlr4::IllegalArgumentException&) {
        throw TFrontendError({}, "invalid UTF-8 input");
    }
    TBoundedLexer<TLexer> lexer(&input, settings, positions);
    lexer.removeErrorListeners(); lexer.addErrorListener(&errors);
    antlr4::CommonTokenStream tokens(&lexer);
    TParser parser(&tokens);
    parser.removeErrorListeners(); parser.addErrorListener(&errors);
    TLimits limits(settings, positions);
    parser.addParseListener(&limits);
    auto* root = parser.sql_query();
    if (parser.getNumberOfSyntaxErrors()) throw TFrontendError({}, "invalid SQL");
    return translate ? TTranslate(parser.getRuleNames(), positions, settings).Run(root) : TParsedQuery{};
}

TParsedQuery Run(std::string_view sql, const TParseSettings& settings, bool translate) {
    if (settings.AnsiQuotes) return Run<NativeRboAnsi::SQLv1Antlr4Lexer, NativeRboAnsi::SQLv1Antlr4Parser>(sql, settings, translate);
    return Run<NativeRboDefault::SQLv1Antlr4Lexer, NativeRboDefault::SQLv1Antlr4Parser>(sql, settings, translate);
}
} // namespace

void ValidateGrammar(std::string_view sql, const TParseSettings& settings) { Run(sql, settings, false); }
TParsedQuery ParseGrammar(std::string_view sql, const TParseSettings& settings) { return Run(sql, settings, true); }

std::string SyntaxFingerprint(const TParsedQuery& query) {
    std::ostringstream out;
    auto text = [&](std::string_view value) { out << value.size() << ':' << value; };
    std::function<void(const TSelect&)> select;
    std::function<void(TSyntaxId)> expr = [&](TSyntaxId id) {
        if (id == NoSyntax) { out << '-'; return; }
        const auto& node = query.Expressions.at(id);
        out << '(' << static_cast<int>(node.Kind) << ',' << node.Distinct;
        text(node.Text);
        for (auto arg : node.Arguments) expr(arg);
        if (node.Query) select(*node.Query);
        out << ')';
    };
    auto source = [&](const TSource& value) {
        text(value.Name); text(value.Alias); if (value.Query) select(*value.Query);
    };
    auto named = [&](const std::vector<TNamedExpr>& values) {
        out << '['; for (const auto& value : values) { expr(value.Expression); text(value.Alias); } out << ']';
    };
    select = [&](const TSelect& value) {
        out << '{' << value.Distinct;
        for (const auto& [name, cte] : value.Ctes) { text(name); select(*cte); }
        if (value.Source) source(*value.Source);
        for (const auto& join : value.Joins) {
            out << static_cast<int>(join.Kind); source(join.Source); expr(join.Predicate);
            for (const auto& column : join.Using) text(column);
        }
        named(value.Projection); expr(value.Where); named(value.GroupBy); expr(value.Having);
        for (const auto& key : value.OrderBy) { out << key.Descending; expr(key.Expression); }
        expr(value.Limit); expr(value.Offset);
        for (const auto& branch : value.UnionAll) select(*branch);
        out << '}';
    };
    for (const auto& statement : query.Statements) {
        out << static_cast<int>(statement.Kind) << ':' << statement.Lambda;
        text(statement.Name); text(statement.Value);
        for (const auto& parameter : statement.Parameters) text(parameter);
        expr(statement.Expression); if (statement.Query) select(*statement.Query);
    }
    return out.str();
}
} // namespace NKikimr::NKqp::NFrontend
