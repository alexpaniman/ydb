// Parser contract tests ported from PR #58587 at fbeb81657ef.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboDefaultLexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboDefaultParser.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboAnsiLexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboAnsiParser.h>
#include <library/cpp/testing/unittest/registar.h>
#include <array>
#include <future>
using namespace NKikimr::NKqp::NFrontend;

namespace {

// Expose parser/lexer DFA and prediction-context cache addresses from real
// parses. Comparing addresses across threads detects accidental thread-local
// cache generation even when both parsers produce identical syntax results.
template <typename TLexer, typename TParser>
std::array<const void*, 4> ParseAndGetCaches() {
    antlr4::ANTLRInputStream input("SELECT 1 + 2;");
    TLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    TParser parser(&tokens);
    UNIT_ASSERT(parser.sql_query());
    UNIT_ASSERT_VALUES_EQUAL(parser.getNumberOfSyntaxErrors(), 0);
    const auto* lexerSimulator = lexer.template getInterpreter<antlr4::atn::LexerATNSimulator>();
    const auto* parserSimulator = parser.template getInterpreter<antlr4::atn::ParserATNSimulator>();
    return {&lexerSimulator->_decisionToDFA, &lexerSimulator->getSharedContextCache(),
        &parserSimulator->decisionToDFA, &parserSimulator->getSharedContextCache()};
}

// Run parsing on separate asynchronous threads and compare against the main
// thread. Cache sharing is a generated-parser property, so test both dialects.
template <typename TLexer, typename TParser>
void CheckProcessWideCaches() {
    const auto first = ParseAndGetCaches<TLexer, TParser>();
    const auto second = ParseAndGetCaches<TLexer, TParser>();
    auto otherThread = std::async(std::launch::async, [] {
        return ParseAndGetCaches<TLexer, TParser>();
    });
    const auto other = otherThread.get();
    for (size_t i = 0; i < first.size(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(first[i], second[i]);
        UNIT_ASSERT_VALUES_EQUAL(first[i], other[i]);
    }
}

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeReferenceParser) {
    Y_UNIT_TEST(ProcessWideDefaultDfaCache) {
        CheckProcessWideCaches<NativeRboDefault::NativeRboDefaultLexer, NativeRboDefault::NativeRboDefaultParser>();
    }
    Y_UNIT_TEST(ProcessWideAnsiDfaCache) {
        CheckProcessWideCaches<NativeRboAnsi::NativeRboAnsiLexer, NativeRboAnsi::NativeRboAnsiParser>();
    }
    Y_UNIT_TEST(AstWithoutSemanticTranslation) {
        auto parsed = ParseNative("SELECT NotAnExistingFunction(1);");
        const auto id = parsed.Statements[0].Query->Projection[0].Expression;
        UNIT_ASSERT(parsed.Expressions[id].Kind == ESyntaxKind::Call);
        UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[id].Text, "NotAnExistingFunction");
        UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[id].Arguments.size(), 1);
    }
    Y_UNIT_TEST(AnsiLexerDirective) {
        const auto parsed = ParseNative("--!ansi_lexer\nSELECT 1 AS \"value\";");
        // Names are decoded once by our parser, before any binding/caller work.
        UNIT_ASSERT_VALUES_EQUAL(parsed.Statements[0].Query->Projection[0].Alias, "value");
    }
    Y_UNIT_TEST(SyntaxErrorDoesNotRequestFallback) {
        try { ParseNative("SELECT ("); UNIT_FAIL("invalid syntax accepted"); }
        catch (const TUnsupportedFeature&) { UNIT_FAIL("syntax error requested fallback"); }
        catch (const TFrontendError& error) { UNIT_ASSERT(error.Position.Column > 1); }
    }
    Y_UNIT_TEST(LexerErrorDoesNotRequestFallback) {
        try { ParseNative("SELECT 1; \x01"); UNIT_FAIL("invalid token accepted"); }
        catch (const TUnsupportedFeature&) { UNIT_FAIL("lexer error requested fallback"); }
        catch (const TFrontendError& error) { UNIT_ASSERT(error.Position.Column > 1); }
    }
    Y_UNIT_TEST(UnsupportedDialectRequestsFallback) {
        UNIT_ASSERT_EXCEPTION(ParseNative("--!syntax_pg\nSELECT 1;"), TUnsupportedFeature);
    }

    Y_UNIT_TEST(QuotedNamesCompileWithOwnedDecodedBindings) {
        const auto compiled = CompileNative("$'x' = 1; SELECT $'x' AS 'answer'", {});
        const auto& program = compiled.Program->Get();
        UNIT_ASSERT_VALUES_EQUAL(program.Results.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns[0].Name, "answer");
        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int32}));
    }

    Y_UNIT_TEST(AdjacentQuotesKeepDialectTokenization) {
        const auto plain = ParseNative("SELECT 'a''b'");
        const auto& projection = plain.Statements[0].Query->Projection[0];
        UNIT_ASSERT_VALUES_EQUAL(projection.Alias, "b");
        UNIT_ASSERT_VALUES_EQUAL(plain.Expressions[projection.Expression].Text, "a");
        const auto ansi = ParseNative("--!ansi_lexer\nSELECT 'a''b'");
        const auto& value = ansi.Statements[0].Query->Projection[0];
        UNIT_ASSERT(value.Alias.empty());
        UNIT_ASSERT_VALUES_EQUAL(ansi.Expressions[value.Expression].Text, "a'b");
    }

    Y_UNIT_TEST(QuotedExpressionKindFollowsLexerMode) {
        for (const std::string_view prefix : {"", "--!ansi_lexer\n"}) {
            const auto parsed = ParseNative(std::string(prefix) + "SELECT \"text\" AS \"label\" FROM \"table\";");
            const auto& query = *parsed.Statements[0].Query;
            UNIT_ASSERT_VALUES_EQUAL(query.Projection[0].Alias, "label");
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "table");
            const auto& value = parsed.Expressions[query.Projection[0].Expression];
            UNIT_ASSERT(value.Kind == (prefix.empty() ? ESyntaxKind::String : ESyntaxKind::Column));
            UNIT_ASSERT_VALUES_EQUAL(value.Text, "text");
        }
    }
}
