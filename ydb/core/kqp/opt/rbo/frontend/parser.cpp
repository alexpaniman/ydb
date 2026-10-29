#include "parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace NKikimr::NKqp::NFrontend {
namespace {

enum class EToken { End, Identifier, QuotedIdentifier, Number, String, Symbol };

struct TToken {
    EToken Kind;
    std::string Text;
    TSourcePosition Position;
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
            return {EToken::Identifier, std::string(Text.substr(start, Position.Offset - start)), pos};
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
            return {EToken::Number, std::string(Text.substr(start, Position.Offset - start)), pos};
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            const bool identifier = ch == '`' || (ch == '"' && Ansi);
            Advance();
            std::string value;
            bool closed = false;
            while (Position.Offset < Text.size()) {
                char current = Peek();
                Advance();
                if (current == ch) {
                    if (Peek() == ch) { Advance(); value += ch; continue; }
                    closed = true;
                    break;
                }
                if (current == '\\' && !(Ansi && ch == '"')) {
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
            // The suffix is emitted as a separate token, allowing the parser to
            // distinguish Utf8 from String without treating quoted names as keywords.
            return {identifier ? EToken::QuotedIdentifier : EToken::String, std::move(value), pos};
        }
        if (ch == '@' && Peek(1) == '@') {
            Advance(); Advance();
            std::string value;
            while (Position.Offset < Text.size() && !(Peek() == '@' && Peek(1) == '@')) {
                value += Peek(); Advance();
            }
            if (Position.Offset == Text.size()) throw TFrontendError(pos, "unterminated multiline string");
            Advance(); Advance();
            return {EToken::String, std::move(value), pos};
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
            if (std::isspace(static_cast<unsigned char>(Peek()))) { Advance(); continue; }
            if (Peek() == '-' && Peek(1) == '-') {
                while (Position.Offset < Text.size() && Peek() != '\n') Advance();
                continue;
            }
            if (Peek() == '/' && Peek(1) == '*') {
                const auto pos = Position;
                Advance(); Advance();
                std::size_t depth = 1;
                while (Position.Offset < Text.size() && depth) {
                    if (Peek() == '/' && Peek(1) == '*') { Advance(); Advance(); ++depth; }
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
        if (text.size() > settings.MaxQueryBytes) throw TFrontendError({}, "query size limit exceeded");
        TLexer lexer(text, settings.AnsiQuotes);
        do {
            Tokens.push_back(lexer.Next());
            if (Tokens.size() > settings.MaxNodes * 8) throw TFrontendError(Tokens.back().Position, "token limit exceeded");
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
            if (++Parser.Depth > Parser.Settings.MaxDepth) Parser.Fail("nesting limit exceeded");
        }
        ~TDepth() { --Parser.Depth; }
    };

    const TToken& Token(std::size_t ahead = 0) const { return Tokens[std::min(Index + ahead, Tokens.size() - 1)]; }
    bool End() const { return Token().Kind == EToken::End; }
    bool Is(std::string_view text, std::size_t ahead = 0) const {
        const auto& token = Token(ahead);
        return (token.Kind == EToken::Identifier && Upper(token.Text) == text) ||
            (token.Kind == EToken::Symbol && token.Text == text);
    }
    bool Take(std::string_view text) { if (!Is(text)) return false; ++Index; return true; }
    void Expect(std::string_view text) { if (!Take(text)) Fail("expected " + std::string(text)); }
    [[noreturn]] void Fail(std::string message) const { throw TFrontendError(Token().Position, std::move(message)); }
    std::string Identifier() {
        if (Token().Kind != EToken::Identifier && Token().Kind != EToken::QuotedIdentifier) Fail("expected identifier");
        return Tokens[Index++].Text;
    }
    std::string Variable() { Expect("$"); return Identifier(); }
    TSyntaxId Add(ESyntaxKind kind, TSourcePosition pos, std::string text = {}, std::vector<TSyntaxId> args = {}) {
        if (Result.Expressions.size() >= Settings.MaxNodes || Result.Expressions.size() >= NoSyntax) Fail("expression limit exceeded");
        const auto id = static_cast<TSyntaxId>(Result.Expressions.size());
        Result.Expressions.push_back({kind, pos, std::move(text), std::move(args), {}, false});
        return id;
    }
    bool CanAlias() const {
        if (Token().Kind == EToken::QuotedIdentifier) return true;
        if (Token().Kind != EToken::Identifier) return false;
        static constexpr std::array stops{"FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "OFFSET", "UNION", "INTERSECT", "EXCEPT",
            "INNER", "LEFT", "RIGHT", "FULL", "CROSS", "JOIN", "ON", "USING", "WINDOW", "WITH", "WITHOUT", "ASSUME", "INTO", "SAMPLE", "TABLESAMPLE", "FLATTEN", "ASC", "DESC", "NULLS"};
        const auto word = Upper(Token().Text);
        return std::find(stops.begin(), stops.end(), word) == stops.end();
    }

    std::string TypeName() {
        std::string name = Identifier();
        if (Upper(name) == "OPTIONAL") {
            Expect("<"); name = TypeName(); Expect(">"); name += '?';
        } else if (Upper(name) == "DECIMAL") {
            Expect("(");
            if (Token().Kind != EToken::Number) Fail("expected Decimal precision");
            name = "Decimal(" + Tokens[Index++].Text;
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
                result.Name = Identifier();
                while (Take(".")) result.Name += "." + Identifier();
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
        return result;
    }

    std::shared_ptr<TSelect> SelectCore() {
        auto result = std::make_shared<TSelect>(); result->Position = Token().Position;
        if (Take("WITH")) {
            do {
                auto name = Identifier(); Expect("AS"); Expect("(");
                auto query = Select(); Expect(")"); result->Ctes.emplace_back(std::move(name), std::move(query));
            } while (Take(","));
        }
        if (Take("FROM")) From(*result);
        Expect("SELECT");
        if (Take("DISTINCT")) result->Distinct = true; else Take("ALL");
        do {
            TNamedExpr item{Expression()};
            if (Take("AS") || CanAlias()) item.Alias = Identifier();
            result->Projection.push_back(std::move(item));
        } while (Take(",") && !Is("FROM") && !End() && !Is(";") && !Is(")"));
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
        while (Precedence() >= minimum) {
            const auto pos = Token().Position;
            const int precedence = Precedence();
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
                if (Is("SELECT") || Is("WITH")) {
                    auto query = Select(); Expect(")");
                    auto id = Add(ESyntaxKind::In, pos, {}, {left}); Result.Expressions[id].Query = std::move(query); left = id;
                } else {
                    std::vector<TSyntaxId> args{left};
                    if (!Take(")")) { do { args.push_back(Expression()); } while (Take(",")); Expect(")"); }
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
            return Add(ESyntaxKind::Unary, pos, Upper(token.Text), {Expression(10)});
        }
        if (Take("NULL")) return Add(ESyntaxKind::Null, pos);
        if (Take("TRUE")) return Add(ESyntaxKind::Boolean, pos, "true");
        if (Take("FALSE")) return Add(ESyntaxKind::Boolean, pos, "false");
        if (Take("*")) return Add(ESyntaxKind::Star, pos);
        if (token.Kind == EToken::Number) {
            ++Index;
            const bool radix = token.Text.size() > 1 && token.Text[0] == '0' &&
                (token.Text[1] == 'x' || token.Text[1] == 'X' || token.Text[1] == 'b' || token.Text[1] == 'B' || token.Text[1] == 'o' || token.Text[1] == 'O');
            return Add(!radix && token.Text.find_first_of(".eE") != std::string::npos ? ESyntaxKind::Real : ESyntaxKind::Integer, pos, token.Text);
        }
        if (token.Kind == EToken::String) {
            ++Index;
            auto id = Add(ESyntaxKind::String, pos, token.Text);
            // S/U are accepted as explicit suffixes. Other typed strings need
            // their own execution representation and are rejected by the caller.
            if (Is("U")) { ++Index; id = Add(ESyntaxKind::Cast, pos, "Utf8", {id}); }
            else Take("S");
            return id;
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
            args.push_back(Take("ELSE") ? Expression() : Add(ESyntaxKind::Null, pos)); Expect("END");
            return Add(ESyntaxKind::Case, pos, {}, std::move(args));
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
        std::string name = Identifier();
        if (!variable) {
            while (Take("::")) name += "::" + Identifier();
            while (Take(".")) {
                if (Take("*")) return Add(ESyntaxKind::Star, pos, std::move(name));
                name += "." + Identifier();
            }
        }
        if (!Take("(")) return Add(variable ? ESyntaxKind::Variable : ESyntaxKind::Column, pos, std::move(name));
        const bool distinct = Take("DISTINCT");
        std::vector<TSyntaxId> args;
        if (!Take(")")) { do { args.push_back(Expression()); } while (Take(",")); Expect(")"); }
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
