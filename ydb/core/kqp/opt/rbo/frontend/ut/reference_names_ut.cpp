// Name-resolution ports from PR #58587 at fbeb81657ef. SQL names bind to
// native column IDs; table discovery is checked independently of scalar typing.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>

using namespace NKikimr::NKqp::NFrontend;

namespace {
struct TFixture {
    TCompilerSettings Settings;
    std::unordered_map<std::string, TCatalogTable> Catalog;
    std::vector<std::string> Lookups;
    TBoundProgram Program;
    TFixture() {
        Settings.Binding.TablePathPrefix = "/Root";
        Catalog.emplace("/Root/t", TCatalogTable{"/Root/t", {{"id", ParseType("Int64")}, {"value", ParseType("String?")}}});
        Catalog.emplace("/Root/u", TCatalogTable{"/Root/u", {{"id", ParseType("Int64")}, {"other", ParseType("Int64?")}}});
    }
    void Ok(std::string_view sql) {
        auto result = CompileNative(sql, [&](std::string_view name, TSourcePosition pos) {
            Lookups.emplace_back(name);
            const auto found = Catalog.find(std::string(name));
            if (found == Catalog.end()) throw TFrontendError(pos, "unknown table: " + std::string(name));
            return found->second;
        }, Settings);
        Program = std::move(*result.Program).Release();
    }
    void Error(std::string_view sql) {
        try { Ok(sql); UNIT_FAIL("invalid name accepted: " << sql); }
        catch (const TUnsupportedFeature& error) { UNIT_FAIL("name error requested fallback: " << error.what()); }
        catch (const TFrontendError&) {}
    }
    std::vector<const TRelation*> Find(ERelation kind) const {
        std::vector<const TRelation*> result;
        for (const auto& relation : Program.Relations) if (relation.Kind == kind) result.push_back(&relation);
        return result;
    }
};
}

