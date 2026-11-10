#include "parser.h"

#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboDefaultLexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboDefaultParser.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboAnsiLexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboAnsiParser.h>

#include <algorithm>
#include <sstream>
#include <utility>

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

    TParsedQuery RunExpression(TRule* root, TSyntaxId& expression) {
        expression = Expr(root);
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
        // The grammar has already selected an identifier position (for
        // example AS or FROM). Quoted spellings here name objects; ordinary
        // expression string literals still go through the literal production.
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
        if (Name(node) == "type_name_list") return "List<" + Type(Child(node, "type_name_or_bind")) + ">";
        if (Name(node) == "type_name_composite") {
            auto* inner = Only(node);
            if (Name(inner) != "type_name_optional" && Name(inner) != "type_name_list")
                Unsupported(node, EFeature::Type, "container types");
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
    TTree* TypeForm(TTree* node) const {
        while (Name(node) == "type_name" || Name(node) == "type_name_composite" || Name(node) == "type_name_or_bind") {
            if (Has(node, "?") || Child(node, "bind_parameter")) return node;
            node = Only(node);
        }
        return node;
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
        if (!sub) return Add(ESyntaxKind::Call, node, "AsTuple");
        auto* unwrapped = Unwrap(sub);
        if (Name(unwrapped) == "tuple_or_expr") {
            auto args = TupleItems(sub);
            if (args.size() != 1 || Has(unwrapped, ",")) return Add(ESyntaxKind::Call, node, "AsTuple", std::move(args));
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
            if (!text.empty() && (text[0] == '\'' || text[0] == '"' || text.starts_with("@@"))) {
                const auto suffix = Upper(text.substr(text.size() - 1));
                if (suffix == "U" || suffix == "S") text.pop_back();
                const auto value = Add(ESyntaxKind::String, node, Decode(text, node));
                return suffix == "U" ? Add(ESyntaxKind::Call, node, "Utf8", {value}) : value;
            }
            Unsupported(node, EFeature::Expression, "literal " + text);
        }
        if (name == "list_literal") {
            std::vector<TSyntaxId> args;
            if (auto* list = Child(node, "expr_list")) for (auto* item : Children(list, "expr")) args.push_back(Expr(item));
            return Add(ESyntaxKind::Call, node, "AsList", std::move(args));
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
        if (name == "json_api_expr") return Expr(Only(node));
        if (name == "json_value") {
            auto* common = Child(node, "json_common_args");
            if (Has(common, "PASSING") || Has(node, "RETURNING") || !Children(node, "json_case_handler").empty())
                Unsupported(node, EFeature::Expression, "JSON_VALUE PASSING, RETURNING and handlers");
            auto* path = Child(common, "jsonpath_spec");
            auto input = Expr(Child(common, "expr"));
            return Add(ESyntaxKind::JsonValue, node, Decode(path->getText(), path), {input});
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
                    const bool named = Result.Expressions[value].Kind == ESyntaxKind::Column ||
                        Result.Expressions[value].Kind == ESyntaxKind::Variable;
                    auto* tail = Child(child, "invoke_expr_tail");
                    if (tail && (Child(tail, "null_treatment") || Child(tail, "filter_clause")))
                        Unsupported(tail, EFeature::Window, "window NULL treatment or FILTER clause");
                    std::vector<TSyntaxId> args;
                    if (Has(child, "*")) args.push_back(Add(ESyntaxKind::Star, child));
                    if (auto* list = Child(child, "named_expr_list")) {
                        for (auto* arg : Children(list, "named_expr")) {
                            if (Child(arg, "an_id_or_type")) Unsupported(arg, EFeature::Expression, "named argument");
                            args.push_back(Expr(Child(arg, "expr")));
                        }
                    }
                    auto function = named ? Result.Expressions[value].Text : std::string{};
                    if (Result.Expressions[value].Kind == ESyntaxKind::Variable) function = "$" + function;
                    auto window = tail && Child(tail, "window_name_or_specification") ?
                        Window(Child(tail, "window_name_or_specification")) : nullptr;
                    if (!named) args.insert(args.begin(), value);
                    value = Add(named ? ESyntaxKind::Call : ESyntaxKind::Invoke, node, std::move(function), std::move(args));
                    Result.Expressions[value].Window = std::move(window);
                    if (auto* q = Child(child, "opt_set_quantifier")) Result.Expressions[value].Distinct = Has(q, "DISTINCT");
                } else if (Name(child) == "key_expr") {
                    const auto index = Expr(Child(child, "expr"));
                    value = Add(ESyntaxKind::Index, child, {}, {value, index});
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
                std::vector<TSyntaxId> args{left};
                if (Name(lambda) == "list_literal") {
                    if (auto* list = Child(lambda, "expr_list"))
                        for (auto* item : Children(list, "expr")) args.push_back(Expr(item));
                    left = Add(ESyntaxKind::In, condition, "list", std::move(args));
                } else if (Name(lambda) != "lambda") {
                    left = Add(ESyntaxKind::In, condition, "collection", {left, Expr(in)});
                } else {
                    if (Has(lambda, "->")) Unsupported(in, EFeature::Expression, "IN does not accept a lambda");
                    auto* paren = Child(lambda, "smart_parenthesis");
                    auto* sub = Child(paren, "select_subexpr");
                    if (!sub) Unsupported(in, EFeature::Expression, "empty IN list");
                    if (Name(Unwrap(sub)) == "tuple_or_expr") {
                        auto list = TupleItems(sub);
                        std::shared_ptr<TSelect> query;
                        if (list.size() == 1 && !Has(Unwrap(sub), ",") && Result.Expressions[list[0]].Kind == ESyntaxKind::ScalarSubquery)
                            query = Result.Expressions[list[0]].Query;
                        else args.insert(args.end(), list.begin(), list.end());
                        left = Add(ESyntaxKind::In, condition, {}, std::move(args));
                        Result.Expressions[left].Query = std::move(query);
                    } else {
                        auto query = Select(sub);
                        left = Add(ESyntaxKind::In, condition, {}, std::move(args));
                        Result.Expressions[left].Query = std::move(query);
                    }
                }
            } else if (auto* match = Child(condition, "match_op")) {
                std::vector<TSyntaxId> args{left, Expr(arguments[0])};
                if (Has(condition, "ESCAPE")) args.push_back(Expr(arguments.at(1)));
                left = Add(ESyntaxKind::Binary, condition, Upper(match->getText()), std::move(args));
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
            name == "unary_subexpr" || name == "in_unary_subexpr" || name == "in_expr") {
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

    TGroupingSets GroupingValue(TTree* expression, std::string alias, TSelect& query, bool& composite) {
        auto* unwrapped = Unwrap(expression);
        if (Name(unwrapped) == "lambda" && !Has(unwrapped, "->")) unwrapped = Child(unwrapped, "smart_parenthesis");
        if (Name(unwrapped) == "smart_parenthesis") {
            auto* sub = Child(unwrapped, "select_subexpr");
            auto* tuple = sub ? Unwrap(sub) : nullptr;
            if (!sub || (Name(tuple) == "tuple_or_expr" && Has(tuple, ","))) {
                composite = true;
                if (!alias.empty()) Unsupported(expression, EFeature::GroupingSets, "alias on a grouping bundle");
                std::vector<TGroupingSets> elements;
                if (tuple) {
                    auto* name = Child(tuple, "an_id_or_type");
                    elements.push_back(GroupingValue(Child(tuple, "expr"), name ? Id(name) : "", query, composite));
                    for (auto* named : Children(tuple, "named_expr")) {
                        auto* name = Child(named, "an_id_or_type");
                        elements.push_back(GroupingValue(Child(named, "expr"), name ? Id(name) : "", query, composite));
                    }
                }
                return ExpandGroupingSets(elements, EGroupingOperation::Product, Settings.MaxNodes, Position(expression));
            }
        }
        const auto index = static_cast<std::uint32_t>(query.GroupBy.size());
        query.GroupBy.push_back({Expr(expression), std::move(alias)});
        return {{index}};
    }

    TGroupingSets GroupingElement(TTree* element, TSelect& query, bool& composite) {
        if (auto* ordinary = Child(element, "ordinary_grouping_set")) {
            auto* named = Child(ordinary, "named_expr");
            auto* alias = Child(named, "an_id_or_type");
            return GroupingValue(Child(named, "expr"), alias ? Id(alias) : "", query, composite);
        }
        composite = true;
        auto* form = Only(element);
        const auto name = Name(form);
        std::vector<TGroupingSets> elements;
        EGroupingOperation op;
        if (name == "grouping_sets_specification") {
            op = EGroupingOperation::Sets;
            for (auto* child : Children(Child(form, "grouping_element_list"), "grouping_element"))
                elements.push_back(GroupingElement(child, query, composite));
        } else if (name == "rollup_list" || name == "cube_list") {
            op = name == "rollup_list" ? EGroupingOperation::Rollup : EGroupingOperation::Cube;
            for (auto* child : Children(Child(form, "ordinary_grouping_set_list"), "ordinary_grouping_set")) {
                auto* named = Child(child, "named_expr");
                auto* alias = Child(named, "an_id_or_type");
                elements.push_back(GroupingValue(Child(named, "expr"), alias ? Id(alias) : "", query, composite));
            }
        } else Unsupported(element, EFeature::GroupingSets, "hopping window grouping");
        return ExpandGroupingSets(elements, op, Settings.MaxNodes, Position(element));
    }
    TSource Source(TTree* node) {
        if (Name(node) == "flatten_source") {
            if (Has(node, "FLATTEN")) Unsupported(node, EFeature::Type, "FLATTEN");
            return Source(Child(node, "named_single_source"));
        }
        for (auto name : {"row_pattern_recognition_clause", "sample_clause", "tablesample_clause"})
            if (Child(node, name)) Unsupported(node, EFeature::Backend, name);
        auto* hinted = Child(node, "hinted_single_source");
        if (Child(hinted, "table_hints")) Unsupported(hinted, EFeature::Backend, "table hints");
        auto* single = Child(hinted, "single_source");
        TSource result; result.Position = Position(node);
        auto* alias = Child(node, "an_id"); if (!alias) alias = Child(node, "an_id_as_compat");
        if (alias) result.Alias = Id(alias);
        if (auto* columns = Child(node, "pure_column_list"))
            for (auto* column : Children(columns, "an_id")) result.ColumnAliases.push_back(Id(column));
        if (auto* query = Child(single, "select_stmt")) result.Query = Select(query);
        else if (auto* table = Child(single, "table_ref")) {
            if (Has(table, "(") && Child(table, "an_id_expr") && Upper(Id(Child(table, "an_id_expr"))) == "AS_TABLE") {
                const auto args = Children(table, "table_arg");
                if (Child(table, "cluster_expr") || Has(table, "@") || args.size() != 1 ||
                    Has(args.front(), "@") || Has(args.front(), "VIEW"))
                    Unsupported(table, EFeature::Statement, "AS_TABLE requires one row-list parameter");
                auto* named = Child(args.front(), "named_expr");
                if (Has(named, "AS")) Unsupported(named, EFeature::Statement, "AS_TABLE argument alias");
                const auto argument = Expr(Child(named, "expr"));
                if (Result.Expressions[argument].Kind != ESyntaxKind::Variable)
                    Unsupported(table, EFeature::Statement, "AS_TABLE requires a row-list parameter");
                result.Name = "$" + Result.Expressions[argument].Text;
                result.ParameterTable = true;
                return result;
            }
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
                if (Has(child, "NATURAL")) Unsupported(child, EFeature::Backend, text);
                if (Has(child, ",") || Has(child, "CROSS")) kind = EJoinKind::Cross;
                else if (Has(child, "LEFT")) kind = Has(child, "SEMI") ? EJoinKind::LeftSemi : Has(child, "ONLY") ? EJoinKind::LeftOnly : EJoinKind::Left;
                else if (Has(child, "RIGHT")) kind = Has(child, "SEMI") ? EJoinKind::RightSemi : Has(child, "ONLY") ? EJoinKind::RightOnly : EJoinKind::Right;
                else if (Has(child, "FULL")) kind = EJoinKind::Full;
                else if (Has(child, "EXCLUSION")) kind = EJoinKind::Exclusion;
                else if (Has(child, "OUTER")) Unsupported(child, EFeature::Backend, "OUTER JOIN requires LEFT, RIGHT or FULL");
                join.emplace(TJoin{kind, {}});
            } else if (Name(child) == "flatten_source") {
                if (join) join->Source = Source(child); else result.Source = Source(child);
            } else if (Name(child) == "join_constraint") {
                if (Has(child, "USING")) {
                    for (auto* key : Children(Child(child, "pure_column_or_named_list"), "pure_column_or_named")) {
                        if (Child(key, "bind_parameter")) Unsupported(key, EFeature::JoinUsing, "named USING column");
                        join->Using.push_back(Id(Child(key, "an_id")));
                    }
                } else join->Predicate = Expr(Child(child, "expr"));
            }
        }
        if (join) result.Joins.push_back(std::move(*join));
    }
    std::shared_ptr<TSyntaxWindow> Window(TTree* node) {
        auto* spec = Name(node) == "window_specification" ? node : Child(node, "window_specification");
        if (!spec) {
            const auto name = Id(Child(node, "window_name"));
            const auto found = NamedWindows.find(name);
            if (found == NamedWindows.end()) throw TFrontendError(Position(node), "unknown window: " + name);
            return found->second;
        }
        auto* details = Child(spec, "window_specification_details");
        if (Child(details, "existing_window_name")) Unsupported(details, EFeature::Window, "inherited window specification");
        auto result = std::make_shared<TSyntaxWindow>();
        if (auto* partition = Child(details, "window_partition_clause")) {
            if (Has(partition, "COMPACT")) Unsupported(partition, EFeature::Window, "compact window partition");
            for (auto* key : Children(Child(partition, "named_expr_list"), "named_expr")) {
                if (Child(key, "an_id_or_type")) Unsupported(key, EFeature::Window, "aliased window partition key");
                result->Partition.push_back(Expr(Child(key, "expr")));
            }
        }
        if (auto* order = Child(details, "window_order_clause")) {
            for (auto* key : Children(Child(Child(order, "order_by_clause"), "sort_specification_list"), "sort_specification"))
                result->Order.push_back({Expr(Child(key, "expr")), Has(key, "DESC")});
        }
        if (auto* frame = Child(details, "window_frame_clause")) {
            if (Child(frame, "window_frame_exclusion")) Unsupported(frame, EFeature::Window, "window frame exclusion");
            auto* units = Child(frame, "window_frame_units");
            result->Frame.Type = Has(units, "ROWS") ? EWindowFrame::Rows : Has(units, "RANGE") ? EWindowFrame::Range : EWindowFrame::Groups;
            auto bound = [&](TTree* node, EWindowBound& kind, TSyntaxId& offset) {
                if (Has(node, "CURRENT")) kind = EWindowBound::CurrentRow;
                else {
                    const bool preceding = Has(node, "PRECEDING");
                    kind = Has(node, "UNBOUNDED") ? (preceding ? EWindowBound::UnboundedPreceding : EWindowBound::UnboundedFollowing) :
                        (preceding ? EWindowBound::Preceding : EWindowBound::Following);
                    if (auto* expr = Child(node, "expr")) offset = Expr(expr);
                }
            };
            auto* extent = Child(frame, "window_frame_extent");
            if (auto* between = Child(extent, "window_frame_between")) {
                const auto bounds = Children(between, "window_frame_bound");
                bound(bounds[0], result->Frame.Begin, result->BeginOffset);
                bound(bounds[1], result->Frame.End, result->EndOffset);
            } else bound(Child(extent, "window_frame_bound"), result->Frame.Begin, result->BeginOffset);
        }
        return result;
    }
    bool ParenthesizedSelect(TTree* node) const {
        if (Name(node) == "select_kind_parenthesis") return Has(node, "(");
        if (Name(node).ends_with("_intersect") && Rules(node).size() == 1) return ParenthesizedSelect(Only(node));
        return false;
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
                    if (Has(binding, "RECURSIVE")) Unsupported(binding, EFeature::Subquery, "recursive CTE");
                    auto* value = Child(binding, "cte_value");
                    if (!Child(value, "select_stmt")) Unsupported(value, EFeature::Statement, "VALUES CTE");
                    TCteBinding cte{Id(Child(key, "id_table_or_type")), {}, Select(Child(value, "select_stmt"))};
                    if (auto* columns = Child(key, "pure_column_list"))
                        for (auto* column : Children(columns, "an_id")) cte.Columns.push_back(Id(column));
                    result->Ctes.push_back(std::move(cte));
                }
            }
            return result;
        }
        if ((name.ends_with("_core") && name != "select_core") || name.ends_with("_intersect")) {
            auto children = Rules(node);
            if (children.size() == 1) return Select(children.front());
            auto result = std::make_shared<TSelect>(); result->Position = Position(node);
            for (std::size_t i = 0; i < children.size(); i += 2) {
                auto input = Select(children[i]);
                if (!ParenthesizedSelect(children[i]) && (!input->OrderBy.empty() || input->Limit != NoSyntax)) {
                    if (i + 1 != children.size()) throw TFrontendError(input->Position, "ORDER BY/LIMIT is only allowed after the last unparenthesized set operand");
                    result->OrderBy = std::move(input->OrderBy);
                    result->Limit = std::exchange(input->Limit, NoSyntax);
                    result->Offset = std::exchange(input->Offset, NoSyntax);
                }
                result->AppendSetInput(std::move(input));
                if (i) {
                    auto* op = children[i - 1];
                    const bool all = Has(op, "ALL");
                    if (Has(op, "UNION")) result->SetOperations.push_back(all ? ESetOperation::UnionAll : ESetOperation::Union);
                    else if (Has(op, "INTERSECT")) result->SetOperations.push_back(all ? ESetOperation::IntersectAll : ESetOperation::Intersect);
                    else if (Has(op, "EXCEPT")) result->SetOperations.push_back(all ? ESetOperation::ExceptAll : ESetOperation::Except);
                    else Unsupported(op, EFeature::SetOperation, "unknown set operation");
                }
            }
            return result;
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
        // Resolve each query's named specifications before its calls. Their
        // expression IDs then precede the calls, preserving the native DAG's
        // topological construction and normal window deduplication.
        auto outerWindows = std::exchange(NamedWindows, {});
        struct TRestoreWindows {
            decltype(NamedWindows)& Current;
            decltype(NamedWindows)& Saved;
            ~TRestoreWindows() { Current = std::move(Saved); }
        } restoreWindows{NamedWindows, outerWindows};
        if (auto* clause = Child(node, "window_clause")) {
            for (auto* definition : Children(Child(clause, "window_definition_list"), "window_definition")) {
                const auto name = Id(Child(definition, "new_window_name"));
                auto window = Window(Child(definition, "window_specification"));
                if (!NamedWindows.emplace(name, std::move(window)).second)
                    throw TFrontendError(Position(definition), "duplicate window: " + name);
            }
        }
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
            bool composite = false;
            std::vector<TGroupingSets> elements;
            for (auto* element : Children(Child(group, "grouping_element_list"), "grouping_element"))
                elements.push_back(GroupingElement(element, *result, composite));
            if (composite) result->GroupingSets = ExpandGroupingSets(elements, EGroupingOperation::Product, Settings.MaxNodes, Position(group));
        }
        if (auto* order = Child(node, "ext_order_by_clause")) {
            if (Has(order, "ASSUME")) Unsupported(order, EFeature::Statement, "ASSUME ORDER BY");
            auto* list = Child(Child(order, "order_by_clause"), "sort_specification_list");
            for (auto* key : Children(list, "sort_specification"))
                result->OrderBy.push_back({Expr(Child(key, "expr")), Has(key, "DESC")});
        }
        return result;
    }
    TWriteStatement Write(TTree* node) {
        TWriteStatement result{};
        const bool into = Name(node) == "into_table_stmt";
        result.Kind = into ? EWriteKind::Insert : Name(node) == "update_stmt" ? EWriteKind::Update : EWriteKind::Delete;
        result.Batch = Has(node, "BATCH");
        auto* simple = Child(node, "simple_table_ref");
        if (into) {
            if (Has(node, "IGNORE")) Unsupported(node, EFeature::Statement, "INSERT OR IGNORE");
            if (Has(node, "UPSERT")) result.Kind = EWriteKind::Upsert;
            if (Has(node, "REPLACE")) result.Kind = EWriteKind::Replace;
            if (Has(node, "REVERT")) result.OnConflict = "revert";
            auto* target = Child(node, "into_simple_table_ref");
            if (Has(target, "ERASE")) Unsupported(target, EFeature::Statement, "ERASE BY");
            simple = Child(target, "simple_table_ref");
        }
        if (Child(simple, "table_hints")) Unsupported(simple, EFeature::Statement, "write table hints");
        auto* core = Child(simple, "simple_table_ref_core");
        auto* object = Child(core, "object_ref");
        if (!object || Child(object, "cluster_expr")) Unsupported(core, EFeature::Statement, "qualified or dynamic write targets");
        auto* id = Child(object, "id_or_at");
        if (Has(id, "@")) Unsupported(id, EFeature::Statement, "anonymous write target");
        result.Target.Position = Position(simple); result.Target.Name = Id(id);
        if (into || Has(node, "ON")) {
            auto* values = Child(node, "into_values_source");
            if (auto* columns = Child(values, "pure_column_list"))
                for (auto* column : Children(columns, "an_id")) result.Columns.push_back(Id(column));
            auto* input = Child(values, "values_source");
            if (!input) Unsupported(values, EFeature::Statement, "DEFAULT VALUES");
            if (auto* select = Child(input, "select_stmt")) result.Input = Select(select);
            else {
                auto* rows = Child(Child(input, "values_stmt"), "values_source_row_list");
                for (auto* row : Children(rows, "values_source_row")) {
                    auto& output = result.Rows.emplace_back();
                    for (auto* expr : Children(Child(row, "expr_list"), "expr")) output.push_back(Expr(expr));
                }
            }
        } else {
            if (auto* where = Child(node, "expr")) result.Where = Expr(where);
            if (result.Kind == EWriteKind::Update) {
                auto* clauses = Child(Child(node, "set_clause_choice"), "set_clause_list");
                if (!clauses) Unsupported(node, EFeature::Statement, "multiple-column UPDATE assignment");
                for (auto* clause : Children(clauses, "set_clause"))
                    result.Assignments.push_back({Expr(Child(clause, "expr")), Id(Child(Child(clause, "set_target"), "column_name"))});
            }
        }
        if (auto* returning = Child(node, "returning_columns_list")) {
            result.ReturningAll = Has(returning, "*");
            for (auto* column : Children(returning, "an_id")) result.Returning.push_back(Id(column));
        }
        return result;
    }

    TStatement Statement(TTree* node) {
        const auto name = Name(node);
        if (name == "into_table_stmt" || name == "update_stmt" || name == "delete_stmt") {
            TStatement result{EStatementKind::Write, Position(node)}; result.Write = std::make_shared<TWriteStatement>(Write(node)); return result;
        }
        if (name == "select_stmt") {
            TStatement result{EStatementKind::Query, Position(node)}; result.Query = Select(node); return result;
        }
        if (name == "declare_stmt") {
            if (Has(node, "=")) Unsupported(node, EFeature::Statement, "DECLARE default");
            TStatement result{EStatementKind::Declaration, Position(node)};
            result.Name = Variable(Child(node, "bind_parameter"));
            auto* type = Child(node, "type_name");
            auto* form = TypeForm(type);
            if (Name(form) == "type_name_list") {
                auto* row = TypeForm(Child(form, "type_name_or_bind"));
                if (Name(row) != "type_name_struct") {
                    result.ScalarList = true; result.Value = Type(Child(form, "type_name_or_bind")); return result;
                }
                result.RowFields.emplace();
                for (auto* field : Children(row, "struct_arg")) {
                    if (result.RowFields->size() >= Settings.MaxNodes) throw TResourceLimitError(Position(field), "parameter field limit exceeded");
                    auto* tag = Child(field, "type_name_tag");
                    if (Child(tag, "bind_parameter")) Unsupported(tag, EFeature::Type, "dynamic parameter field name");
                    result.RowFields->emplace_back(Decode(tag->getText(), tag, bool(Child(tag, "id"))), Type(Child(field, "type_name_or_bind")));
                }
            } else result.Value = Type(type);
            return result;
        }
        if (name == "pragma_stmt") {
            TStatement result{EStatementKind::Pragma, Position(node)};
            const auto pragmaName = [&](TTree* part) {
                return Settings.AnsiQuotes ? Decode(part->getText(), part, true) : Id(part);
            };
            if (auto* prefix = Child(Child(node, "opt_id_prefix_or_type"), "an_id_or_type"))
                result.Name = pragmaName(prefix) + ".";
            result.Name += pragmaName(Child(node, "an_id"));
            auto values = Children(node, "pragma_value");
            if (values.size() > 1) Unsupported(node, EFeature::Pragma, "pragma argument list");
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
    std::unordered_map<std::string, std::shared_ptr<TSyntaxWindow>> NamedWindows;
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
TParsedQuery Run(std::string_view sql, const TParseSettings& settings, bool translate, TSyntaxId* expression = nullptr) {
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
    TRule* root = expression ? static_cast<TRule*>(parser.expr()) : static_cast<TRule*>(parser.sql_query());
    if (parser.getNumberOfSyntaxErrors()) throw TFrontendError({}, "invalid SQL");
    if (expression) {
        if (tokens.LA(1) != antlr4::Token::EOF)
            throw TFrontendError(positions.Get(tokens.LT(1)), "expected end of expression");
        return TTranslate(parser.getRuleNames(), positions, settings).RunExpression(root, *expression);
    }
    return translate ? TTranslate(parser.getRuleNames(), positions, settings).Run(root) : TParsedQuery{};
}

TParsedQuery Run(std::string_view sql, const TParseSettings& settings, bool translate, TSyntaxId* expression = nullptr) {
    if (settings.AnsiQuotes) return Run<NativeRboAnsi::NativeRboAnsiLexer, NativeRboAnsi::NativeRboAnsiParser>(sql, settings, translate, expression);
    return Run<NativeRboDefault::NativeRboDefaultLexer, NativeRboDefault::NativeRboDefaultParser>(sql, settings, translate, expression);
}
} // namespace

void ValidateGrammar(std::string_view sql, const TParseSettings& settings) { Run(sql, settings, false); }
TParsedQuery ParseGrammar(std::string_view sql, const TParseSettings& settings) { return Run(sql, settings, true); }
TParsedExpression ParseGrammarExpression(std::string_view sql, const TParseSettings& settings) {
    TSyntaxId root;
    auto syntax = Run(sql, settings, true, &root);
    return {std::move(syntax.Expressions), root};
}

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
        if (node.Window) {
            const auto& w = *node.Window;
            out << "window[" << static_cast<int>(w.Frame.Type) << ',' << static_cast<int>(w.Frame.Begin) << ',' << static_cast<int>(w.Frame.End) << ':';
            expr(w.BeginOffset); expr(w.EndOffset);
            out << '['; for (auto key : w.Partition) expr(key); out << ']';
            for (const auto& key : w.Order) { out << key.Descending; expr(key.Expression); }
            out << ']';
        }
        if (node.Query) select(*node.Query);
        out << ')';
    };
    auto source = [&](const TSource& value) {
        text(value.Name); text(value.Alias); out << value.ParameterTable;
        out << "aliases["; for (const auto& name : value.ColumnAliases) text(name); out << "]";
        if (value.Query) select(*value.Query);
    };
    auto named = [&](const std::vector<TNamedExpr>& values) {
        out << '['; for (const auto& value : values) { expr(value.Expression); text(value.Alias); } out << ']';
    };
    select = [&](const TSelect& value) {
        out << '{' << value.Distinct;
        for (const auto& cte : value.Ctes) { text(cte.Name); out << "["; for (const auto& name : cte.Columns) text(name); out << "]"; select(*cte.Query); }
        if (value.Source) source(*value.Source);
        for (const auto& join : value.Joins) {
            out << static_cast<int>(join.Kind); source(join.Source); expr(join.Predicate);
            for (const auto& column : join.Using) text(column);
        }
        named(value.Projection); expr(value.Where); named(value.GroupBy); expr(value.Having);
        out << '['; for (const auto& set : value.GroupingSets) { out << '['; for (auto key : set) out << key << ','; out << ']'; } out << ']';
        for (const auto& key : value.OrderBy) { out << key.Descending; expr(key.Expression); }
        expr(value.Limit); expr(value.Offset);
        out << '['; for (auto op : value.SetOperations) out << static_cast<int>(op) << ','; out << ']';
        for (const auto& branch : value.SetInputs) select(*branch);
        out << '}';
    };
    for (const auto& statement : query.Statements) {
        out << static_cast<int>(statement.Kind) << ':' << statement.Lambda;
        text(statement.Name); text(statement.Value); out << statement.ScalarList;
        out << bool(statement.RowFields);
        if (statement.RowFields) for (const auto& [name, type] : *statement.RowFields) { text(name); text(type); }
        for (const auto& parameter : statement.Parameters) text(parameter);
        expr(statement.Expression); if (statement.Query) select(*statement.Query);
        if (statement.Write) {
            const auto& write = *statement.Write;
            out << 'W' << static_cast<int>(write.Kind) << write.Batch << write.ReturningAll;
            source(write.Target); text(write.OnConflict);
            out << '['; for (const auto& column : write.Columns) text(column); out << ']';
            if (write.Input) select(*write.Input);
            out << '['; for (const auto& row : write.Rows) { out << '['; for (auto value : row) expr(value); out << ']'; } out << ']';
            expr(write.Where); named(write.Assignments);
            out << '['; for (const auto& column : write.Returning) text(column); out << ']';
        }
    }
    return out.str();
}
} // namespace NKikimr::NKqp::NFrontend
