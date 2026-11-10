// Native-boundary ports of PR #58587 type-annotation tests at fbeb81657ef.
// Keep SQL and semantic checks; inspect native schemas and dependencies rather
// than YQL lambdas. Public star presentation is checked by name.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <library/cpp/testing/unittest/registar.h>
#include <algorithm>
#include <unordered_set>

using namespace NKikimr::NKqp::NFrontend;

namespace {
struct TFixture {
    TCompilerSettings Settings;
    TBoundProgram Program;
    std::vector<TCatalogColumn> Columns{
        {"id", {ETypeKind::Int64}}, {"value", {ETypeKind::String, true}},
        {"score", {ETypeKind::Int32, true}}, {"amount", {ETypeKind::Decimal, false, 12, 2}}};
    void Ok(std::string_view sql) {
        auto result = CompileNative(sql, [&](std::string_view name, TSourcePosition) {
            return TCatalogTable{std::string(name), Columns};
        }, Settings);
        Program = std::move(*result.Program).Release();
    }
    TType Output(std::size_t index, std::size_t result = 0) const {
        return Program.Results.at(result).Columns.at(index).Type;
    }
    void Types(std::initializer_list<std::string_view> expected, std::size_t result = 0) const {
        UNIT_ASSERT_VALUES_EQUAL(Program.Results.at(result).Columns.size(), expected.size());
        std::size_t i = 0;
        for (const auto type : expected) {
            UNIT_ASSERT_VALUES_EQUAL(Output(i, result).ToString(), ParseType(type).ToString());
            ++i;
        }
    }
    TExprId Expression(TColumnId column) {
        for (const auto& relation : Program.Relations) for (const auto& definition : relation.Definitions)
            if (definition.Column == column) return definition.Expression;
        return Program.Expressions.Column(column, Program.Columns.at(column).Type);
    }
    TExprId Projection(std::size_t index) { return Expression(Program.Results.at(0).Columns.at(index).Column); }
    std::unordered_set<TColumnId> Inputs(TExprId expression) const {
        std::unordered_set<TColumnId> columns;
        std::unordered_set<TExprId> seen;
        std::vector<TExprId> pending{expression};
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (!seen.insert(id).second) continue;
            const auto& node = Program.Expressions[id];
            if (node.Op == EOpcode::Column) columns.insert(node.ColumnId);
            pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
        }
        return columns;
    }
    std::vector<const TRelation*> Find(ERelation kind) const {
        std::vector<const TRelation*> found;
        for (const auto& relation : Program.Relations) if (relation.Kind == kind) found.push_back(&relation);
        return found;
    }
};
}