Y_UNIT_TEST_SUITE(KqpNativeReferenceNames) {
    Y_UNIT_TEST(UnsupportedBindingDoesNotReturnPartialNames) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TFixture f; f.Settings.Parser = parser;
            // USING now has native binding rules. Keep the reference SQL as
            // a positive check rather than retaining its old fallback.
            f.Ok("SELECT * FROM t JOIN u USING(id);");
            UNIT_ASSERT_VALUES_EQUAL(f.Program.Results.at(0).Columns.size(), 3);
            for (const auto sql : {"SELECT * FROM t WITH SCHEMA (id String);",
                    "PRAGMA AnsiQuotedIdentifiers; SELECT id FROM t;",
                    "PRAGMA AnsiImplicitCrossJoin = 'false'; SELECT id FROM t;"}) {
                TFixture unsupported; unsupported.Settings.Parser = parser;
                UNIT_ASSERT_EXCEPTION(unsupported.Ok(sql), TUnsupportedFeature);
                UNIT_ASSERT(unsupported.Program.Results.empty());
                unsupported.Settings.Mode = ECompilerMode::Auto;
                const auto result = CompileNative(sql, [&](std::string_view, TSourcePosition) {
                    return unsupported.Catalog.at("/Root/t");
                }, unsupported.Settings);
                UNIT_ASSERT(result.UsesLegacy());
                UNIT_ASSERT(result.Unsupported);
                UNIT_ASSERT(!result.Program);
            }
            // Member access on a String is a name/type error in the native
            // profile; it must not expose bindings from the preceding SELECT.
            TFixture invalid; invalid.Settings.Parser = parser;
            invalid.Error("SELECT id FROM t; SELECT value.field FROM t;");
            UNIT_ASSERT(invalid.Program.Results.empty());
        }
    }

    Y_UNIT_TEST(TableCollectionPreservesQuotedNamesAndUnsupportedDiagnostics) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TFixture f; f.Settings.Parser = parser;
            const auto parsed = ParseNative("--!ansi_lexer\nWITH \"c\" AS (SELECT * FROM \"t\") SELECT * FROM \"c\";", f.Settings);
            const auto tables = CollectTables(parsed, f.Settings.Binding);
            UNIT_ASSERT_VALUES_EQUAL(tables.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(tables.front().Name, "/Root/t");
            // Native admission may reject a dynamic pragma while parsing;
            // neither parsing nor discovery returns a partial table list.
            UNIT_ASSERT_EXCEPTION(CollectTables(ParseNative(
                "PRAGMA TablePathPrefix = $prefix; SELECT * FROM t;", f.Settings), f.Settings.Binding), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(AnsiQuotedAliasesAndExactCase) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TFixture f; f.Settings.Parser = parser;
            f.Catalog.emplace("/Root/Mixed", TCatalogTable{"/Root/Mixed", {{"Id", ParseType("Int64")}}});
            f.Ok("--!ansi_lexer\nSELECT \"a\"\"b\".Id FROM \"Mixed\" AS \"a\"\"b\";");
            const auto& output = f.Program.Results.at(0).Columns.at(0);
            UNIT_ASSERT_VALUES_EQUAL(f.Program.Columns.at(output.Column).Name, "Id");
            UNIT_ASSERT_VALUES_EQUAL(f.Lookups.front(), "/Root/Mixed");
            f.Error("SELECT id FROM Mixed;");
        }
    }

    Y_UNIT_TEST(CostBasedOptimizationLevelPragmas) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TFixture f; f.Settings.Parser = parser;
            f.Ok("PRAGMA ydb.Cost_Based_Optimization_Level('\\x30'); "
                "PRAGMA kikimr.CostBasedOptimizationLevel = '4'; SELECT id FROM t;");
            const auto& settings = f.Program.PlannerSettings;
            UNIT_ASSERT_VALUES_EQUAL(settings.size(), 2);
            for (const auto& setting : settings) UNIT_ASSERT(setting.Kind == EPlannerSetting::CostBasedOptimizationLevel);
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint32_t>(settings[0].Value), 0);
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint32_t>(settings[1].Value), 4);
            for (const auto sql : {"PRAGMA ydb.CostBasedOptimizationLevel = 0; SELECT id FROM t;",
                    "PRAGMA ydb.CostBasedOptimizationLevel('0', '4'); SELECT id FROM t;"}) f.Error(sql);
            for (const auto sql : {"$level = '0'; PRAGMA ydb.CostBasedOptimizationLevel = $level; SELECT id FROM t;",
                    "PRAGMA yt.CostBasedOptimizationLevel = '0'; SELECT id FROM t;"})
                UNIT_ASSERT_EXCEPTION(f.Ok(sql), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(PreambleCapturesAndLambdaShadowing) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TFixture f; f.Settings.Parser = parser;
            f.Ok("$n = 1; $f = ($x) -> { $n = $n + $x; return $n; }; "
                "$n = 100; SELECT $f(id), $n FROM t;");
            TEvaluationContext context;
            context.Column = [](TColumnId) { return TValue{{ETypeKind::Int64}, std::int64_t{7}}; };
            const std::string_view expected[]{"8", "100"};
            for (std::size_t i = 0; i < std::size(expected); ++i) {
                bool found = false;
                for (const auto& relation : f.Program.Relations) for (const auto& definition : relation.Definitions) {
                    if (definition.Column != f.Program.Results[0].Columns[i].Column) continue;
                    found = true;
                    UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(f.Program.Expressions, definition.Expression, context)), expected[i]);
                }
                UNIT_ASSERT(found);
            }
        }
    }

    Y_UNIT_TEST(CollectsPhysicalTablesBeforeColumnResolution) {
        constexpr std::string_view sql = "$q = (SELECT id FROM t); "
            "WITH c AS (SELECT id FROM u), d AS (SELECT id FROM c) "
            "SELECT q.id FROM $q AS q JOIN d ON q.id = d.id "
            "WHERE EXISTS (SELECT id FROM t WHERE id IN (SELECT id FROM u)); "
            "SELECT id FROM (SELECT id FROM t UNION ALL SELECT id FROM u) AS s;";
        TFixture f;
        const auto tables = CollectTables(ParseNative(sql, f.Settings), f.Settings.Binding);
        UNIT_ASSERT_VALUES_EQUAL(tables.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(tables[0].Name, "/Root/t");
        UNIT_ASSERT_VALUES_EQUAL(tables[1].Name, "/Root/u");
        f.Ok(sql);
        for (const auto& lookup : f.Lookups)
            UNIT_ASSERT(std::any_of(tables.begin(), tables.end(), [&](const auto& table) { return table.Name == lookup; }));
    }

    Y_UNIT_TEST(SetOrderingUsesCompoundOutputScope) {
        TFixture f;
        f.Ok("SELECT value AS z, id AS a FROM t UNION ALL SELECT value AS z, id AS a FROM t ORDER BY a, z;");
        const auto& outputs = f.Program.Results[0].Columns;
        UNIT_ASSERT_VALUES_EQUAL(outputs[0].Name, "z"); UNIT_ASSERT_VALUES_EQUAL(outputs[1].Name, "a");
        const auto& sort = *f.Find(ERelation::Sort).at(0);
        UNIT_ASSERT_VALUES_EQUAL(sort.SortKeys[0].Column, outputs[1].Column);
        UNIT_ASSERT_VALUES_EQUAL(sort.SortKeys[1].Column, outputs[0].Column);
        for (const std::string name : {"id", "t.id", "value"})
            f.Error("SELECT id AS n FROM t UNION ALL SELECT id AS n FROM t ORDER BY " + name + ";");
    }

    Y_UNIT_TEST(NativeResultColumnNames) {
        TFixture f;
        f.Ok("SELECT l.id, r.id, 1 AS column2, 2 FROM t AS l JOIN u AS r ON l.id = r.id;");
        const std::vector<std::string> names{"l.id", "r.id", "column2", "column3"};
        for (std::size_t i = 0; i < names.size(); ++i) UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns[i].Name, names[i]);
        f.Ok("SELECT 1, 2 AS column0, 3;");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns[0].Name, "column1");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns[2].Name, "column2");
        f.Error("SELECT id AS x, value AS x FROM t;");
    }

    Y_UNIT_TEST(BindsCatalogIdentityAndColumns) {
        TFixture f; f.Settings.Binding.ParameterTypes.emplace("min", ParseType("Int64"));
        f.Ok("SELECT t.id, value FROM t WHERE id > $min ORDER BY t.id;");
        UNIT_ASSERT_VALUES_EQUAL(f.Lookups.size(), 1); UNIT_ASSERT_VALUES_EQUAL(f.Lookups[0], "/Root/t");
        const auto& read = *f.Find(ERelation::Read).at(0);
        UNIT_ASSERT_VALUES_EQUAL(read.Table, "/Root/t");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns[0].Column, read.Columns[0]);
        UNIT_ASSERT(f.Program.Columns[read.Columns[0]].Type == ParseType("Int64"));
    }

    Y_UNIT_TEST(RejectsMissingAndAmbiguousNames) {
        for (const auto sql : {"SELECT id FROM absent;", "SELECT absent FROM t;", "SELECT x.absent FROM t AS x;",
            "SELECT id FROM t AS l JOIN u AS r ON l.id = r.id;", "SELECT l.id FROM t AS l JOIN u AS l ON l.id = l.id;",
            "SELECT t.id FROM t AS hidden;", "SELECT id FROM t WHERE absent > 0;", "SELECT COUNT(*) FROM t GROUP BY absent;",
            "SELECT id FROM t ORDER BY absent;", "SELECT missing.* FROM t;", "SELECT l.id FROM t AS l JOIN u AS r ON l.absent = r.id;"}) {
            TFixture f; f.Error(sql);
        }
    }

    Y_UNIT_TEST(StarUsesCatalogColumnOrder) {
        TFixture f; f.Ok("SELECT t.* FROM t;");
        const auto& read = *f.Find(ERelation::Read).at(0);
        UNIT_ASSERT_VALUES_EQUAL(read.Columns.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Columns[read.Columns[0]].Name, "id");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Columns[read.Columns[1]].Name, "value");
    }

    Y_UNIT_TEST(DerivedTablesAndProjectionAliases) {
        TFixture f; f.Ok("SELECT d.key FROM (SELECT id AS key FROM t) AS d ORDER BY key;");
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns[0].Name, "key");
        UNIT_ASSERT(f.Program.Results[0].Columns[0].Type == ParseType("Int64"));
        const auto& read = *f.Find(ERelation::Read).at(0);
        // A derived alias preserves the existing value ID without requiring
        // an otherwise redundant expression or operator.
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results[0].Columns[0].Column, read.Columns[0]);
        f.Error("SELECT d.value FROM (SELECT id AS key FROM t) AS d;");
    }

    Y_UNIT_TEST(OrderByAliasesDoNotLeakIntoWhere) {
        TFixture f; f.Ok("SELECT id + 1 AS value FROM t ORDER BY value;");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(ERelation::Sort).at(0)->SortKeys[0].Column, f.Program.Results[0].Columns[0].Column);
        f.Error("SELECT id AS new_name FROM t WHERE new_name > 0;");
    }

    Y_UNIT_TEST(CteShadowsCatalogAndCopiesBindings) {
        TFixture f;
        f.Ok("WITH t(key) AS (SELECT id FROM u) SELECT l.key, r.key AS other FROM t AS l JOIN t AS r ON l.key = r.key;");
        UNIT_ASSERT_VALUES_EQUAL(f.Lookups.size(), 1); UNIT_ASSERT_VALUES_EQUAL(f.Lookups[0], "/Root/u");
        UNIT_ASSERT(f.Program.Results[0].Columns[0].Column != f.Program.Results[0].Columns[1].Column);
        const auto uses = f.Find(ERelation::Reuse);
        UNIT_ASSERT_VALUES_EQUAL(uses.size(), 2); UNIT_ASSERT_VALUES_EQUAL(uses[0]->Inputs[0], uses[1]->Inputs[0]);
        f.Error("WITH c(a,b) AS (SELECT id FROM t) SELECT a FROM c;");
    }

    Y_UNIT_TEST(CorrelatedSubqueryAndLocalShadowing) {
        TFixture f;
        f.Ok("SELECT l.id FROM t AS l WHERE EXISTS (SELECT r.id FROM u AS r WHERE r.id = l.id);");
        const auto& capture = *f.Find(ERelation::Capture).at(0);
        UNIT_ASSERT_VALUES_EQUAL(capture.Captures.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(capture.Captures[0].Outer, f.Program.Results[0].Columns[0].Column);
        f.Error("SELECT l.value FROM t AS l WHERE EXISTS (SELECT l.value FROM u AS l);");
    }

    Y_UNIT_TEST(SemiJoinExposesOnlyPreservedSide) {
        TFixture f; f.Ok("SELECT l.id FROM t AS l LEFT SEMI JOIN u AS r ON l.id = r.id;");
        const auto& join = *f.Find(ERelation::Join).at(0);
        UNIT_ASSERT_VALUES_EQUAL(join.Columns.size(), 2);
        f.Error("SELECT r.id FROM t AS l LEFT SEMI JOIN u AS r ON l.id = r.id;");
    }

    Y_UNIT_TEST(PrefixPragmasAreAppliedInStatementOrder) {
        TFixture f;
        f.Catalog.emplace("/Other/t", TCatalogTable{"/Other/t", {{"different", ParseType("String")}}});
        f.Ok("SELECT id FROM t; PRAGMA TablePathPrefix = '/Other'; SELECT different FROM t; SELECT id FROM `/Root/t`;");
        UNIT_ASSERT_VALUES_EQUAL(f.Lookups.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(f.Lookups[0], "/Root/t"); UNIT_ASSERT_VALUES_EQUAL(f.Lookups[1], "/Other/t");
        const auto reads = f.Find(ERelation::Read);
        UNIT_ASSERT_VALUES_EQUAL(reads.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(f.Program.Results.size(), 3);
        for (std::size_t i = 0; i < reads.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(reads[i]->Table, i == 1 ? "/Other/t" : "/Root/t");
            const auto& result = f.Program.Results[i].Columns.at(0);
            UNIT_ASSERT_VALUES_EQUAL(result.Column, reads[i]->Columns.at(0));
            UNIT_ASSERT_VALUES_EQUAL(result.Name, i == 1 ? "different" : "id");
            UNIT_ASSERT(result.Type == ParseType(i == 1 ? "String" : "Int64"));
        }
        UNIT_ASSERT(reads[0]->Columns[0] != reads[2]->Columns[0]);
    }

    Y_UNIT_TEST(ErrorsHaveSourcePositions) {
        TFixture f;
        try { f.Ok("SELECT\n  missing FROM t;"); UNIT_FAIL("unknown column accepted"); }
        catch (const TFrontendError& error) {
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 2); UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, 3);
        }
    }

    Y_UNIT_TEST(NamedQueriesBindOnceAndExposeIndependentColumns) {
        TFixture f;
        f.Ok("$q = (SELECT id FROM t); $alias = $q; SELECT l.id, r.id FROM $q AS l JOIN $alias AS r ON l.id = r.id;");
        UNIT_ASSERT_VALUES_EQUAL(f.Lookups.size(), 1);
        const auto uses = f.Find(ERelation::Reuse);
        UNIT_ASSERT_VALUES_EQUAL(uses.size(), 2); UNIT_ASSERT_VALUES_EQUAL(uses[0]->Inputs[0], uses[1]->Inputs[0]);
        UNIT_ASSERT(f.Program.Results[0].Columns[0].Column != f.Program.Results[0].Columns[1].Column);
    }

    Y_UNIT_TEST(PreambleBindingErrorsHaveNoPartialResult) {
        for (const auto sql : {"$f = ($x, $x) -> ($x); SELECT $f(1, 2);",
            "$q = (SELECT id FROM $later); $later = (SELECT id FROM t); SELECT * FROM $q;"}) {
            TFixture f; f.Error(sql);
        }
    }
}
