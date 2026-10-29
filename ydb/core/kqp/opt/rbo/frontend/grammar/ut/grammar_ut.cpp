#include <ydb/core/kqp/opt/rbo/frontend/grammar/grammar.h>
#include <ydb/core/kqp/opt/rbo/frontend/binder.h>
#include <ydb/core/kqp/opt/rbo/frontend/diagnostic.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/resource/resource.h>

using namespace NKikimr::NKqp::NFrontend;

Y_UNIT_TEST_SUITE(KqpNativeGrammar) {
    Y_UNIT_TEST(StructureAgreesWithHandwrittenParser) {
        for (auto sql : {
            "SELECT 1 + 2 * 3 AS x",
            "SELECT (1 + 2) * 3, NOT false OR true AND false",
            "SELECT IF(true, 1, 2), CAST(1 AS Int64), COALESCE(NULL, 2)",
            "SELECT CASE WHEN true THEN 1 ELSE 2 END",
            "SELECT CASE 1 WHEN 1 THEN 2 WHEN 3 THEN 4 END",
            "SELECT 1 IN (1,2,3), 1 NOT BETWEEN 2 AND 4, NULL IS NOT NULL",
            "SELECT 'abc' LIKE '%b%', 1 ?? 2 ?? 3",
            "SELECT t.*, t.id FROM t AS t WHERE t.id > 1 ORDER BY t.id DESC LIMIT 10",
            "SELECT a.id, b.id FROM a LEFT JOIN b ON a.id == b.id",
            "FROM t SELECT id, SUM(value) AS total GROUP BY id HAVING SUM(value) > 0",
            "SELECT 1 AS x UNION ALL SELECT 2 AS x",
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
            "UPSERT INTO t (id) VALUES (1);",
            "SELECT ROW_NUMBER() OVER () FROM t;",
            "SELECT id FROM t GROUP BY ROLLUP(id);",
            "SELECT 1 INTERSECT SELECT 2;",
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
