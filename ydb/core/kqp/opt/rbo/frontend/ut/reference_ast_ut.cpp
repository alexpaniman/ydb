// Syntax boundary cases from PR #58587 at fbeb81657ef, adapted to the native
// owning syntax arena. These assertions do not imply binder/runtime admission.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TParsedQuery Ast(std::string text, bool grammar) {
    TCompilerSettings settings;
    settings.Parser = grammar ? EParser::Grammar : EParser::Fast;
    return ParseNative(text, settings);
}

const TSyntaxExpr& Projection(const TParsedQuery& syntax, std::size_t index) {
    return syntax.Expressions.at(syntax.Statements.at(0).Query->Projection.at(index).Expression);
}

const TSyntaxExpr& Child(const TParsedQuery& syntax, const TSyntaxExpr& expression, std::size_t index) {
    return syntax.Expressions.at(expression.Arguments.at(index));
}
}

Y_UNIT_TEST_SUITE(KqpNativeReferenceAst) {
    Y_UNIT_TEST(Predicates) {
        const auto* sql = R"(SELECT x NOT IN (1, 2), x NOT BETWEEN SYMMETRIC 1 AND 3,
            x IS NOT NULL, x IS NOT DISTINCT FROM y, name NOT LIKE 'a%' ESCAPE '!';)";
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)));
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(sql, grammar);
            const auto& in = Projection(syntax, 0);
            UNIT_ASSERT(in.Kind == ESyntaxKind::Unary && in.Text == "NOT");
            UNIT_ASSERT(Child(syntax, in, 0).Kind == ESyntaxKind::In);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, in, 0).Arguments.size(), 3);
            const auto& between = Projection(syntax, 1);
            UNIT_ASSERT(between.Kind == ESyntaxKind::Between && between.Text == "NOT BETWEEN SYMMETRIC");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, between, 1).Text, "1");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, between, 2).Text, "3");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 2).Text, "IS NOT NULL");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "IS NOT DISTINCT FROM");
            const auto& like = Projection(syntax, 4);
            UNIT_ASSERT(like.Kind == ESyntaxKind::Unary && like.Text == "NOT");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, like, 0).Text, "LIKE");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Child(syntax, like, 0), 2).Text, "!");
        }
    }

    Y_UNIT_TEST(NamedPreambleAndLambdaBodies) {
        const auto* sql = "$f = ($x, $y) -> { $n = $x + 1; return $n + $y; }; "
            "$g = () -> (42); $q = SELECT $f(id, 2) AS k FROM t; "
            "$r = (WITH c AS (SELECT k FROM $q) SELECT * FROM c); SELECT * FROM $r;";
        const auto fast = Parse(sql), grammar = ParseGrammar(sql);
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
        for (const auto* parsed : {&fast, &grammar}) {
            UNIT_ASSERT_VALUES_EQUAL(parsed->Statements.size(), 5);
            const auto& f = parsed->Expressions[parsed->Statements[0].Expression];
            UNIT_ASSERT(f.Kind == ESyntaxKind::Lambda);
            UNIT_ASSERT(f.Lambda->Parameters == (std::vector<std::string>{"x", "y"}));
            UNIT_ASSERT(f.Lambda->Locals == (std::vector<std::string>{"n"}));
            UNIT_ASSERT_VALUES_EQUAL(f.Arguments.size(), 2);
            UNIT_ASSERT(parsed->Expressions[f.Arguments.back()].Kind == ESyntaxKind::Binary);
            UNIT_ASSERT(parsed->Expressions[parsed->Statements[1].Expression].Lambda->Parameters.empty());
            UNIT_ASSERT(parsed->Statements[2].Query);
            UNIT_ASSERT_VALUES_EQUAL(parsed->Statements[3].Query->Ctes.size(), 1);
        }
    }

    Y_UNIT_TEST(SetOperationRejectsMisplacedTrailingClauses) {
        for (const bool grammar : {false, true}) {
            for (const auto* sql : {
                "SELECT 1 AS n ORDER BY n UNION ALL SELECT 2 AS n;",
                "SELECT 1 AS n LIMIT 1 INTERSECT SELECT 2 AS n;",
                "SELECT 1 AS n UNION ALL SELECT 2 AS n ORDER BY n EXCEPT SELECT 3 AS n;",
                "SELECT 1 AS n UNION ALL SELECT 2 AS n ASSUME ORDER BY n;"}) {
                UNIT_ASSERT_EXCEPTION_C(Ast(sql, grammar), TFrontendError, sql);
            }
        }
    }

    Y_UNIT_TEST(SetOperationTrailingClauses) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT 3 AS n UNION ALL SELECT 2 AS n INTERSECT SELECT 1 AS n ORDER BY n DESC LIMIT 2 OFFSET 1;", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            UNIT_ASSERT_VALUES_EQUAL(query.OrderBy.size(), 1);
            UNIT_ASSERT(query.OrderBy[0].Descending);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Limit).Text, "2");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Offset).Text, "1");
            UNIT_ASSERT_VALUES_EQUAL(query.SetInputs.size(), 3);
            for (const auto& input : query.SetInputs) {
                UNIT_ASSERT(input->OrderBy.empty());
                UNIT_ASSERT(input->Limit == NoSyntax);
            }
            const auto local = Ast("(SELECT 3 AS n ORDER BY n LIMIT 1) UNION ALL (SELECT 2 AS n ORDER BY n DESC LIMIT 1);", grammar);
            const auto& compound = *local.Statements.at(0).Query;
            UNIT_ASSERT(compound.OrderBy.empty());
            UNIT_ASSERT(compound.Limit == NoSyntax);
            UNIT_ASSERT_VALUES_EQUAL(compound.SetInputs.size(), 2);
            for (const auto& input : compound.SetInputs) {
                UNIT_ASSERT_VALUES_EQUAL(input->OrderBy.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(local.Expressions.at(input->Limit).Text, "1");
            }
        }
    }

    Y_UNIT_TEST(PrecedenceAndAssociativity) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT 10 - 3 - 2 * 4, a OR b AND c, x ?? y ?? z, -x * y;", grammar);
            const auto& subtract = Projection(syntax, 0);
            UNIT_ASSERT(subtract.Kind == ESyntaxKind::Binary);
            UNIT_ASSERT_VALUES_EQUAL(subtract.Text, "-");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, subtract, 0).Text, "-");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, subtract, 1).Text, "*");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Child(syntax, subtract, 0), 0).Text, "10");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Child(syntax, subtract, 1), 1).Text, "4");
            const auto& logical = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(logical.Text, "OR");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, logical, 1).Text, "AND");
            const auto& coalesce = Projection(syntax, 2);
            UNIT_ASSERT_VALUES_EQUAL(coalesce.Text, "??");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, coalesce, 1).Text, "??");
            const auto& negate = Child(syntax, Projection(syntax, 3), 0);
            UNIT_ASSERT(negate.Kind == ESyntaxKind::Unary);
            UNIT_ASSERT_VALUES_EQUAL(negate.Text, "-");
        }
    }

    Y_UNIT_TEST(SelectClausesAndJoin) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(
                SELECT DISTINCT t.id AS id, COUNT(DISTINCT u.value) AS n
                FROM `/Root/t` AS t LEFT JOIN `/Root/u` AS u ON t.id = u.id
                WHERE t.id > $min GROUP BY t.id HAVING COUNT(*) > 1
                ORDER BY n DESC, id ASC LIMIT 10 OFFSET 2;
            )", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            UNIT_ASSERT(query.Distinct);
            UNIT_ASSERT_VALUES_EQUAL(query.Projection.at(0).Alias, "id");
            UNIT_ASSERT(Projection(syntax, 1).Distinct);
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "/Root/t");
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Alias, "t");
            const auto& join = query.Joins.at(0);
            UNIT_ASSERT(join.Kind == EJoinKind::Left);
            UNIT_ASSERT_VALUES_EQUAL(join.Source.Name, "/Root/u");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(join.Predicate).Text, "=");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, syntax.Expressions.at(query.Where), 1).Text, "min");
            UNIT_ASSERT_VALUES_EQUAL(query.GroupBy.size(), 1);
            const auto& count = Child(syntax, syntax.Expressions.at(query.Having), 0);
            UNIT_ASSERT(Child(syntax, count, 0).Kind == ESyntaxKind::Star);
            UNIT_ASSERT_VALUES_EQUAL(query.OrderBy.size(), 2);
            UNIT_ASSERT(query.OrderBy[0].Descending);
            UNIT_ASSERT(!query.OrderBy[1].Descending);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Limit).Text, "10");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Offset).Text, "2");
            // The reference's cluster qualifier is outside this native catalog
            // contract. It must be rejected, never stripped from the table name.
            UNIT_ASSERT_EXCEPTION(Ast("SELECT * FROM plato.`/Root/t`;", grammar), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(DerivedTableAndCommaLimit) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT d.id FROM (SELECT id FROM t LIMIT 2, 5) AS d;", grammar);
            const auto& source = *syntax.Statements.at(0).Query->Source;
            UNIT_ASSERT_VALUES_EQUAL(source.Alias, "d");
            UNIT_ASSERT(source.Query);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(source.Query->Limit).Text, "5");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(source.Query->Offset).Text, "2");
        }
    }

    Y_UNIT_TEST(InSingletonCollectionsPreserveTheirOuterTuple) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT x IN (1), x IN ((1, 2)), x IN $items, x IN (SELECT id FROM t), (1), x IN ((SELECT id FROM t)), "
                "x IN ((SELECT id FROM t),), x IN (((SELECT id FROM t)));", grammar);
            const auto& singleton = Projection(syntax, 0);
            UNIT_ASSERT(singleton.Kind == ESyntaxKind::In);
            UNIT_ASSERT_VALUES_EQUAL(singleton.Arguments.size(), 2);
            UNIT_ASSERT(Child(syntax, singleton, 1).Kind == ESyntaxKind::Integer);
            const auto& tuples = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(tuples.Arguments.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, tuples, 1).Arguments.size(), 2);
            UNIT_ASSERT(Child(syntax, Projection(syntax, 2), 1).Kind == ESyntaxKind::Variable);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Projection(syntax, 2), 1).Text, "items");
            UNIT_ASSERT(Projection(syntax, 3).Query);
            UNIT_ASSERT(Projection(syntax, 4).Kind == ESyntaxKind::Integer);
            UNIT_ASSERT(Projection(syntax, 5).Query);
            UNIT_ASSERT(!Projection(syntax, 6).Query);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 6).Arguments.size(), 2);
            UNIT_ASSERT(Child(syntax, Projection(syntax, 6), 1).Kind == ESyntaxKind::ScalarSubquery);
            UNIT_ASSERT(Projection(syntax, 7).Query);
        }
    }

    Y_UNIT_TEST(ParenthesesCollectionsAndSubqueries) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT (1 + 2), (1,), (), [1, 2], EXISTS (SELECT 1 FROM t), (SELECT MAX(x) FROM u);", grammar);
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Binary);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 0).Text, "+");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 1).Text, "AsTuple");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 1).Arguments.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 2).Text, "AsTuple");
            UNIT_ASSERT(Projection(syntax, 2).Arguments.empty());
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "AsList");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Arguments.size(), 2);
            UNIT_ASSERT(Projection(syntax, 4).Kind == ESyntaxKind::Exists && Projection(syntax, 4).Query);
            UNIT_ASSERT(Projection(syntax, 5).Kind == ESyntaxKind::ScalarSubquery && Projection(syntax, 5).Query);
        }
    }

    Y_UNIT_TEST(CaseAndCast) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT CASE x WHEN 1 THEN 'one' ELSE 'many' END, CASE WHEN x > 0 THEN 1 END, "
                "CAST($x AS Int64?), CAST(1 AS Decimal(22, 9)), CAST($items AS List<Int32>);", grammar);
            const auto& simple = Projection(syntax, 0);
            UNIT_ASSERT(simple.Kind == ESyntaxKind::Case);
            UNIT_ASSERT_VALUES_EQUAL(simple.Arguments.size(), 3);
            const auto& condition = Child(syntax, simple, 0);
            UNIT_ASSERT_VALUES_EQUAL(condition.Text, "==");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, condition, 0).Text, "x");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, condition, 1).Text, "1");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, simple, 2).Text, "many");
            const auto& searched = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(searched.Text, "implicit_else");
            UNIT_ASSERT(Child(syntax, searched, 2).Kind == ESyntaxKind::Null);
            UNIT_ASSERT(Projection(syntax, 2).Kind == ESyntaxKind::Cast);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 2).Text, "Int64?");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "Decimal(22,9)");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 4).Text, "List<Int32>");
        }
    }

    Y_UNIT_TEST(CteAndSetOperationPrecedence) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("WITH c(id) AS (SELECT 1) SELECT id FROM c UNION ALL SELECT 2 INTERSECT SELECT 3 EXCEPT SELECT 4;", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            UNIT_ASSERT_VALUES_EQUAL(query.Ctes.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(query.Ctes[0].Name, "c");
            UNIT_ASSERT_VALUES_EQUAL(query.Ctes[0].Columns.at(0), "id");
            const std::vector<ESetOperation> expected{ESetOperation::Push, ESetOperation::Push, ESetOperation::Push,
                ESetOperation::Intersect, ESetOperation::UnionAll, ESetOperation::Push, ESetOperation::Except};
            UNIT_ASSERT(query.SetOperations == expected);
        }
    }

    Y_UNIT_TEST(GroupingSets) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT a, COUNT(*) FROM t GROUP BY GROUPING SETS ((), ROLLUP(a, b), CUBE(a, b));", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            // Native syntax expands bounded grouping combinations once. Retain
            // duplicate sets: they produce duplicate aggregate result groups.
            std::vector<std::vector<std::string>> sets;
            for (const auto& set : query.GroupingSets) {
                std::vector<std::string> keys;
                for (auto key : set) keys.push_back(syntax.Expressions.at(query.GroupBy.at(key).Expression).Text);
                sets.push_back(std::move(keys));
            }
            std::vector<std::vector<std::string>> expected{{}, {"a", "b"}, {"a"}, {}, {}, {"a"}, {"b"}, {"a", "b"}};
            std::sort(sets.begin(), sets.end());
            std::sort(expected.begin(), expected.end());
            UNIT_ASSERT(sets == expected);
        }
    }

    Y_UNIT_TEST(MemberIndexAndModuleCall) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT t.record.field, $items[0], Math::Abs(-$value);", grammar);
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Column);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 0).Text, "t.record.field");
            const auto& index = Projection(syntax, 1);
            UNIT_ASSERT(index.Kind == ESyntaxKind::Index);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, index, 0).Text, "items");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, index, 1).Text, "0");
            const auto& call = Projection(syntax, 2);
            UNIT_ASSERT(call.Kind == ESyntaxKind::Call);
            UNIT_ASSERT_VALUES_EQUAL(call.Text, "Math::Abs");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, call, 0).Text, "-");
        }
    }

    Y_UNIT_TEST(SourcePositionsAndOwnership) {
        for (const bool grammar : {false, true}) {
            auto syntax = Ast("-- comment\nSELECT 'Ж', x + 12;", grammar);
            const auto& literal = Projection(syntax, 0);
            UNIT_ASSERT_VALUES_EQUAL(literal.Position.Line, 2);
            UNIT_ASSERT_VALUES_EQUAL(literal.Position.Column, 8);
            UNIT_ASSERT_VALUES_EQUAL(literal.Text, "Ж");
            const auto& binary = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(binary.Position.Line, 2);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, binary, 0).Text, "x");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, binary, 1).Text, "12");
            // Ast receives its SQL by value, so the source and parser have already
            // been destroyed. Moving the owning arena must preserve its contents.
            const auto moved = std::move(syntax);
            UNIT_ASSERT_VALUES_EQUAL(Projection(moved, 0).Text, "Ж");
            UNIT_ASSERT_VALUES_EQUAL(Child(moved, Projection(moved, 1), 0).Text, "x");
        }
    }

    Y_UNIT_TEST(EmptyScript) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("-- comment\n", grammar);
            UNIT_ASSERT(syntax.Statements.empty());
            UNIT_ASSERT(syntax.Expressions.empty());
        }
    }

    Y_UNIT_TEST(UnsupportedSyntaxDoesNotDisappear) {
        for (const bool grammar : {false, true}) {
            for (const auto* sql : {
                "SELECT * WITHOUT id FROM t;",
                "SELECT x = y = z;",
                "SELECT * FROM t OUTER JOIN u;",
                "SELECT * FROM t WITH SCHEMA Callable<() -> Int32>;",
                "SELECT * FROM ANY t;"}) {
                UNIT_ASSERT_EXCEPTION_C(Ast(sql, grammar), TFrontendError, sql);
            }
            // DELETE was outside the reference AST profile. Our effects profile
            // accepts it, so require its complete statement instead of dropping it.
            for (const auto* sql : {"SELECT 1; DELETE FROM t;", "PRAGMA OrderedColumns; DELETE FROM t;"}) {
                const auto syntax = Ast(sql, grammar);
                UNIT_ASSERT_VALUES_EQUAL(syntax.Statements.size(), 2);
                const auto& statement = syntax.Statements[1];
                UNIT_ASSERT(statement.Kind == EStatementKind::Write && statement.Write);
                UNIT_ASSERT(statement.Write->Kind == EWriteKind::Delete);
                UNIT_ASSERT_VALUES_EQUAL(statement.Write->Target.Name, "t");
                UNIT_ASSERT(statement.Write->Where == NoSyntax);
            }
        }
    }

    Y_UNIT_TEST(LiteralKindsAndSpellings) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT NULL, true, 1.25, 42ul, 'a\\n';", grammar);
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Null);
            UNIT_ASSERT(Projection(syntax, 1).Kind == ESyntaxKind::Boolean);
            UNIT_ASSERT(Projection(syntax, 2).Kind == ESyntaxKind::Real);
            UNIT_ASSERT(Projection(syntax, 3).Kind == ESyntaxKind::Integer);
            UNIT_ASSERT(Projection(syntax, 4).Kind == ESyntaxKind::String);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "42ul");
            // Native syntax owns decoded strings instead of retaining quote tokens.
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 4).Text, "a\n");
        }
    }
}