Y_UNIT_TEST_SUITE(KqpNativeReferenceTypes) {
    Y_UNIT_TEST(SetMatchingColumnsPreserveMixedCoercionPositions) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar})
        for (const std::string op : {"UNION ALL", "UNION", "INTERSECT", "EXCEPT"}) {
            TFixture f; f.Settings.Parser = parser;
            // Default YQL sets match names, while the reference frontend test
            // matches positions. Give both operands the same aliases to retain
            // its mixed-coercion contract without changing SQL set semantics.
            f.Ok("SELECT id AS same, id AS widened, value AS nullable, amount AS decimal_same, "
                "amount AS decimal_widened FROM t " + op + " SELECT id AS same, score AS widened, "
                "value AS nullable, amount AS decimal_same, CAST(amount AS Decimal(15,2)) "
                "AS decimal_widened FROM t ORDER BY same;");
            f.Types({"Int64", "Int64?", "String?", "Decimal(12,2)", "Decimal(15,2)"});
            std::unordered_map<TColumnId, TExprId> definitions;
            for (const auto& relation : f.Program.Relations) for (const auto& definition : relation.Definitions)
                definitions.emplace(definition.Column, definition.Expression);
            std::unordered_set<std::string> conversions;
            for (const auto& relation : f.Program.Relations) for (const auto& definition : relation.Definitions) {
                const auto& expression = f.Program.Expressions[definition.Expression];
                if (expression.Op != EOpcode::Convert) continue;
                auto source = expression.Arguments.at(0);
                // Repeated projections acquire distinct output IDs. Follow
                // their identity maps back to the original catalog field.
                while (f.Program.Expressions[source].Op == EOpcode::Column) {
                    const auto found = definitions.find(f.Program.Expressions[source].ColumnId);
                    if (found == definitions.end()) break;
                    source = found->second;
                }
                const auto& input = f.Program.Expressions[source];
                UNIT_ASSERT(input.Op == EOpcode::Column);
                conversions.insert(f.Program.Columns.at(input.ColumnId).Name + ":" + input.Type.ToString()
                    + "->" + f.Program.Columns.at(definition.Column).Name + ":" + expression.Type.ToString());
                UNIT_ASSERT(expression.Type == f.Program.Columns.at(definition.Column).Type);
            }
            // Widening and optional wrapping must still reference the right
            // operand's field, with unchanged fields between the conversions.
            UNIT_ASSERT(conversions.contains("id:Int64->widened:Int64?"));
            UNIT_ASSERT(conversions.contains("score:Int32?->widened:Int64?"));
            UNIT_ASSERT(conversions.contains("amount:Decimal(12,2)->decimal_widened:Decimal(15,2)"));
        }
    }

    Y_UNIT_TEST(PreambleLambdasArePolymorphicAndCaptureLexically) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TFixture f; f.Settings.Parser = parser;
            f.Ok("$identity = ($x) -> ($x); $make = ($x) -> { $n = $x + 1; "
                "return ($y) -> ($n + $y); }; $zero = () -> (42); "
                "SELECT $identity(id), $identity(value), $make(id)(2), $zero() FROM t;");
            f.Types({"Int64", "String?", "Int64", "Int32"});
        }
    }

    Y_UNIT_TEST(ScalarInputsContainOnlyReferencedColumns) {
        TFixture f;
        f.Ok("SELECT id + id, 42, value FROM t WHERE score > 0 ORDER BY id LIMIT 5;");
        UNIT_ASSERT_VALUES_EQUAL(f.Inputs(f.Projection(0)).size(), 1);
        UNIT_ASSERT(f.Inputs(f.Projection(1)).empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Inputs(f.Projection(2)).size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Inputs(*f.Find(ERelation::Filter).at(0)->Predicate).size(), 1);
        UNIT_ASSERT(f.Inputs(*f.Find(ERelation::Limit).at(0)->Count).empty());
    }

    Y_UNIT_TEST(ScalarInputsKeepDistinctJoinColumnsAndLambdaArguments) {
        TFixture f;
        f.Ok("$add = ($x, $y) -> ($x + $y); SELECT $add(l.id, r.id), l.* FROM t AS l JOIN t AS r ON l.id = r.id;");
        UNIT_ASSERT_VALUES_EQUAL(f.Inputs(f.Projection(0)).size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns.size(), 5);
        const auto reads = f.Find(ERelation::Read);
        UNIT_ASSERT_VALUES_EQUAL(reads.size(), 2);
        for (auto id : reads[0]->Columns)
            UNIT_ASSERT(std::find(reads[1]->Columns.begin(), reads[1]->Columns.end(), id) == reads[1]->Columns.end());
    }

    Y_UNIT_TEST(ScalarInputsExcludeSiblingSubqueries) {
        TFixture f;
        f.Ok("SELECT l.id, (SELECT MAX(r.id) FROM t AS r WHERE r.id = l.id), "
            "(SELECT COUNT(*) FROM t AS s WHERE s.score = l.score) FROM t AS l;");
        for (unsigned i = 0; i < 3; ++i) UNIT_ASSERT_VALUES_EQUAL(f.Inputs(f.Projection(i)).size(), 1);
        UNIT_ASSERT(f.Inputs(f.Projection(1)) != f.Inputs(f.Projection(2)));
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Subplans.size(), 2);
        for (const auto* capture : f.Find(ERelation::Capture)) UNIT_ASSERT_VALUES_EQUAL(capture->Captures.size(), 1);
    }

    Y_UNIT_TEST(ScalarSubqueryInputsStayLocalToEachExpression) {
        TFixture f;
        f.Ok("SELECT l.id + (SELECT MAX(r.id) FROM t AS r WHERE r.id = l.id) "
            "+ (SELECT MAX(s.id) FROM t AS s WHERE s.score = l.score), l.id FROM t AS l;");
        f.Types({"Int64?", "Int64"});
        UNIT_ASSERT_VALUES_EQUAL(f.Inputs(f.Projection(0)).size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(f.Inputs(f.Projection(1)).size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Subplans.size(), 2);
    }

    Y_UNIT_TEST(SetOrderingUsesCoercedPositionalTypes) {
        TFixture f;
        f.Ok("SELECT value AS z, id AS a FROM t UNION ALL SELECT 'text' AS z, score AS a FROM t "
            "ORDER BY a + 1, z LIMIT 2 OFFSET 1;");
        f.Types({"String?", "Int64?"});
        const auto& sort = *f.Find(ERelation::Sort).at(0);
        UNIT_ASSERT(f.Program.Columns[sort.SortKeys[0].Column].Type == ParseType("Int64?"));
        UNIT_ASSERT(f.Program.Columns[sort.SortKeys[1].Column].Type == ParseType("String?"));
        const auto& limit = *f.Find(ERelation::Limit).at(0);
        UNIT_ASSERT(f.Program.Expressions[*limit.Count].Type == ParseType("Uint64"));
        UNIT_ASSERT(f.Program.Expressions[*limit.Offset].Type == ParseType("Uint64"));
    }

    Y_UNIT_TEST(AnnotatesScalarQueryAndRetainsRewrites) {
        TFixture f;
        f.Ok("SELECT id + 1 AS next, value ?? 'none', CAST(id AS Double), amount, [1, 2][0u] "
            "FROM t WHERE score > 0 ORDER BY next;");
        f.Types({"Int64", "String", "Double", "Decimal(12,2)", "Int32?"});
        UNIT_ASSERT(f.Program.Expressions[f.Projection(4)].Op == EOpcode::ListElement);
        UNIT_ASSERT(f.Program.Expressions[*f.Find(ERelation::Filter)[0]->Predicate].Type.Kind == ETypeKind::Bool);
    }

    Y_UNIT_TEST(StarAndCatalogNullability) {
        TFixture f; f.Ok("SELECT t.* FROM t;");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns.size(), f.Columns.size());
        for (const auto& column : f.Program.Results[0].Columns) {
            const auto expected = std::find_if(f.Columns.begin(), f.Columns.end(), [&](const auto& input) { return input.Name == column.Name; });
            UNIT_ASSERT(expected != f.Columns.end()); UNIT_ASSERT(column.Type == expected->Type);
        }
        const auto& read = *f.Find(ERelation::Read).at(0);
        for (std::size_t i = 0; i < f.Columns.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(f.Program.Columns[read.Columns[i]].Name, f.Columns[i].Name);
            UNIT_ASSERT(f.Program.Columns[read.Columns[i]].Type == f.Columns[i].Type);
        }
    }

    Y_UNIT_TEST(CteAndDerivedOutputsAreTypedBeforeConsumers) {
        TFixture f;
        f.Ok("WITH c(key, label) AS (SELECT id + 1, value ?? 'none' FROM t) "
            "SELECT d.key + 1, d.label FROM (SELECT key, label FROM c) AS d;");
        f.Types({"Int64", "String"});
        for (const auto& column : f.Program.Columns) f.Program.Expressions.ValidateType(column.Type);
    }

    Y_UNIT_TEST(LeftJoinNullabilityDoesNotAffectOnOrInputSchemas) {
        TFixture f;
        f.Ok("SELECT l.id, r.id, r.score FROM t AS l LEFT JOIN t AS r ON l.id = r.id;");
        f.Types({"Int64", "Int64?", "Int32?"});
        for (const auto* read : f.Find(ERelation::Read)) UNIT_ASSERT(f.Program.Columns[read->Columns[0]].Type == ParseType("Int64"));
        const auto predicate = *f.Find(ERelation::Join).at(0)->Predicate;
        UNIT_ASSERT(f.Program.Expressions[predicate].Type == ParseType("Bool"));
        const auto& on = f.Program.Expressions[predicate];
        for (auto argument : on.Arguments) UNIT_ASSERT(!f.Program.Expressions[argument].Type.OptionalDepth);
    }

    Y_UNIT_TEST(RightFullAndSemiJoins) {
        TFixture right; right.Ok("SELECT l.id, r.id FROM t AS l RIGHT JOIN t AS r ON l.id = r.id;"); right.Types({"Int64?", "Int64"});
        TFixture full; full.Ok("SELECT l.id, r.id FROM t AS l FULL JOIN t AS r ON l.id = r.id;"); full.Types({"Int64?", "Int64?"});
        TFixture semi; semi.Ok("SELECT l.id FROM t AS l LEFT SEMI JOIN t AS r ON l.id = r.id;"); semi.Types({"Int64"});
        UNIT_ASSERT_VALUES_EQUAL(semi.Find(ERelation::Join).at(0)->Columns.size(), 4);
    }

    Y_UNIT_TEST(CteSelfJoinHasIndependentNullability) {
        TFixture f;
        f.Ok("WITH c AS (SELECT id FROM t) SELECT l.id, r.id FROM c AS l LEFT JOIN c AS r ON l.id = r.id;");
        f.Types({"Int64", "Int64?"});
        const auto uses = f.Find(ERelation::Reuse);
        UNIT_ASSERT_VALUES_EQUAL(uses.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(uses[0]->Inputs[0], uses[1]->Inputs[0]);
        for (const auto* use : uses) UNIT_ASSERT(!f.Program.Expressions[use->Definitions[0].Expression].Type.OptionalDepth);
    }

    Y_UNIT_TEST(ErrorsDoNotReturnPartialAnnotations) {
        for (const char* sql : {"SELECT id, id + 'bad' FROM t;", "SELECT id FROM t WHERE 1;",
            "SELECT l.id FROM t AS l JOIN t AS r ON l.id;", "SELECT id FROM t LIMIT 'bad';", "SELECT $missing;"}) {
            TFixture f; UNIT_ASSERT_EXCEPTION_C(f.Ok(sql), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(TemporalConstructorsUseSqlEncoding) {
        TFixture f;
        f.Ok("SELECT Date('1998-12-01') - Interval('P90D'), DateTime64('2024-01-01T00:00:00Z');");
        f.Types({"Date?", "Datetime64"});
    }

    Y_UNIT_TEST(CorrelatedSubqueriesHaveExplicitCaptures) {
        TFixture f;
        f.Ok("SELECT l.id, (SELECT MAX(r.id) FROM t AS r WHERE r.id = l.id) FROM t AS l "
            "WHERE EXISTS (SELECT r.id FROM t AS r WHERE r.id = l.id) AND l.id IN (SELECT id FROM t);");
        f.Types({"Int64", "Int64?"});
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Subplans.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Capture).size(), 2);
    }

    Y_UNIT_TEST(UngroupedColumnsAreRejected) {
        TFixture f; UNIT_ASSERT_EXCEPTION(f.Ok("SELECT id, SUM(score) FROM t;"), TFrontendError);
    }

    Y_UNIT_TEST(PreambleQueriesUseTypedLambdasAndOuterJoinSchemas) {
        TFixture f;
        f.Ok("$inc = ($x) -> ($x + 1); $q = SELECT $inc(id) AS k FROM t; "
            "$r = (WITH c AS (SELECT k FROM $q) SELECT * FROM c); "
            "SELECT l.k, r.k FROM $q AS l LEFT JOIN $r AS r ON l.k = r.k;");
        f.Types({"Int64", "Int64?"});
    }

    Y_UNIT_TEST(PreambleLambdaCapturesSdkParameter) {
        TFixture f; f.Settings.Binding.ParameterTypes.emplace("offset", TType{ETypeKind::Int64});
        f.Ok("$f = ($x) -> ($x + $offset); SELECT $f(id) FROM t;"); f.Types({"Int64"});
    }

    Y_UNIT_TEST(PreambleLambdaErrorsUseNativeDiagnostics) {
        for (const char* sql : {"$f = ($x) -> ($x + 1); SELECT $f();", "$f = ($x) -> ($x + 1); SELECT $f(1, 2);",
            "$f = ($x) -> ($x + 1); SELECT $f('text');", "$f = ($x) -> ($missing + $x); SELECT $f(1);"}) {
            TFixture f; UNIT_ASSERT_EXCEPTION_C(f.Ok(sql), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(CostBasedOptimizationLevelDoesNotChangeTypes) {
        TFixture f; f.Ok("PRAGMA ydb.CostBasedOptimizationLevel = '0'; SELECT id + 1, value FROM t;"); f.Types({"Int64", "String?"});
    }

    Y_UNIT_TEST(PreambleImplicitCrossJoinPragma) {
        TFixture f;
        f.Ok("PRAGMA AnsiImplicitCrossJoin; $q = (SELECT id FROM t); SELECT l.id, r.id FROM $q AS l, $q AS r;");
        f.Types({"Int64", "Int64"});
    }

    Y_UNIT_TEST(LazyInputsPreserveGrandparentCapturesAndNegatedIn) {
        TFixture f;
        f.Ok("SELECT l.id, (SELECT (SELECT MAX(z.id) FROM t AS z WHERE z.id = l.id) "
            "FROM t AS r LIMIT 1) FROM t AS l;");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Subplans.size(), 2);
        const auto captures = f.Find(ERelation::Capture);
        UNIT_ASSERT_VALUES_EQUAL(captures.size(), 2);
        for (const auto* capture : captures) UNIT_ASSERT_VALUES_EQUAL(capture->Captures.size(), 1);
        TFixture negated; negated.Ok("SELECT l.id NOT IN (SELECT id FROM t) FROM t AS l;");
        const auto expression = negated.Projection(0);
        UNIT_ASSERT_VALUES_EQUAL(negated.Inputs(expression).size(), 1);
        UNIT_ASSERT(negated.Program.Expressions[expression].Op == EOpcode::Not);
        UNIT_ASSERT_VALUES_EQUAL(negated.Program.Subplans.size(), 1);
    }

    Y_UNIT_TEST(StoredTypesKeepColumnNullabilityAndDecimalParameters) {
        TFixture f;
        f.Columns = {{"id", ParseType("Int64")}, {"optional_id", ParseType("Int64?")},
            {"amount", ParseType("Decimal(12,2)")}, {"optional_amount", ParseType("Decimal(12,2)?")},
            {"wide_amount", ParseType("Decimal(35,2)")}};
        f.Ok("SELECT * FROM t;");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns.size(), f.Columns.size());
        for (const auto& column : f.Program.Results[0].Columns) {
            const auto expected = std::find_if(f.Columns.begin(), f.Columns.end(), [&](const auto& input) { return input.Name == column.Name; });
            UNIT_ASSERT(expected != f.Columns.end()); UNIT_ASSERT(column.Type == expected->Type);
        }
    }

    Y_UNIT_TEST(LimitOffsetAndParameters) {
        TFixture f; f.Settings.Binding.ParameterTypes.emplace("p", ParseType("Int64"));
        f.Ok("SELECT id + $p FROM t LIMIT 10 OFFSET NULL;"); f.Types({"Int64"});
        const auto& limit = *f.Find(ERelation::Limit).at(0);
        UNIT_ASSERT(f.Program.Expressions[*limit.Count].Type == ParseType("Uint64"));
        UNIT_ASSERT(f.Program.Expressions[*limit.Offset].Type == ParseType("Uint64"));
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(f.Program.Expressions, *limit.Offset).Data), 0);
    }

    Y_UNIT_TEST(NullCountsUseClauseDefaults) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TFixture f; f.Settings.Parser = parser;
            f.Ok("SELECT id FROM t LIMIT NULL OFFSET NULL;");
            const auto& limit = *f.Find(ERelation::Limit).at(0);
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(f.Program.Expressions, *limit.Count).Data),
                std::numeric_limits<std::uint64_t>::max());
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(f.Program.Expressions, *limit.Offset).Data), 0);
            TFixture parameter; parameter.Settings.Parser = parser;
            parameter.Ok("DECLARE $n AS Uint64?; SELECT id FROM t LIMIT $n OFFSET $n;");
            const auto& dynamic = *parameter.Find(ERelation::Limit).at(0);
            for (const auto count : {dynamic.Count, dynamic.Offset}) {
                UNIT_ASSERT(parameter.Program.Expressions[*count].Type == ParseType("Uint64"));
                UNIT_ASSERT(parameter.Program.Expressions[*count].Op == EOpcode::Coalesce);
            }
        }
    }

    Y_UNIT_TEST(DiagnosticsKeepSqlPositions) {
        TFixture f;
        try { f.Ok("SELECT\n  id + 'bad' FROM t;"); UNIT_FAIL("invalid arithmetic accepted"); }
        catch (const TFrontendError& error) {
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 2);
            // Native scalar diagnostics identify the invalid operator itself.
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, 6);
            UNIT_ASSERT_STRING_CONTAINS(error.what(), "No common type");
        }
    }

    Y_UNIT_TEST(AggregationGroupingAndSetTypes) {
        TFixture f;
        f.Ok("SELECT value, SUM(id) AS s, COUNT(DISTINCT score), AVG(id) FROM t GROUP BY value HAVING s > 0 ORDER BY s;");
        f.Types({"String?", "Int64", "Uint64", "Double"});
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Aggregate).at(0)->Aggregates.size(), 3);
        TFixture scalar; scalar.Ok("SELECT SUM(id), COUNT(*) FROM t;"); scalar.Types({"Int64?", "Uint64"});
        // Default SQL UNION aligns names. Retain the reference SQL and check
        // its actual two-column contract, including NULL-filled missing names.
        TFixture set; set.Ok("SELECT id FROM t UNION ALL SELECT score FROM t;"); set.Types({"Int64?", "Int32?"});
    }

    Y_UNIT_TEST(GroupingSetsAndWindows) {
        TFixture f;
        f.Ok("SELECT id, GROUPING(id), SUM(SUM(score)) OVER (), RANK() OVER (ORDER BY SUM(score)) "
            "FROM t GROUP BY ROLLUP(id);");
        f.Types({"Int64?", "Uint64", "Int64?", "Uint64"});
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Aggregate).at(0)->GroupingSets.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Window).size(), 2);
    }

    Y_UNIT_TEST(RepeatedAggregatesShareResults) {
        TFixture f;
        f.Ok("SELECT COUNT(DISTINCT t.id), SUM(score), SUM(id) FROM t "
            "HAVING SUM(score) > 0 ORDER BY COUNT(DISTINCT id);");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Aggregate).at(0)->Aggregates.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Sort).at(0)->SortKeys.at(0).Column,
            f.Program.Results[0].Columns[0].Column);
    }

    Y_UNIT_TEST(RepeatedComputedGroupsAndAggregates) {
        TFixture f;
        f.Ok("PRAGMA YqlSelectAllowUnnamedGroupByExpr; "
            "SELECT t.id + 1, SUM(t.score + 1), SUM(score + 1), SUM(score + 2) FROM t "
            "GROUP BY id + 1 HAVING SUM(score + 1) > 0 ORDER BY id + 1, SUM(t.score + 1);");
        const auto& grouping = *f.Find(ERelation::Aggregate).at(0);
        UNIT_ASSERT_VALUES_EQUAL(grouping.GroupKeys.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(grouping.Aggregates.size(), 2);
        UNIT_ASSERT(f.Inputs(f.Projection(1)) == f.Inputs(f.Projection(2)));
        const auto& sort = *f.Find(ERelation::Sort).at(0);
        UNIT_ASSERT_VALUES_EQUAL(sort.SortKeys[0].Column, f.Program.Results[0].Columns[0].Column);
        UNIT_ASSERT_VALUES_EQUAL(sort.SortKeys[1].Column, f.Program.Results[0].Columns[1].Column);
    }

    Y_UNIT_TEST(ComputedAggregatesKeepCteSourceIdentity) {
        TFixture f;
        f.Ok("WITH c AS (SELECT id, score FROM t) "
            "SELECT SUM(l.score + 1), SUM(r.score + 1), SUM(l.score + 1) "
            "FROM c AS l JOIN c AS r ON l.id = r.id;");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Aggregate).at(0)->Aggregates.size(), 2);
        UNIT_ASSERT(f.Inputs(f.Projection(0)) == f.Inputs(f.Projection(2)));
        UNIT_ASSERT(f.Inputs(f.Projection(0)) != f.Inputs(f.Projection(1)));
    }

    Y_UNIT_TEST(HavingAliasesUseAggregateInputsBeforeProjection) {
        TFixture f;
        f.Ok("SELECT id + 1 AS id FROM t GROUP BY id HAVING id > 1;");
        const auto& filter = *f.Find(ERelation::Filter).at(0);
        const auto& group = *f.Find(ERelation::Aggregate).at(0);
        UNIT_ASSERT(f.Inputs(*filter.Predicate) == std::unordered_set<TColumnId>(group.GroupKeys.begin(), group.GroupKeys.end()));
        TFixture invalid;
        UNIT_ASSERT_EXCEPTION(invalid.Ok("SELECT COUNT(*) AS n, SUM(COUNT(*)) OVER () AS w FROM t HAVING w > 1;"), TFrontendError);
    }
}
