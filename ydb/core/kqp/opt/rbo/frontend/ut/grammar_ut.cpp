#include <ydb/core/kqp/opt/rbo/frontend/parser.h>
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/expression.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/resource/resource.h>

using namespace NKikimr::NKqp::NFrontend;

Y_UNIT_TEST_SUITE(KqpNativeGrammar) {
    Y_UNIT_TEST(NullSafeEqualityAgreesAcrossParsers) {
        for (const auto* sql : {"SELECT x IS DISTINCT FROM y", "SELECT x IS NOT DISTINCT FROM y + 1",
                "SELECT x IS NOT DISTINCT FROM y AND a IS DISTINCT FROM b", "SELECT NOT x IS DISTINCT FROM y"}) {
            UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)), sql);
        }
        for (const auto* sql : {"SELECT 1 IS DISTINCT 2", "SELECT 1 IS NOT DISTINCT FROM", "SELECT 1 IS FROM 2"}) {
            UNIT_ASSERT_EXCEPTION_C(Parse(sql), TFrontendError, sql);
            UNIT_ASSERT_EXCEPTION_C(ParseGrammar(sql), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(BetweenModesAndCompactMembershipAgreeAcrossParsers) {
        for (const auto* sql : {"SELECT 2 BETWEEN 1 AND 3", "SELECT 2 NOT BETWEEN 1 AND 3",
                "SELECT 2 BETWEEN SYMMETRIC 3 AND 1", "SELECT 2 NOT BETWEEN SYMMETRIC 3 AND 1",
                "SELECT 2 BETWEEN ASYMMETRIC 1 AND 3", "SELECT 2 NOT BETWEEN ASYMMETRIC 1 AND 3",
                "SELECT 2 IN COMPACT (1, NULL)", "SELECT 2 NOT IN COMPACT [1, NULL]"}) {
            UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)), sql);
        }
        for (const auto* sql : {"SELECT 2 BETWEEN SYMMETRIC ASYMMETRIC 1 AND 3",
                "SELECT 2 BETWEEN SYMMETRIC 1", "SELECT 2 NOT BETWEEN ASYMMETRIC AND 3"}) {
            UNIT_ASSERT_EXCEPTION_C(Parse(sql), TFrontendError, sql);
            UNIT_ASSERT_EXCEPTION_C(ParseGrammar(sql), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(PragmaArgumentsRetainArityAndLiteralKinds) {
        for (const bool ansi : {false, true}) {
            TParseSettings settings; settings.AnsiQuotes = ansi;
            const auto* sql = "PRAGMA custom; PRAGMA custom(''); PRAGMA custom('a', 'b');";
            const auto fast = Parse(sql, settings), grammar = ParseGrammar(sql, settings);
            UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
            for (const auto* parsed : {&fast, &grammar}) {
                UNIT_ASSERT(parsed->Statements[0].PragmaValues.empty());
                UNIT_ASSERT(parsed->Statements[1].PragmaValues == std::vector<std::string>{""});
                UNIT_ASSERT(parsed->Statements[2].PragmaValues == (std::vector<std::string>{"a", "b"}));
            }
            for (const auto* value : {"0", "+0", "false", "`text`"}) {
                const auto invalid = std::string("PRAGMA custom = ") + value;
                UNIT_ASSERT_EXCEPTION(Parse(invalid, settings), TFrontendError);
                UNIT_ASSERT_EXCEPTION(ParseGrammar(invalid, settings), TFrontendError);
            }
        }
    }

    Y_UNIT_TEST(AnsiIdentifiersPreserveEscapingCaseAndSourcePositions) {
        TParseSettings settings; settings.AnsiQuotes = true;
        const auto* sql = R"(SELECT "a""b"."Id", "literal\n", 'string\n' FROM "Mixed" AS "a""b")";
        const auto fast = Parse(sql, settings), grammar = ParseGrammar(sql, settings);
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
        for (const auto* parsed : {&fast, &grammar}) {
            const auto& query = *parsed->Statements[0].Query;
            const auto& id = parsed->Expressions[query.Projection[0].Expression];
            UNIT_ASSERT(id.Kind == ESyntaxKind::Column);
            UNIT_ASSERT_VALUES_EQUAL(id.Text, "a\"b.Id");
            UNIT_ASSERT_VALUES_EQUAL(id.Position.Column, 8);
            const auto& quoted = parsed->Expressions[query.Projection[1].Expression];
            UNIT_ASSERT(quoted.Kind == ESyntaxKind::Column);
            UNIT_ASSERT_VALUES_EQUAL(quoted.Text, R"(literal\n)");
            const auto& string = parsed->Expressions[query.Projection[2].Expression];
            UNIT_ASSERT(string.Kind == ESyntaxKind::String);
            UNIT_ASSERT_VALUES_EQUAL(string.Text, R"(string\n)");
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "Mixed");
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Alias, "a\"b");
        }
    }

    Y_UNIT_TEST(BinaryOperatorPositionsAgreeWithHandwrittenParser) {
        const auto sql = "SELECT\n a + b * c - d / e % f, a || b, a < b, a = b, a != b, a ?? b,\n"
            " a AND b OR c XOR d, a & b | c ^ d, a << b, a >> b;";
        const auto fast = Parse(sql), grammar = ParseGrammar(sql);
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
        const auto positions = [](const TParsedQuery& parsed) {
            std::vector<std::tuple<std::string, std::size_t, std::uint32_t, std::uint32_t>> result;
            for (const auto& expression : parsed.Expressions) if (expression.Kind == ESyntaxKind::Binary)
                result.emplace_back(expression.Text, expression.Position.Offset, expression.Position.Line, expression.Position.Column);
            std::sort(result.begin(), result.end());
            return result;
        };
        UNIT_ASSERT(positions(fast) == positions(grammar));
    }

    Y_UNIT_TEST(IndexViewsHaveOneParsedSourceIdentity) {
        for (const auto* sql : {"SELECT id FROM t VIEW ByValue",
                "SELECT a.id FROM `/Root/t` VIEW `By Value` AS a",
                "SELECT a.id FROM t VIEW `By\\x56alue` a JOIN t VIEW ById b ON a.id = b.id",
                "SELECT id FROM t VIEW ByValue UNION ALL SELECT id FROM t VIEW ById",
                "$q = SELECT id FROM t VIEW ByValue; SELECT id FROM $q"}) {
            UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)), sql);
            TCompilerSettings settings; settings.Parser = EParser::Verify;
            const auto compiled = CompileNative(sql, [](std::string_view name, TSourcePosition) {
                return TCatalogTable{std::string(name), {{"id", {ETypeKind::Uint64}}}};
            }, settings);
            UNIT_ASSERT(compiled.Program);
            bool found = false;
            for (const auto& relation : compiled.Program->Get().Relations) if (relation.Kind == ERelation::Read) {
                UNIT_ASSERT(relation.Index);
                found = true;
            }
            UNIT_ASSERT(found);
        }
        UNIT_ASSERT_VALUES_EQUAL(*Parse(R"(SELECT * FROM t VIEW `By\x56alue`)").Statements[0].Query->Source->Index, "ByValue");
        for (const auto* sql : {"$q = SELECT 1 AS id; SELECT id FROM $q VIEW ix",
                "WITH q AS (SELECT 1 AS id) SELECT id FROM q VIEW ix"}) {
            UNIT_ASSERT_EXCEPTION(CompileNative(sql, {}), TFrontendError);
        }
    }

    Y_UNIT_TEST(QuotedEscapesAgreeAcrossBothParsersAndPragmas) {
        const auto check = [](const std::string& text, const std::string& expected, bool ansi = false) {
            TParseSettings settings; settings.AnsiQuotes = ansi;
            const auto sql = "SELECT '" + text + "' AS value";
            const auto fast = Parse(sql, settings), grammar = ParseGrammar(sql, settings);
            UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
            const auto id = fast.Statements[0].Query->Projection[0].Expression;
            UNIT_ASSERT_VALUES_EQUAL(fast.Expressions[id].Text, expected);
            const auto pragma = "PRAGMA custom = '" + text + "'; SELECT 1";
            UNIT_ASSERT_VALUES_EQUAL(Parse(pragma, settings).Statements[0].PragmaValues.at(0), expected);
            UNIT_ASSERT_VALUES_EQUAL(ParseGrammar(pragma, settings).Statements[0].PragmaValues.at(0), expected);
        };
        constexpr std::string_view hex = "0123456789abcdef";
        for (unsigned value = 0; value < 256; ++value) {
            const std::string expected(1, static_cast<char>(value));
            check(std::string("\\x") + hex[value >> 4] + hex[value & 15], expected);
            check(std::string("\\") + static_cast<char>('0' + (value >> 6)) +
                static_cast<char>('0' + ((value >> 3) & 7)) + static_cast<char>('0' + (value & 7)), expected);
        }
        for (const auto& [text, expected] : std::vector<std::pair<std::string, std::string>>{
                {R"(\a\b\f\n\r\t\v)", "\a\b\f\n\r\t\v"},
                {R"(\q\4\?\'\"\\)", "q4?'\"\\"},
                {R"(\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff)", "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf"},
                {R"(\U00010000\U0010ffff)", "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"},
                {R"(\u0000\U00000000)", std::string(2, '\0')}}) check(text, expected);
        check(R"(\x30\n)", R"(\x30\n)", true);
        check("it''s", "it's", true);
        for (const bool ansi : {false, true}) {
            TParseSettings settings; settings.AnsiQuotes = ansi;
            const auto sql = R"(SELECT `a\x20\u0062``c` FROM `t\x31`)";
            const auto fast = Parse(sql, settings), grammar = ParseGrammar(sql, settings);
            UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
            const auto& query = *fast.Statements[0].Query;
            UNIT_ASSERT_VALUES_EQUAL(fast.Expressions[query.Projection[0].Expression].Text, "a b`c");
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "t1");
        }
    }

    Y_UNIT_TEST(MalformedEscapesAreErrorsInBothParsers) {
        for (const auto* text : {R"(\x)", R"(\x0)", R"(\x0g)", R"(\0)", R"(\00)", R"(\378)",
                R"(\u000)", R"(\uD800)", R"(\uDFFF)", R"(\U00110000)", R"(\Uffffffff)", R"(\U0000000x)"}) {
            for (const auto& sql : {std::string("SELECT '") + text + "'", std::string("PRAGMA custom='") + text + "'; SELECT 1"}) {
                UNIT_ASSERT_EXCEPTION_C(Parse(sql), TFrontendError, sql);
                UNIT_ASSERT_EXCEPTION_C(ParseGrammar(sql), TFrontendError, sql);
            }
        }
    }

    Y_UNIT_TEST(MultilineDelimiterEscapesDecodeOnce) {
        TCompilerSettings settings;
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            settings.Parser = parser;
            const auto parsed = ParseNative("SELECT @@one@@@@two@@, @@\\x30@@", settings);
            const auto& projection = parsed.Statements[0].Query->Projection;
            UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[projection[0].Expression].Text, "one@@two");
            UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[projection[1].Expression].Text, R"(\x30)");
        }
    }

    Y_UNIT_TEST(ParenthesizedSetOperandsPreserveLocalClauses) {
        for (const auto* sql : {
            "SELECT * FROM ((SELECT 1 AS x) UNION ALL (SELECT 2 AS x))",
            "SELECT * FROM ((SELECT 1 AS x ORDER BY x LIMIT 1) UNION ALL SELECT 3 AS x)",
            "SELECT * FROM (SELECT 1 AS x UNION ALL (SELECT 2 AS x ORDER BY x DESC LIMIT 1))",
            "SELECT * FROM ((SELECT 1 AS x) EXCEPT (SELECT 2 AS x) INTERSECT (SELECT 3 AS x))",
            "WITH q AS (SELECT 1 AS x) SELECT * FROM ((SELECT x FROM q LIMIT 1) UNION SELECT 3 AS x)",
        }) {
            const auto fast = Parse(sql);
            const auto grammar = ParseGrammar(sql);
            UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(fast), SyntaxFingerprint(grammar), sql);
            TCompilerSettings settings; settings.Mode = ECompilerMode::NativeRequired; settings.Parser = EParser::Verify;
            const auto result = CompileNative(sql, {}, settings);
            UNIT_ASSERT_C(result.Program, sql);
        }
        const auto parsed = Parse("SELECT * FROM ((SELECT 1 AS x LIMIT 1) UNION SELECT 3 AS x)");
        const auto& set = *parsed.Statements[0].Query->Source->Query;
        UNIT_ASSERT_VALUES_EQUAL(set.SetInputs.size(), 2);
        UNIT_ASSERT(set.SetInputs[0]->Limit != NoSyntax);
        UNIT_ASSERT(set.SetInputs[0]->SetInputs.empty());
        for (const auto* sql : {
            "SELECT * FROM ((SELECT 1 AS x UNION SELECT 2 AS x) UNION SELECT 3 AS x)",
            "SELECT * FROM ((WITH q AS (SELECT 1 AS x) SELECT x FROM q) UNION SELECT 3 AS x)",
            "SELECT * FROM (((SELECT 1 AS x)))",
        }) {
            UNIT_ASSERT_EXCEPTION_C(Parse(sql), TFrontendError, sql);
            UNIT_ASSERT_EXCEPTION_C(ParseGrammar(sql), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(GroupingSetsHaveSharedBoundedExpansion) {
        const std::pair<const char*, TGroupingSets> cases[]{
            {"id, value", {}},
            {"()", {{}}},
            {"(id, value)", {{0, 1}}},
            {"ROLLUP(id, value)", {{0, 1}, {0}, {}}},
            {"CUBE(id, value)", {{0, 1}, {1}, {0}, {}}},
            {"GROUPING SETS ((id, value), (id), (),)", {{0, 1}, {2}, {}}},
            {"id, ROLLUP(value)", {{0, 1}, {0}}},
            {"ROLLUP(id), CUBE(value)", {{0, 1}, {0}, {1}, {}}},
            {"ROLLUP((id, value), other)", {{0, 1, 2}, {0, 1}, {}}},
            {"GROUPING SETS (ROLLUP(id, value), CUBE(other), GROUPING SETS (id, ()))", {{0, 1}, {0}, {}, {2}, {}, {3}, {}}},
            {"GROUPING SETS ((id, id), (id), (id))", {{0, 1}, {2}, {3}}},
            {"GROUPING SETS ((id + 1ul AS k, value), ())", {{0, 1}, {}}},
        };
        for (const auto& [clause, sets] : cases) {
            const std::string sql = std::string("SELECT COUNT(*) FROM t GROUP BY ") + clause;
            try {
                const auto fast = Parse(sql);
                const auto grammar = ParseGrammar(sql);
                UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(fast), SyntaxFingerprint(grammar), sql);
                UNIT_ASSERT_C(fast.Statements[0].Query->GroupingSets == sets, sql);
            } catch (const TFrontendError& error) {
                UNIT_FAIL(TStringBuilder() << sql << ": " << error.what());
            }
        }
        const auto sql = "SELECT COUNT(*) FROM t GROUP BY CUBE(a,b,c,d,e,f,g,h,i,j)";
        TParseSettings small; small.MaxNodes = 128;
        UNIT_ASSERT_EXCEPTION(Parse(sql, small), TResourceLimitError);
        UNIT_ASSERT_EXCEPTION(ParseGrammar(sql, small), TResourceLimitError);
        for (const auto* clause : {"ROLLUP()", "CUBE()", "GROUPING SETS ()", "ROLLUP(id,)", "CUBE(id,,value)"}) {
            const auto invalid = std::string("SELECT COUNT(*) FROM t GROUP BY ") + clause;
            UNIT_ASSERT_EXCEPTION_C(Parse(invalid), TFrontendError, invalid);
            UNIT_ASSERT_EXCEPTION_C(ParseGrammar(invalid), TFrontendError, invalid);
        }
    }

    Y_UNIT_TEST(StructureAgreesWithHandwrittenParser) {
        for (auto sql : {
            "SELECT 1 + 2 * 3 AS x",
            "SELECT (1 + 2) * 3, NOT false OR true AND false",
            "SELECT IF(true, 1, 2), CAST(1 AS Int64), COALESCE(NULL, 2)",
            "SELECT CASE WHEN true THEN 1 ELSE 2 END",
            "SELECT CASE 1 WHEN 1 THEN 2 WHEN 3 THEN 4 END",
            "SELECT 1 IN (1,2,3), 1 NOT BETWEEN 2 AND 4, NULL IS NOT NULL",
            "SELECT 'a' IN ['a','b',], NULL NOT IN [NULL], 1 IN [], 1 IN [,]",
            "SELECT 'abc' LIKE '%b%', 1 ?? 2 ?? 3",
            "SELECT t.*, t.id FROM t AS t WHERE t.id > 1 ORDER BY t.id DESC LIMIT 10",
            "SELECT a.id, b.id FROM a LEFT JOIN b ON a.id == b.id",
            "FROM t SELECT id, SUM(value) AS total GROUP BY id HAVING SUM(value) > 0",
            "SELECT 1 AS x UNION ALL SELECT 2 AS x",
            "SELECT 1 AS x UNION SELECT 2 AS x INTERSECT SELECT 3 AS x EXCEPT ALL SELECT 4 AS x ORDER BY x LIMIT 2",
            "WITH q AS (SELECT 1 AS x) SELECT x FROM q INTERSECT ALL SELECT x FROM q UNION DISTINCT SELECT x FROM q",
            "SELECT 1 AS x UNION ALL SELECT 2 AS y ORDER BY x, y DESC LIMIT 1 OFFSET 1",
            "WITH q AS (SELECT 1 AS x) SELECT x FROM q",
            "DECLARE $p AS Uint64; SELECT $p LIMIT $p",
            "$q = (SELECT 1 AS x); SELECT x FROM $q",
            "$f = ($x) -> { RETURN $x + 1; }; SELECT $f(2)",
            "PRAGMA YqlSelect = 'force'; SELECT 1",
            "SELECT DateTime::GetYear(Date('2020-01-01'))",
        }) {
            try {
                const auto generated = ParseGrammar(sql);
                const auto handwritten = Parse(sql);
                UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(generated), SyntaxFingerprint(handwritten), sql);
            } catch (const TFrontendError& error) {
                UNIT_FAIL(TStringBuilder() << sql << ": " << error.what());
            }
        }
    }

    Y_UNIT_TEST(CompleteGrammarAndExplicitSubset) {
        for (auto sql : {
            "CREATE TABLE t (id Uint64, PRIMARY KEY(id));",
            "SELECT id FROM t GROUP COMPACT BY id;",
        }) {
            ValidateGrammar(sql);
            UNIT_ASSERT_EXCEPTION(ParseGrammar(sql), TUnsupportedFeature);
        }
        for (auto resource : {"tpch-q1", "tpcds-q47"}) {
            TString sql;
            UNIT_ASSERT(NResource::FindExact(resource, &sql));
            ValidateGrammar(std::string_view(sql.data(), sql.size()));
        }
    }

    Y_UNIT_TEST(NoRecoveredTreesOrTrailingGarbage) {
        UNIT_ASSERT_EXCEPTION(ValidateGrammar(std::string("SELECT '") + char(0xff) + "'"), TFrontendError);
        for (auto sql : {"SELECT FROM", "SELECT 1 +", "SELECT 1; rubbish", "SELECT 'unterminated", "SELECT @", "SELECT (1"}) {
            UNIT_ASSERT_EXCEPTION(ValidateGrammar(sql), TFrontendError);
        }
    }

    Y_UNIT_TEST(LexerModesAndLimits) {
        TParseSettings ansi; ansi.AnsiQuotes = true;
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(ParseGrammar("SELECT \"value\" FROM t", ansi)),
            SyntaxFingerprint(Parse("SELECT \"value\" FROM t", ansi)));
        ValidateGrammar("/* outer /* inner */ outer */ SELECT 1", ansi);
        TParseSettings small; small.MaxQueryBytes = 8;
        UNIT_ASSERT_EXCEPTION(ValidateGrammar("SELECT 123456789", small), TResourceLimitError);
        small.MaxQueryBytes = 1024; small.MaxDepth = 0;
        UNIT_ASSERT_EXCEPTION(ValidateGrammar("SELECT 1", small), TResourceLimitError);
    }
}
