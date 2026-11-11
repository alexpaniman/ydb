// Parser contract tests ported from PR #58587 at fbeb81657ef.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboDefaultLexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboDefaultParser.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboAnsiLexer.h>
#include <ydb/core/kqp/opt/rbo/frontend/grammar/NativeRboAnsiParser.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/common/env.h>
#include <library/cpp/resource/resource.h>
#include <array>
#include <chrono>
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
    // Port of the reference's isolated cold/warm DFA experiment. The native
    // parser owns CST construction, translation and destruction behind one
    // boundary, so measure that complete boundary rather than exposing CSTs.
    // Run alone: resetting the shared DFA is unsafe during another parse.
    Y_UNIT_TEST(ParserDfaWarmup_TPCDS66) {
        if (GetTestParam("ENABLE_PERF_PARSER").empty()) return;
        const std::string sql = "PRAGMA ydb.CostBasedOptimizationLevel = '0';\n"
            "$round = ($x,$y) -> {return $x;};\n"
            "$to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };\n"
            "$to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };\n"
            + std::string(NResource::Find("tpcds-q66"));
        const auto expected = SyntaxFingerprint(Parse(sql));
        using TClock = std::chrono::steady_clock;
        std::size_t previousStates = 0;
        Cout << "PERF_PARSER_HEADER,iteration,reset_dfa,dfa_before,dfa_after,native_parse_us" << Endl;
        for (unsigned iteration = 1; iteration <= 12; ++iteration) {
            antlr4::ANTLRInputStream input(sql);
            NativeRboDefault::NativeRboDefaultLexer lexer(&input);
            antlr4::CommonTokenStream tokens(&lexer);
            NativeRboDefault::NativeRboDefaultParser parser(&tokens);
            auto* simulator = parser.getInterpreter<antlr4::atn::ParserATNSimulator>();
            const auto countStates = [&] {
                std::size_t count = 0;
                for (const auto& dfa : simulator->decisionToDFA) count += dfa.states.size();
                return count;
            };
            const bool reset = iteration == 1 || iteration == 7;
            if (reset) {
                simulator->clearDFA();
                lexer.getInterpreter<antlr4::atn::LexerATNSimulator>()->clearDFA();
            }
            const auto before = countStates();
            UNIT_ASSERT_VALUES_EQUAL(before, reset ? 0 : previousStates);
            const auto start = TClock::now();
            const auto syntax = ParseGrammar(sql);
            const auto elapsed = std::chrono::duration<double, std::micro>(TClock::now() - start).count();
            previousStates = countStates();
            UNIT_ASSERT(previousStates > 0);
            UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(syntax), expected);
            Cout << "PERF_PARSER," << iteration << ',' << reset << ',' << before << ',' << previousStates
                << ',' << elapsed << Endl;
        }
    }

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
