#include "parser.h"
#include "diagnostic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace NKikimr::NKqp::NFrontend {
namespace {

enum class EToken { End, Identifier, QuotedIdentifier, Number, String, Symbol };

enum EIdentifier { ExprIdentifier = 1, TableIdentifier = 2, AliasIdentifier = 4, GeneralIdentifier = 8 };
struct TKeyword { std::string_view Word; unsigned Identifiers; };
#include <ydb/core/kqp/opt/rbo/frontend/grammar_keywords.inc>

struct TToken {
    EToken Kind;
    std::string Text;
    TSourcePosition Position;
    std::size_t EndOffset = 0;
    std::string Word = {};
    unsigned Identifiers = 15;
};

std::string Upper(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    }
    return result;
}

bool Alpha(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool Digit(char ch) { return ch >= '0' && ch <= '9'; }

bool SupportedNumber(std::string_view text) {
    const auto upper = Upper(text);
    std::size_t end = 0;
    if (upper.starts_with("0X") || upper.starts_with("0B") || upper.starts_with("0O")) {
        const auto digits = upper[1] == 'X' ? "0123456789ABCDEF" : upper[1] == 'B' ? "01" : "012345678";
        end = 2;
        while (end < upper.size() && std::string_view(digits).find(upper[end]) != std::string_view::npos) ++end;
        if (end == 2) return false;
    } else {
        while (end < upper.size() && Digit(upper[end])) ++end;
        bool real = false;
        if (end < upper.size() && upper[end] == '.') {
            real = true; ++end;
            while (end < upper.size() && Digit(upper[end])) ++end;
        }
        if (end < upper.size() && upper[end] == 'E') {
            real = true; ++end;
            if (end < upper.size() && (upper[end] == '+' || upper[end] == '-')) ++end;
            const auto start = end;
            while (end < upper.size() && Digit(upper[end])) ++end;
            if (end == start) return false;
        }
        if (real) return end == upper.size() || upper.substr(end) == "F";
    }
    if (end < upper.size() && (upper[end] == 'U' || upper[end] == 'P')) ++end;
    if (end < upper.size() && std::string_view("LSTIBN").find(upper[end]) != std::string_view::npos) ++end;
    return end == upper.size();
}

class TLexer {
public:
    TLexer(std::string_view text, bool ansi) : Text(text), Ansi(ansi) {}

    TToken Next() {
        SkipSpace();
        const auto pos = Position;
        const auto start = Position.Offset;
        if (start == Text.size()) return {EToken::End, {}, pos};
        const char ch = Peek();
        if (Alpha(ch)) {
            do { Advance(); } while (Alpha(Peek()) || Digit(Peek()));
            TToken token{EToken::Identifier, std::string(Text.substr(start, Position.Offset - start)), pos};
            token.Word = Upper(token.Text);
            const auto it = std::lower_bound(std::begin(Keywords), std::end(Keywords), token.Word,
                [](const TKeyword& key, std::string_view word) { return key.Word < word; });
            if (it != std::end(Keywords) && it->Word == token.Word) token.Identifiers = it->Identifiers;
            return token;
        }
        if (Digit(ch)) {
            Advance();
            if (ch == '0' && (Peek() == 'x' || Peek() == 'X' || Peek() == 'o' || Peek() == 'O' || Peek() == 'b' || Peek() == 'B')) {
                Advance();
                while (Alpha(Peek()) || Digit(Peek())) Advance();
            } else {
                while (Digit(Peek())) Advance();
                if (Peek() == '.') { Advance(); while (Digit(Peek())) Advance(); }
                if (Peek() == 'e' || Peek() == 'E') {
                    Advance();
                    if (Peek() == '+' || Peek() == '-') Advance();
                    if (!Digit(Peek())) throw TFrontendError(Position, "expected exponent digits");
                    while (Digit(Peek())) Advance();
                }
                while (Alpha(Peek()) || Digit(Peek())) Advance();
            }
            const auto number = Text.substr(start, Position.Offset - start);
            if (!SupportedNumber(number)) throw TUnsupportedFeature(EFeature::Syntax, pos, "numeric token requires grammar parser");
            return {EToken::Number, std::string(number), pos};
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            const bool identifier = ch == '`';
            Advance();
            std::string value;
            bool closed = false;
            while (Position.Offset < Text.size()) {
                char current = Peek();
                Advance();
                if (current == ch) {
                    if ((Ansi || identifier) && Peek() == ch) { Advance(); value += ch; continue; }
                    closed = true;
                    break;
                }
                if (current == '\\' && (!Ansi || identifier)) {
                    if (Position.Offset == Text.size()) break;
                    current = Peek(); Advance();
                    switch (current) {
                        case 'n': current = '\n'; break;
                        case 'r': current = '\r'; break;
                        case 't': current = '\t'; break;
                        case '0': current = '\0'; break;
                        case '\\': case '\'': case '"': case '`': break;
                        default: throw TFrontendError(Position, "unsupported string escape");
                    }
                }
                value += current;
            }
            if (!closed) throw TFrontendError(pos, "unterminated quoted value");
            if (!identifier && (Alpha(Peek()) || Peek() == '@'))
                throw TUnsupportedFeature(EFeature::Syntax, pos, "typed or adjacent string literal requires grammar parser");
            return {identifier ? EToken::QuotedIdentifier : EToken::String, std::move(value), pos, Position.Offset};
        }
        if (ch == '@' && Peek(1) == '@') {
            throw TUnsupportedFeature(EFeature::Syntax, pos, "multiline literal requires grammar parser");
        }
        constexpr std::array pairs{"<=", ">=", "!=", "<>", "==", "||", "??", "::", "->", "<<", ">>"};
        for (auto pair : pairs) {
            if (Text.substr(start, 2) == pair) { Advance(); Advance(); return {EToken::Symbol, pair, pos}; }
        }
        if (std::string_view("()[]{},.;:+-*/%=<>?!$&|^~").find(ch) == std::string_view::npos) {
            throw TFrontendError(pos, "unexpected character");
        }
        Advance();
        return {EToken::Symbol, std::string(1, ch), pos};
    }

private:
    char Peek(std::size_t ahead = 0) const {
        return Position.Offset + ahead < Text.size() ? Text[Position.Offset + ahead] : '\0';
    }
    void Advance() {
        if (Position.Offset == Text.size()) return;
        if (Text[Position.Offset++] == '\n') { ++Position.Line; Position.Column = 1; }
        else ++Position.Column;
    }
    void SkipSpace() {
        while (Position.Offset < Text.size()) {
            if (std::string_view(" \r\t\f\n").find(Peek()) != std::string_view::npos) { Advance(); continue; }
            if (Peek() == '-' && Peek(1) == '-') {
                while (Position.Offset < Text.size() && Peek() != '\n' && Peek() != '\r') Advance();
                continue;
            }
            if (Peek() == '/' && Peek(1) == '*') {
                const auto pos = Position;
                Advance(); Advance();
                std::size_t depth = 1;
                while (Position.Offset < Text.size() && depth) {
                    if (Ansi && Peek() == '/' && Peek(1) == '*') { Advance(); Advance(); ++depth; }
                    else if (Peek() == '*' && Peek(1) == '/') { Advance(); Advance(); --depth; }
                    else Advance();
                }
                if (depth) throw TFrontendError(pos, "unterminated comment");
                continue;
            }
            break;
        }
    }
    std::string_view Text;
    bool Ansi;
    TSourcePosition Position;
};

class TParser {
public:
    TParser(std::string_view text, const TParseSettings& settings) : Settings(settings) {
        if (text.size() > settings.MaxQueryBytes) throw TResourceLimitError({}, "query size limit exceeded");
        // The grammar runtime validates UTF-8 and reports character positions.
        // Delegate non-ASCII input until the fast lexer has the same contract.
        for (unsigned char c : text) if (c >= 128)
            throw TUnsupportedFeature(EFeature::Syntax, {}, "non-ASCII input requires grammar parser");
        TLexer lexer(text, settings.AnsiQuotes);
        do {
            Tokens.push_back(lexer.Next());
            if ((Tokens.size() - 1) / 8 >= settings.MaxNodes) throw TResourceLimitError(Tokens.back().Position, "token limit exceeded");
        } while (Tokens.back().Kind != EToken::End);
    }

    TParsedQuery Run() {
        while (!End()) {
            if (Take(";")) continue;
            Result.Statements.push_back(Statement());
            if (!End()) Expect(";");
        }
        return std::move(Result);
    }

private:
    struct TDepth {
        TParser& Parser;
        explicit TDepth(TParser& parser) : Parser(parser) {
            if (++Parser.Depth > Parser.Settings.MaxDepth) throw TResourceLimitError(Parser.Token().Position, "nesting limit exceeded");
        }
        ~TDepth() { --Parser.Depth; }
    };

    const TToken& Token(std::size_t ahead = 0) const { return Tokens[std::min(Index + ahead, Tokens.size() - 1)]; }
    bool End() const { return Token().Kind == EToken::End; }
    bool Is(std::string_view text, std::size_t ahead = 0) const {
        const auto& token = Token(ahead);
        return (token.Kind == EToken::Identifier && token.Word == text) ||
            (token.Kind == EToken::Symbol && token.Text == text);
    }
    bool Take(std::string_view text) { if (!Is(text)) return false; ++Index; return true; }
    void Expect(std::string_view text) { if (!Take(text)) Fail("expected " + std::string(text)); }
    [[noreturn]] void Fail(std::string message) const { throw TFrontendError(Token().Position, std::move(message)); }
    std::string Identifier(unsigned category = GeneralIdentifier) {
        if (Token().Kind == EToken::String)
            throw TUnsupportedFeature(EFeature::Syntax, Token().Position, "string literals as identifiers are outside native-read-v1; use backticks");
        if (Token().Kind != EToken::Identifier && Token().Kind != EToken::QuotedIdentifier &&
            Token().Kind != EToken::String) Fail("expected identifier");
        if (Token().Kind == EToken::Identifier && !(Token().Identifiers & category)) Fail("keyword is not an identifier here");
        return Tokens[Index++].Text;
    }
    std::string Variable() { Expect("$"); return Identifier(); }
    TSyntaxId Add(ESyntaxKind kind, TSourcePosition pos, std::string text = {}, std::vector<TSyntaxId> args = {}) {
        if (Result.Expressions.size() >= Settings.MaxNodes || Result.Expressions.size() >= NoSyntax)
            throw TResourceLimitError(pos, "expression limit exceeded");
        const auto id = static_cast<TSyntaxId>(Result.Expressions.size());
        Result.Expressions.push_back({kind, pos, std::move(text), std::move(args), {}, false});
        return id;
    }
    bool CanAlias() const {
        if (Token().Kind == EToken::QuotedIdentifier || Token().Kind == EToken::String) return true;
        if (Token().Kind != EToken::Identifier) return false;
        // These words can be aliases in isolation, but the grammar's adaptive
        // prediction gives a following clause precedence. An isolated alias
        // using one of them is handled by the generated parser.
        for (auto word : {"GROUP", "ORDER", "INNER", "LEFT", "RIGHT", "CROSS", "JOIN", "ON", "USING",
                          "NATURAL", "EXCLUSION", "OUTER", "ANY", "WINDOW", "WITH", "WITHOUT", "ASSUME",
                          "INTO", "SAMPLE", "TABLESAMPLE", "FLATTEN"})
            if (Token().Word == word) return false;
        return Token().Identifiers & AliasIdentifier;
    }

    std::string TypeName() {
        TDepth depth(*this);
        std::string name = Identifier();
        if (Upper(name) == "OPTIONAL") {
            Expect("<"); name = TypeName(); Expect(">"); name += '?';
        } else if (Upper(name) == "DECIMAL") {
            Expect("(");
            if (Token().Kind != EToken::Number) Fail("expected Decimal precision");
            name += "(" + Tokens[Index++].Text;
            Expect(",");
            if (Token().Kind != EToken::Number) Fail("expected Decimal scale");
            name += "," + Tokens[Index++].Text + ")";
            Expect(")");
        }
        if (Take("?")) name += '?';
        return name;
    }

    TStatement Statement() {
        const auto pos = Token().Position;
        if (Take("DECLARE")) {
            TStatement result{EStatementKind::Declaration, pos};
            result.Name = Variable(); Expect("AS"); result.Value = TypeName(); return result;
        }
        if (Take("USE")) {
            TStatement result{EStatementKind::Use, pos}; result.Name = Identifier(); return result;
        }
        if (Take("PRAGMA")) {
            TStatement result{EStatementKind::Pragma, pos}; result.Name = Identifier();
            if (Take(".")) result.Name += "." + Identifier();
            if (Take("=")) {
                if (Token().Kind != EToken::Identifier && Token().Kind != EToken::String && Token().Kind != EToken::Number) Fail("expected pragma value");
                result.Value = Tokens[Index++].Text;
            }
            return result;
        }
        if (Is("$")) {
            TStatement result{EStatementKind::Binding, pos}; result.Name = Variable(); Expect("=");
            if (Is("SELECT") || Is("WITH") || Is("FROM")) result.Query = Select();
            else if (Is("(") && (Is("SELECT", 1) || Is("WITH", 1) || Is("FROM", 1))) {
                Expect("("); result.Query = Select(); Expect(")");
            } else if (IsLambda()) {
                result.Lambda = true; Expect("(");
                if (!Take(")")) { do { result.Parameters.push_back(Variable()); } while (Take(",")); Expect(")"); }
                Expect("->");
                if (Take("{")) { Expect("RETURN"); result.Expression = Expression(); Take(";"); Expect("}"); }
                else { Expect("("); result.Expression = Expression(); Expect(")"); }
            } else result.Expression = Expression();
            return result;
        }
        TStatement result{EStatementKind::Query, pos};
        result.Query = Select(); return result;
    }

    bool IsLambda() const {
        if (!Is("(")) return false;
        std::size_t ahead = 1;
        if (Is(")", ahead)) return Is("->", ahead + 1);
        while (Is("$", ahead)) {
            if (Token(ahead + 1).Kind != EToken::Identifier && Token(ahead + 1).Kind != EToken::QuotedIdentifier) return false;
            ahead += 2;
            if (Is(")", ahead)) return Is("->", ahead + 1);
            if (!Is(",", ahead)) return false;
            ++ahead;
        }
        return false;
    }

    TSource Source() {
        TSource result; result.Position = Token().Position;
        if (Take("(")) { result.Query = Select(); Expect(")"); }
        else {
            if (Take("$")) result.Name = "$" + Identifier();
            else {
                result.Name = Identifier(TableIdentifier);
                if (Is(".")) Fail("cluster-qualified source requires grammar parser");
            }
        }
        if (Take("AS") || CanAlias()) result.Alias = Identifier();
        return result;
    }

    void From(TSelect& select) {
        select.Source = Source();
        while (true) {
            EJoinKind kind;
            if (Take(",")) kind = EJoinKind::Cross;
            else if (Take("CROSS")) { Expect("JOIN"); kind = EJoinKind::Cross; }
            else if (Take("JOIN")) kind = EJoinKind::Inner;
            else if (Take("INNER")) { Expect("JOIN"); kind = EJoinKind::Inner; }
            else if (Take("LEFT")) {
                kind = Take("SEMI") ? EJoinKind::LeftSemi : Take("ONLY") ? EJoinKind::LeftOnly : EJoinKind::Left;
                Take("OUTER"); Expect("JOIN");
            } else if (Take("RIGHT")) {
                kind = Take("SEMI") ? EJoinKind::RightSemi : Take("ONLY") ? EJoinKind::RightOnly : EJoinKind::Right;
                Take("OUTER"); Expect("JOIN");
            } else if (Take("FULL")) { Take("OUTER"); Expect("JOIN"); kind = EJoinKind::Full; }
            else break;
            TJoin join{kind, Source()};
            if (Take("ON")) join.Predicate = Expression();
            else if (Take("USING")) {
                Expect("("); do { join.Using.push_back(Identifier()); } while (Take(",")); Expect(")");
            } else if (kind != EJoinKind::Cross) Fail("expected ON or USING for join");
            select.Joins.push_back(std::move(join));
        }
    }

    std::shared_ptr<TSelect> Select() {
        TDepth depth(*this);
        auto result = SelectCore();
        while (Take("UNION")) {
            if (!Take("ALL")) Fail("only UNION ALL is supported");
            if (Is("WITH")) Fail("UNION branch cannot start with WITH");
            result->UnionAll.push_back(SelectCore());
        }
        if (Take("ORDER")) {
            Expect("BY");
            do {
                TSortKey key{Expression()};
                if (Take("DESC")) key.Descending = true; else Take("ASC");
                result->OrderBy.push_back(key);
            } while (Take(","));
        }
        if (Take("LIMIT")) {
            result->Limit = Expression();
            if (Take(",")) { result->Offset = result->Limit; result->Limit = Expression(); }
            else if (Take("OFFSET")) result->Offset = Expression();
        }
        if (!result->UnionAll.empty() && (!result->OrderBy.empty() || result->Limit != NoSyntax))
            throw TUnsupportedFeature(EFeature::SetOperation, result->Position, "ordered or limited UNION branch");
        return result;
    }

    std::shared_ptr<TSelect> SelectCore() {
        auto result = std::make_shared<TSelect>(); result->Position = Token().Position;
        if (Take("WITH")) {
            do {
                auto name = Identifier(TableIdentifier); Expect("AS"); Expect("(");
                auto query = Select(); Expect(")"); result->Ctes.emplace_back(std::move(name), std::move(query));
            } while (Take(",") && !Is("SELECT") && !Is("FROM"));
        }
        if (Take("FROM")) From(*result);
        Expect("SELECT");
        if (Take("DISTINCT")) result->Distinct = true; else Take("ALL");
        do {
            TNamedExpr item;
            if (Take("*")) item.Expression = Add(ESyntaxKind::Star, Tokens[Index - 1].Position);
            else if (Is(".", 1) && Is("*", 2)) {
                const auto position = Token().Position;
                auto qualifier = Identifier(); Expect("."); Expect("*");
                item.Expression = Add(ESyntaxKind::Star, position, std::move(qualifier));
            } else item.Expression = Expression();
            if (Result.Expressions[item.Expression].Kind == ESyntaxKind::Star) {
                if (Is("AS") || CanAlias()) Fail("star cannot have an alias");
            } else if (Take("AS") || CanAlias()) item.Alias = Identifier();
            result->Projection.push_back(std::move(item));
        } while (Take(",") && !Is("FROM") && !Is("WHERE") && !Is("GROUP") && !Is("HAVING") &&
            !Is("ORDER") && !Is("LIMIT") && !Is("UNION") && !End() && !Is(";") && !Is(")"));
        if (Take("FROM")) {
            if (result->Source) Fail("duplicate FROM clause");
            From(*result);
        }
        if (Take("WHERE")) result->Where = Expression();
        if (Take("GROUP")) {
            Expect("BY");
            do {
                TNamedExpr key{Expression()}; if (Take("AS")) key.Alias = Identifier();
                result->GroupBy.push_back(std::move(key));
            } while (Take(","));
        }
        if (Take("HAVING")) result->Having = Expression();
        return result;
    }

    // YQL precedence, from SQLv1Antlr4.g.in: OR, AND, XOR, equality /
    // predicates, ordering comparisons, bit ops / ??, +/-, */%, ||, unary.
    int Precedence() const {
        if (Is("OR")) return 1;
        if (Is("AND")) return 2;
        if (Is("XOR")) return 3;
        if (Is("=") || Is("==") || Is("!=") || Is("<>") || Is("IS") || Is("ISNULL") || Is("NOTNULL") ||
            Is("IN") || Is("BETWEEN") || Is("LIKE") || Is("ILIKE") ||
            (Is("NOT") && (Is("IN", 1) || Is("BETWEEN", 1) || Is("LIKE", 1) || Is("ILIKE", 1)))) return 4;
        if (Is("<") || Is("<=") || Is(">") || Is(">=")) return 5;
        if (Is("??") || Is("&") || Is("|") || Is("^") || Is("<<") || Is(">>")) return 6;
        if (Is("+") || Is("-")) return 7;
        if (Is("*") || Is("/") || Is("%")) return 8;
        if (Is("||")) return 9;
        return 0;
    }

    TSyntaxId Expression(int minimum = 1) {
        TDepth depth(*this);
        auto left = Primary();
        bool predicate = false, ordering = false;
        while (Precedence() >= minimum) {
            const auto pos = Token().Position;
            const int precedence = Precedence();
            if ((precedence == 4 && std::exchange(predicate, true)) ||
                (precedence == 5 && std::exchange(ordering, true)))
                throw TUnsupportedFeature(EFeature::Expression, pos, "chained comparison requires grammar parser");
            auto op = Upper(Tokens[Index++].Text);
            bool negate = false;
            if (op == "NOT") { negate = true; op = Upper(Tokens[Index++].Text); }
            if (op == "IS" || op == "ISNULL" || op == "NOTNULL") {
                const bool notNull = op == "NOTNULL" || (op == "IS" && Take("NOT"));
                if (op == "IS") Expect("NULL");
                left = Add(ESyntaxKind::Unary, pos, notNull ? "IS NOT NULL" : "IS NULL", {left});
            } else if (op == "BETWEEN") {
                auto low = Expression(precedence + 1); Expect("AND"); auto high = Expression(precedence + 1);
                left = Add(ESyntaxKind::Between, pos, {}, {left, low, high});
            } else if (op == "IN") {
                Expect("(");
                if (Is("SELECT") || Is("WITH") || Is("FROM")) {
                    auto query = Select(); Expect(")");
                    auto id = Add(ESyntaxKind::In, pos, {}, {left}); Result.Expressions[id].Query = std::move(query); left = id;
                } else {
                    std::vector<TSyntaxId> args{left};
                    if (Is(")")) Fail("empty IN list");
                    do { args.push_back(Expression()); } while (Take(",") && !Is(")")); Expect(")");
                    left = Add(ESyntaxKind::In, pos, {}, std::move(args));
                }
            } else {
                auto right = Expression(op == "??" ? precedence : precedence + 1);
                left = Add(ESyntaxKind::Binary, pos, std::move(op), {left, right});
            }
            if (negate) left = Add(ESyntaxKind::Unary, pos, "NOT", {left});
        }
        return left;
    }

    TSyntaxId Primary() {
        const auto token = Token();
        const auto pos = token.Position;
        if (Take("+") || Take("-") || Take("NOT") || Take("~")) {
            if (Is("+") || Is("-") || Is("NOT") || Is("~")) Fail("multiple unary operators require parentheses");
            return Add(ESyntaxKind::Unary, pos, Upper(token.Text), {Expression(10)});
        }
        if (Take("NULL")) return Add(ESyntaxKind::Null, pos);
        if (Take("TRUE")) return Add(ESyntaxKind::Boolean, pos, "true");
        if (Take("FALSE")) return Add(ESyntaxKind::Boolean, pos, "false");
        if (token.Kind == EToken::Number) {
            ++Index;
            const bool radix = token.Text.size() > 1 && token.Text[0] == '0' &&
                (token.Text[1] == 'x' || token.Text[1] == 'X' || token.Text[1] == 'b' || token.Text[1] == 'B' || token.Text[1] == 'o' || token.Text[1] == 'O');
            return Add(!radix && token.Text.find_first_of(".eE") != std::string::npos ? ESyntaxKind::Real : ESyntaxKind::Integer, pos, token.Text);
        }
        if (token.Kind == EToken::String) {
            ++Index;
            return Add(ESyntaxKind::String, pos, token.Text);
        }
        if (Take("CAST")) {
            Expect("("); auto arg = Expression(); Expect("AS"); auto type = TypeName(); Expect(")");
            return Add(ESyntaxKind::Cast, pos, std::move(type), {arg});
        }
        if (Take("CASE")) {
            TSyntaxId value = NoSyntax;
            if (!Is("WHEN")) value = Expression();
            std::vector<TSyntaxId> args;
            if (!Is("WHEN")) Fail("expected WHEN");
            while (Take("WHEN")) {
                auto condition = Expression();
                if (value != NoSyntax) condition = Add(ESyntaxKind::Binary, pos, "==", {value, condition});
                Expect("THEN"); args.push_back(condition); args.push_back(Expression());
            }
            const bool hasElse = Take("ELSE");
            args.push_back(hasElse ? Expression() : Add(ESyntaxKind::Null, pos)); Expect("END");
            return Add(ESyntaxKind::Case, pos, hasElse ? "" : "implicit_else", std::move(args));
        }
        if (Take("EXISTS")) {
            Expect("("); auto query = Select(); Expect(")"); auto id = Add(ESyntaxKind::Exists, pos);
            Result.Expressions[id].Query = std::move(query); return id;
        }
        if (Take("(")) {
            if (Is("SELECT") || Is("WITH") || Is("FROM")) {
                auto query = Select(); Expect(")"); auto id = Add(ESyntaxKind::ScalarSubquery, pos);
                Result.Expressions[id].Query = std::move(query); return id;
            }
            auto result = Expression(); Expect(")"); return result;
        }
        const bool variable = Take("$");
        std::string name = Identifier(variable || Is("::", 1) ? GeneralIdentifier : ExprIdentifier);
        if (!variable) {
            if (Take("::")) name += "::" + Identifier();
            while (Take(".")) {
                name += "." + Identifier();
            }
        }
        if (!Take("(")) return Add(variable ? ESyntaxKind::Variable : ESyntaxKind::Column, pos, std::move(name));
        const bool distinct = Take("DISTINCT");
        const bool quantified = distinct || Take("ALL");
        std::vector<TSyntaxId> args;
        if (Take("*")) {
            if (quantified) Fail("invalid star argument");
            args.push_back(Add(ESyntaxKind::Star, Tokens[Index - 1].Position)); Expect(")");
        } else if (!Take(")")) { do { args.push_back(Expression()); } while (Take(",") && !Is(")")); Expect(")"); }
        if (quantified && args.empty()) Fail("set quantifier requires an argument");
        for (auto arg : args) if (Result.Expressions[arg].Kind == ESyntaxKind::Star &&
            (quantified || args.size() != 1 || !Result.Expressions[arg].Text.empty())) Fail("invalid star argument");
        auto id = Add(ESyntaxKind::Call, pos, variable ? "$" + name : name, std::move(args));
        Result.Expressions[id].Distinct = distinct;
        return id;
    }

    const TParseSettings& Settings;
    std::vector<TToken> Tokens;
    std::size_t Index = 0;
    std::size_t Depth = 0;
    TParsedQuery Result;
};

} // namespace

TParsedQuery Parse(std::string_view sql, const TParseSettings& settings) {
    return TParser(sql, settings).Run();
}

} // namespace NKikimr::NKqp::NFrontend
