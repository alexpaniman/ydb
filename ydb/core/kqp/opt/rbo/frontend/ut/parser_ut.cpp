#include <ydb/core/kqp/opt/rbo/frontend/parser.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

Y_UNIT_TEST_SUITE(KqpNativeParser) {
    Y_UNIT_TEST(ParsedSyntaxOwnsTextAfterInputAndLexerAreReleased) {
        const auto parse = [] {
            std::string sql = "PRAGMA warning('disable', '4527'); DECLARE $input AS List<Struct<`field``name`:Decimal(12,2)>>; SELECT ";
            for (unsigned i = 0; i < 300; ++i) {
                if (i) sql += ',';
                sql += "'value\\n" + std::to_string(i) + "' AS `alias``" + std::to_string(i) + "`";
            }
            sql += " FROM `table``name`;";
            auto result = Parse(sql);
            sql.assign(sql.size(), '#');
            return result;
        };
        auto parsed = parse();
        auto moved = std::move(parsed);
        UNIT_ASSERT_VALUES_EQUAL(moved.Statements[0].Name, "warning");
        UNIT_ASSERT(moved.Statements[0].PragmaValues == (std::vector<std::string>{"disable", "4527"}));
        const auto& declaration = moved.Statements[1];
        UNIT_ASSERT_VALUES_EQUAL(declaration.Name, "input");
        UNIT_ASSERT_VALUES_EQUAL(declaration.RowFields->at(0).first, "field`name");
        UNIT_ASSERT_VALUES_EQUAL(declaration.RowFields->at(0).second, "Decimal(12,2)");
        const auto& query = *moved.Statements[2].Query;
        UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "table`name");
        UNIT_ASSERT_VALUES_EQUAL(query.Projection.size(), 300);
        for (unsigned i = 0; i < 300; ++i) {
            UNIT_ASSERT_VALUES_EQUAL(query.Projection[i].Alias, "alias`" + std::to_string(i));
            UNIT_ASSERT_VALUES_EQUAL(moved.Expressions[query.Projection[i].Expression].Text, "value\n" + std::to_string(i));
        }
    }

    Y_UNIT_TEST(YqlPrecedence) {
        const auto parsed = Parse("SELECT 1 + 2 * 3 == 7 AND true OR false, 'a' || 'b' * 2;");
        const auto& query = *parsed.Statements[0].Query;
        const auto& root = parsed.Expressions[query.Projection[0].Expression];
        UNIT_ASSERT_VALUES_EQUAL(root.Text, "OR");
        const auto& conjunction = parsed.Expressions[root.Arguments[0]];
        UNIT_ASSERT_VALUES_EQUAL(conjunction.Text, "AND");
        const auto& equality = parsed.Expressions[conjunction.Arguments[0]];
        UNIT_ASSERT_VALUES_EQUAL(equality.Text, "==");
        const auto& addition = parsed.Expressions[equality.Arguments[0]];
        UNIT_ASSERT_VALUES_EQUAL(addition.Text, "+");
        UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[addition.Arguments[1]].Text, "*");
        const auto& multiply = parsed.Expressions[query.Projection[1].Expression];
        UNIT_ASSERT_VALUES_EQUAL(multiply.Text, "*");
        UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[multiply.Arguments[0]].Text, "||");
    }

    Y_UNIT_TEST(StatementsAndLambdas) {
        const auto parsed = Parse(R"(
            DECLARE $limit AS Uint64;
            $twice = ($x) -> { RETURN $x * 2; };
            $rows = (SELECT id FROM `/Root/a` WHERE id > 0);
            SELECT $twice(id) AS x FROM $rows ORDER BY x DESC LIMIT $limit OFFSET 1;
        )");
        UNIT_ASSERT_VALUES_EQUAL(parsed.Statements.size(), 4);
        const auto& lambda = parsed.Expressions[parsed.Statements[1].Expression];
        UNIT_ASSERT(lambda.Kind == ESyntaxKind::Lambda);
        UNIT_ASSERT_VALUES_EQUAL(lambda.Lambda->Parameters[0], "x");
        UNIT_ASSERT(parsed.Statements[2].Query);
        const auto& query = *parsed.Statements[3].Query;
        UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "$rows");
        UNIT_ASSERT(query.OrderBy[0].Descending);
        UNIT_ASSERT(query.Offset != NoSyntax);
    }

    Y_UNIT_TEST(QuotedNamesCommentsAndAnsi) {
        const auto parsed = Parse("/* comment */ SELECT `a``b` AS `from`, \"a\\n\" FROM `/Root/t`; -- tail");
        const auto& query = *parsed.Statements[0].Query;
        UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[query.Projection[0].Expression].Text, "a`b");
        UNIT_ASSERT_VALUES_EQUAL(query.Projection[0].Alias, "from");
        UNIT_ASSERT_VALUES_EQUAL(parsed.Expressions[query.Projection[1].Expression].Text, "a\n");
        TParseSettings settings;
        settings.AnsiQuotes = true;
        const auto ansi = Parse("SELECT \"x\" FROM `t`", settings);
        UNIT_ASSERT(ansi.Expressions[ansi.Statements[0].Query->Projection[0].Expression].Kind == ESyntaxKind::Column);
        Parse("/* outer /* inner */ outer */ SELECT 1", settings);
    }

    Y_UNIT_TEST(StringSuffixMustBeAdjacent) {
        const auto parsed = Parse("SELECT 'x' u");
        const auto& projection = parsed.Statements[0].Query->Projection;
        UNIT_ASSERT_VALUES_EQUAL(projection[0].Alias, "u");
        UNIT_ASSERT(parsed.Expressions[projection[0].Expression].Kind == ESyntaxKind::String);
        const auto typed = Parse("SELECT 'y'u AS y");
        const auto& value = typed.Expressions[typed.Statements[0].Query->Projection[0].Expression];
        UNIT_ASSERT(value.Kind == ESyntaxKind::Utf8String);
        UNIT_ASSERT_VALUES_EQUAL(value.Text, "y");
    }

    Y_UNIT_TEST(CasePredicatesAndJoins) {
        const auto parsed = Parse(R"(
            SELECT CASE WHEN a.x BETWEEN 1 AND 3 THEN a.x ELSE 0 END AS x
            FROM a LEFT JOIN b ON a.id == b.id
            WHERE b.x IS NULL OR a.x NOT IN (1, 2, NULL)
            GROUP BY a.x HAVING COUNT(*) > 0;
        )");
        const auto& query = *parsed.Statements[0].Query;
        UNIT_ASSERT(query.Joins[0].Kind == EJoinKind::Left);
        UNIT_ASSERT(parsed.Expressions[query.Projection[0].Expression].Kind == ESyntaxKind::Case);
        UNIT_ASSERT_VALUES_EQUAL(query.GroupBy.size(), 1);
        UNIT_ASSERT(query.Having != NoSyntax);
    }

    Y_UNIT_TEST(RejectsIncompleteAndUnsupportedStatements) {
        for (auto text : {"SELECT", "SELECT 1 +", "SELECT 1 FROM", "SELECT 'unterminated", "SELECT 1 /*", "SELECT 1 UNION"}) {
            UNIT_ASSERT_EXCEPTION(Parse(text), TFrontendError);
        }
        try {
            Parse("SELECT 1;\nSELECT (");
            UNIT_FAIL("expected error");
        } catch (const TFrontendError& error) {
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 2);
        }
    }

    Y_UNIT_TEST(BoundedNesting) {
        TParseSettings settings;
        settings.MaxDepth = 16;
        UNIT_ASSERT_EXCEPTION(Parse("SELECT " + std::string(40, '(') + "1" + std::string(40, ')'), settings), TFrontendError);
    }

    Y_UNIT_TEST(LongFlatProjectionAndTopologicalSyntax) {
        std::string query = "SELECT ";
        for (unsigned i = 0; i < 5000; ++i) query += (i ? "," : "") + std::to_string(i) + "+1";
        const auto parsed = Parse(query);
        UNIT_ASSERT_VALUES_EQUAL(parsed.Statements[0].Query->Projection.size(), 5000);
        for (std::size_t i = 0; i < parsed.Expressions.size(); ++i) for (auto child : parsed.Expressions[i].Arguments) UNIT_ASSERT(child < i);
    }
}
