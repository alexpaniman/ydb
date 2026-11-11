#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <library/cpp/testing/unittest/registar.h>

#include <unordered_set>

using namespace NKikimr::NKqp::NFrontend;
using NKikimr::NKqp::NFrontend::NDetail::Compile;

namespace {
TCatalogTable Catalog(std::string_view name, TSourcePosition pos) {
    if (name != "a" && name != "b" && name != "/Root/a") throw TFrontendError(pos, "unknown table");
    return {std::string(name), {{"id", {ETypeKind::Uint64}}, {"value", {ETypeKind::Int32, true}}, {"name", {ETypeKind::String}}}};
}

const TRelation& Find(const TBoundProgram& program, ERelation kind) {
    for (const auto& relation : program.Relations) if (relation.Kind == kind) return relation;
    UNIT_FAIL("operator not found");
    throw std::runtime_error("operator not found");
}
}

Y_UNIT_TEST_SUITE(KqpNativeBinder) {
    Y_UNIT_TEST(FilterOptionalityMatchesTheRboInputContract) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("SELECT id FROM a WHERE Just(id > 0ul)", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto root = *Find(program, ERelation::Filter).Predicate;
            UNIT_ASSERT(program.Expressions[root].Op == EOpcode::Greater);
            for (const auto* sql : {"SELECT id FROM a WHERE Just(value > 0)",
                    "SELECT id FROM a WHERE Just(Just(id > 0ul))"})
                UNIT_ASSERT_EXCEPTION_CONTAINS(CompileNative(sql, Catalog, settings), TFrontendError, "Bool or Bool?");
        }
    }

    Y_UNIT_TEST(QualifiedAndDottedNamesRetainIdentityAndAmbiguity) {
        const auto catalog = [](std::string_view name, TSourcePosition) {
            return TCatalogTable{std::string(name), {{"k", {ETypeKind::Uint64}}, {"x.k", {ETypeKind::Uint64}}}};
        };
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "SELECT k AS plain, x.`x.k` AS dotted FROM t AS x",
            "SELECT x.`x.k` AS a, y.k AS b FROM t AS x JOIN t AS y ON x.`x.k` = y.`x.k`",
        }) {
            const auto compiled = CompileNative(sql, catalog, settings);
            const auto& columns = compiled.Program->Get().Results[0].Columns;
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 2);
            UNIT_ASSERT(columns[0].Column != columns[1].Column);
        }
        for (const auto* sql : {
            "SELECT x.k FROM t AS x",
            "SELECT x.k FROM t AS x JOIN t AS y ON x.`x.k` = y.`x.k`",
        }) UNIT_ASSERT_EXCEPTION_CONTAINS(CompileNative(sql, catalog, settings), TFrontendError, "ambiguous column: x.k");
    }

    Y_UNIT_TEST(CatalogColumnUniquenessIsCheckedForReadsAndWrites) {
        const auto invalid = [](std::string_view name, TSourcePosition) {
            TCatalogTable table{std::string(name), {{"id", {ETypeKind::Uint64}}, {"id", {ETypeKind::String}}}};
            table.WriteSchema = TWriteSchema{{"id"}, {}};
            return table;
        };
        for (const auto* sql : {"SELECT id FROM a", "UPSERT INTO a(id) VALUES (1ul)"}) {
            UNIT_ASSERT_EXCEPTION_CONTAINS(CompileNative(sql, invalid), TFrontendError, "duplicate catalog column: id");
        }
    }

    Y_UNIT_TEST(NamedInQueriesReuseTheSourceProducer) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(
                "$q = SELECT value FROM a; SELECT value IN $q AS found FROM $q", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto saved = CompileNative(
                "$q = SELECT id FROM a; $saved = $q; $q = SELECT id FROM b; SELECT 1ul IN $saved", Catalog, settings);
            UNIT_ASSERT_VALUES_EQUAL(Find(saved.Program->Get(), ERelation::Read).Table, "a");
            UNIT_ASSERT_VALUES_EQUAL(program.Subplans.size(), 1);
            UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Bool, true}));
            std::vector<TRelId> producers;
            size_t reads = 0;
            for (const auto& relation : program.Relations) {
                reads += relation.Kind == ERelation::Read;
                if (relation.Kind == ERelation::Reuse) producers.push_back(relation.Inputs.at(0));
            }
            UNIT_ASSERT_VALUES_EQUAL(reads, 1);
            UNIT_ASSERT_VALUES_EQUAL(producers.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(producers[0], producers[1]);
        }
    }

    Y_UNIT_TEST(NamedInDiscoveryFollowsDeclarationScopeAndScalarClosures) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                "$q = SELECT id FROM a; $saved = $q; $q = SELECT id FROM b; SELECT 1ul IN $saved",
                "$q = SELECT id FROM a; $found = 1ul IN $q; $q = SELECT id FROM b; SELECT $found",
                "$q = SELECT id FROM a; $f = ($v)->{ RETURN $v IN $q; }; $q = SELECT id FROM b; SELECT $f(1ul)",
            }) {
                const auto parsed = ParseNative(sql, settings);
                const auto tables = CollectTables(parsed);
                UNIT_ASSERT_VALUES_EQUAL_C(tables.size(), 1, sql);
                UNIT_ASSERT_VALUES_EQUAL_C(tables[0].Name, "a", sql);

            }
        }
    }

    Y_UNIT_TEST(NamedInQueriesValidateShapeAndDoNotCaptureConsumerColumns) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "$q = SELECT id, value FROM a; SELECT 1ul IN $q",
            "$q = SELECT id; SELECT id IN $q FROM a",
            "SELECT 1ul IN $missing",
        }) UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
        UNIT_ASSERT_EXCEPTION(CompileNative(
            "$q = SELECT id FROM a; SELECT a.id FROM a JOIN b ON a.id IN $q", Catalog, settings), TUnsupportedFeature);
    }

    Y_UNIT_TEST(NamedSourcesDoNotAcquireConsumerColumnCaptures) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "$q = SELECT id; SELECT (SELECT id FROM $q) FROM a",
            "WITH c AS (SELECT id) SELECT (SELECT id FROM c) FROM a",
        }) UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
        const auto valid = CompileNative("SELECT (WITH c AS (SELECT a.id AS x) SELECT x FROM c) FROM a", Catalog, settings);
        UNIT_ASSERT_VALUES_EQUAL(valid.Program->Get().Subplans.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(Find(valid.Program->Get(), ERelation::Capture).Captures.size(), 1);
    }

    Y_UNIT_TEST(TableDiscoveryAndBindingUseDeclarationScope) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
            {"$q = SELECT id FROM c; WITH c AS (SELECT id FROM t) SELECT id FROM $q", {"c"}},
            {"PRAGMA TablePathPrefix='/first'; $q = SELECT id FROM t; "
             "PRAGMA TablePathPrefix='/second'; SELECT id FROM $q", {"/first/t"}},
            {"WITH x AS (SELECT id FROM c), c AS (SELECT id FROM t) SELECT id FROM x", {"c"}},
            {"WITH c AS (SELECT id FROM t), x AS (SELECT id FROM c) "
             "SELECT id FROM (WITH c AS (SELECT id FROM other) SELECT id FROM x) AS d", {"t"}},
            {"WITH c AS (SELECT id FROM t) "
             "SELECT id FROM (WITH c AS (SELECT id FROM other) SELECT id FROM c) AS d", {"other"}},
            {"PRAGMA TablePathPrefix='/first'; $q = SELECT id FROM t; $saved = $q; "
             "PRAGMA TablePathPrefix='/second'; $q = SELECT id FROM t; "
             "SELECT l.id FROM $saved AS l JOIN $q AS r ON l.id = r.id", {"/first/t", "/second/t"}},
            {"PRAGMA TablePathPrefix='/first'; $q = SELECT id FROM t; "
             "PRAGMA TablePathPrefix='/second'; INSERT INTO dest SELECT id FROM $q", {"/second/dest", "/first/t"}},
        };
        for (const auto& [sql, expected] : cases) {
            const auto parsed = ParseNative(sql, settings);
            std::vector<std::string> discovered, resolved;
            for (const auto& request : CollectTables(parsed)) discovered.push_back(request.Name);
            UNIT_ASSERT_C(discovered == expected, sql);
            const auto program = BindNative(parsed, [&](std::string_view name, TSourcePosition) {
                resolved.emplace_back(name);
                return TCatalogTable{std::string(name), {{"id", {ETypeKind::Uint64}}}, TWriteSchema{{"id"}, {}, false}};
            });
            UNIT_ASSERT_C(resolved == expected, sql);
            UNIT_ASSERT_VALUES_EQUAL(program.Get().Results.size(), 1);
        }
    }

    Y_UNIT_TEST(RepeatedSourcesShareCatalogSnapshotButKeepIndependentBindings) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        const auto parsed = ParseNative(
            "SELECT l.id AS a, r.id AS b FROM t AS l JOIN t AS r ON l.id = r.id; "
            "INSERT INTO t SELECT id FROM t", settings);
        std::size_t lookups = 0;
        const auto compiled = BindNative(parsed, [&](std::string_view name, TSourcePosition) {
            ++lookups;
            return TCatalogTable{std::string(name), {{"id", {ETypeKind::Uint64}}}, TWriteSchema{{"id"}, {}, false}};
        });
        UNIT_ASSERT_VALUES_EQUAL(lookups, 1);
        std::unordered_set<TColumnId> inputs;
        std::size_t reads = 0;
        for (const auto& relation : compiled.Get().Relations) if (relation.Kind == ERelation::Read) {
            ++reads;
            UNIT_ASSERT_VALUES_EQUAL(relation.Columns.size(), 1);
            UNIT_ASSERT(inputs.insert(relation.Columns.front()).second);
        }
        UNIT_ASSERT_VALUES_EQUAL(reads, 3);
        UNIT_ASSERT_VALUES_EQUAL(compiled.Get().Results.size(), 2);
    }

    Y_UNIT_TEST(ListSubscriptsBindBeforeUnaryAndBinaryOperators) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "SELECT [1,2][0u] AS v, CAST(3 AS Int64) + 2 AS n LIMIT 1",
            "SELECT -[1,2][1u] + [3,4][0u]",
            "DECLARE $items AS List<Int32?>; DECLARE $index AS Int64?; SELECT $items[$index]",
            "SELECT [1, IF(false, 2)][0u], [1, IF(false, 2)][1u], [1, IF(false, 2)][2u]",
        }) UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).Program, sql);
    }

    Y_UNIT_TEST(ColumnAliasListsPreserveCatalogNamesAndIndependentBindings) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "WITH c(k) AS (SELECT id + 1 FROM a) SELECT l.k AS x, r.k AS y FROM c AS l JOIN c AS r ON l.k = r.k",
            "SELECT d.key FROM (SELECT key FROM a AS x(key, label, count)) AS d",
            "WITH c(k) AS (SELECT id FROM a) SELECT x.renamed FROM c AS x(renamed)",
            "SELECT x.k FROM (SELECT id FROM a) AS x(k,)",
            "DECLARE $rows AS List<Struct<id:Uint64>>; SELECT x.k FROM AS_TABLE($rows) AS x(k)",
            "WITH c(k) AS (SELECT id FROM a) SELECT d.value FROM (WITH c(value) AS (SELECT name FROM b) SELECT value FROM c) AS d",
        }) {
            const auto compiled = CompileNative(sql, Catalog, settings);
            const auto& program = compiled.Program->Get();
            for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Read) {
                UNIT_ASSERT_VALUES_EQUAL(program.Columns[relation.Columns[0]].Name, "id");
                UNIT_ASSERT_VALUES_EQUAL(program.Columns[relation.Columns[1]].Name, "value");
                UNIT_ASSERT_VALUES_EQUAL(program.Columns[relation.Columns[2]].Name, "name");
            }
        }
        for (const auto* sql : {
            "SELECT * FROM a AS x(k)",
            "SELECT * FROM a AS x(k,k,n)",
            "WITH c(k,n) AS (SELECT id FROM a) SELECT * FROM c",
            "WITH c(k,k) AS (SELECT id,name FROM a) SELECT * FROM c",
        }) UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
    }

    Y_UNIT_TEST(NamedQueryAliasesRetainProducerIdentityAndDefinitionScope) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                $offset = 1; $inc = ($x) -> ($x + $offset);
                $q = SELECT $inc(id) AS k FROM a; $alias = $q; $saved = $alias;
                $q = SELECT id FROM b; $offset = 100;
                SELECT l.k, r.k FROM $saved AS l JOIN $alias AS r ON l.k = r.k
            )", Catalog, settings);
            const auto& program = compiled.Program->Get();
            std::vector<TRelId> producers;
            unsigned reads = 0;
            for (const auto& relation : program.Relations) {
                if (relation.Kind == ERelation::Read) {
                    ++reads; UNIT_ASSERT_VALUES_EQUAL(relation.Table, "a");
                }
                if (relation.Kind == ERelation::Reuse) producers.push_back(relation.Inputs.at(0));
            }
            UNIT_ASSERT_VALUES_EQUAL(reads, 1);
            UNIT_ASSERT_VALUES_EQUAL(producers.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(producers[0], producers[1]);
        }
    }

    Y_UNIT_TEST(StarPresentationDoesNotChangeBoundSourceOrder) {
        const auto compiled = CompileNative(R"(
            SELECT * FROM a;
            PRAGMA OrderedColumns;
            SELECT * FROM a;
            PRAGMA DisableOrderedColumns;
            SELECT * FROM a;
            SELECT value, id, name FROM a;
        )", Catalog);
        const auto& program = compiled.Program->Get();
        const std::vector<std::vector<std::string>> expected{{"id", "value", "name"}, {"id", "value", "name"},
            {"id", "name", "value"}, {"value", "id", "name"}};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            std::vector<std::string> names;
            for (const auto& column : program.Results[i].Columns) names.push_back(column.Name);
            UNIT_ASSERT(names == expected[i]);
        }
        for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Read) {
            UNIT_ASSERT_VALUES_EQUAL(program.Columns[relation.Columns[0]].Name, "id");
            UNIT_ASSERT_VALUES_EQUAL(program.Columns[relation.Columns[1]].Name, "value");
            UNIT_ASSERT_VALUES_EQUAL(program.Columns[relation.Columns[2]].Name, "name");
        }
    }

    Y_UNIT_TEST(ExclusionJoinMakesBothOutputSidesNullable) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        const auto compiled = CompileNative("SELECT l.id AS a, r.id AS b FROM a AS l EXCLUSION JOIN b AS r ON l.id = r.id", Catalog, settings);
        const auto& program = compiled.Program->Get();
        UNIT_ASSERT(Find(program, ERelation::Join).JoinKind == EJoinKind::Exclusion);
        for (const auto& column : program.Results[0].Columns) UNIT_ASSERT(column.Type == (TType{ETypeKind::Uint64, true}));
    }

    Y_UNIT_TEST(CapturesShareOneInputAcrossQueryClauses) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                "SELECT (SELECT MAX(b.value - a.value) FROM b WHERE b.id <= a.id) FROM a",
                "SELECT (SELECT b.value + a.value FROM b WHERE b.id = a.id) FROM a"}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                std::size_t captures = 0;
                for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Capture) {
                    ++captures;
                    UNIT_ASSERT_VALUES_EQUAL_C(relation.Captures.size(), 2, sql);
                }
                UNIT_ASSERT_VALUES_EQUAL_C(captures, 1, sql);
            }
            const auto compiled = CompileNative("SELECT id FROM a LIMIT 1 + 2 OFFSET 3 - 1", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& limit = Find(program, ERelation::Limit);
            UNIT_ASSERT(program.Expressions[*limit.Count].Type == (TType{ETypeKind::Uint64}));
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(program.Expressions, *limit.Count).Data), 3);
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(program.Expressions, *limit.Offset).Data), 2);
        }
    }

    Y_UNIT_TEST(DynamicCountsUseOrdinaryUint64Conversions) {
        for (const auto* type : {"Int8", "Int16", "Int32", "Uint8", "Uint16", "Uint32", "Uint64", "Int32?"}) {
            const auto compiled = CompileNative(std::string("DECLARE $n AS ") + type + "; SELECT 1 LIMIT $n OFFSET $n", Catalog);
            const auto& program = compiled.Program->Get();
            const auto& limit = Find(program, ERelation::Limit);
            UNIT_ASSERT(program.Expressions[*limit.Count].Type == (TType{ETypeKind::Uint64}));
            UNIT_ASSERT(program.Expressions[*limit.Offset].Type == (TType{ETypeKind::Uint64}));
        }
        UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $n AS Int64; SELECT 1 LIMIT $n", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(NamedWindowsResolveWithinTheirQueryScope) {
        TCompilerSettings settings;
        settings.Parser = EParser::Grammar;
        const auto compiled = CompileNative(R"(
            SELECT SUM(value) OVER w AS total, COUNT(*) OVER w AS n
            FROM a WINDOW w AS (PARTITION BY name ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
        )", Catalog, settings);
        const auto& window = Find(compiled.Program->Get(), ERelation::Window);
        UNIT_ASSERT_VALUES_EQUAL(window.WindowFunctions.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(window.GroupKeys.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(window.WindowFrame.BeginOffset, 1);
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT ROWNUMBER() OVER missing FROM a", Catalog, settings), TFrontendError);
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT ROWNUMBER() OVER w FROM a WINDOW w AS (), w AS ()", Catalog, settings), TFrontendError);
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT ROWNUMBER() OVER w FROM b) FROM a WINDOW w AS ()", Catalog, settings), TFrontendError);
        const auto nested = CompileNative(R"(
            SELECT ROWNUMBER() OVER w FROM
                (SELECT id, ROWNUMBER() OVER w AS r FROM b WINDOW w AS (ORDER BY value)) AS q
            WINDOW w AS (ORDER BY id)
        )", Catalog, settings);
        UNIT_ASSERT(!nested.UsesLegacy());
    }

    Y_UNIT_TEST(SortNullOrderingUsesTheBoundScope) {
        struct TCase { const char* Sql; ERelation Kind; std::vector<bool> First; };
        const TCase cases[]{
            {"SELECT id FROM a ORDER BY id DESC", ERelation::Sort, {true}},
            {"SELECT value FROM a ORDER BY value DESC", ERelation::Sort, {false}},
            {"SELECT b.id AS nullable_id, a.id AS required_id FROM a LEFT JOIN b ON a.id = b.id ORDER BY nullable_id DESC, required_id DESC", ERelation::Sort, {false, true}},
            {"SELECT a.id FROM a LEFT JOIN b ON a.id = b.id ORDER BY b.id DESC, a.id DESC", ERelation::Sort, {false, true}},
            {"SELECT ROWNUMBER() OVER(ORDER BY b.id DESC, a.id DESC) AS r FROM a LEFT JOIN b ON a.id = b.id", ERelation::Window, {false, true}},
            {"SELECT id AS x FROM a UNION ALL SELECT CAST(value AS Uint64) AS x FROM b ORDER BY x DESC", ERelation::Sort, {false}},
        };
        for (const auto parser : {EParser::Fast, EParser::Grammar}) for (const auto& test : cases) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(test.Sql, Catalog, settings);
            const auto& sort = Find(compiled.Program->Get(), test.Kind);
            UNIT_ASSERT_VALUES_EQUAL_C(sort.SortKeys.size(), test.First.size(), test.Sql);
            for (std::size_t i = 0; i < test.First.size(); ++i) {
                UNIT_ASSERT(sort.SortKeys[i].Descending);
                UNIT_ASSERT_VALUES_EQUAL_C(sort.SortKeys[i].NullsFirst, bool(test.First[i]), test.Sql);
            }
        }
    }

    Y_UNIT_TEST(InSubplansMaterializeTypedLookupInputs) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto compiled = CompileNative("SELECT value + 1 IN (SELECT CAST(b.value AS Int64) FROM b WHERE b.id = a.id) AS yes FROM a", Catalog, settings);
            const auto& program = compiled.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(program.Subplans.size(), 1);
            const auto& [binding, subplan] = *program.Subplans.begin();
            UNIT_ASSERT(subplan.Kind == ESubquery::In && subplan.Lookup);
            UNIT_ASSERT(program.Columns[binding].Type == (TType{ETypeKind::Bool, true}));
            UNIT_ASSERT(program.Columns[*subplan.Lookup].Type == (TType{ETypeKind::Int64, true}));
            UNIT_ASSERT(program.Columns[subplan.Result].Type == (TType{ETypeKind::Int64, true}));
            bool materialized = false;
            for (const auto& relation : program.Relations) for (const auto& definition : relation.Definitions)
                materialized |= definition.Column == *subplan.Lookup;
            UNIT_ASSERT(materialized);
            UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::Capture).Captures.size(), 1);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT id IN (SELECT id, value FROM b) FROM a", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT id IN (SELECT missing FROM b) FROM a", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT a.id FROM a JOIN b ON a.id IN (SELECT id FROM a)", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT(!CompileNative("SELECT NULL IN (SELECT NULL), (value IN (SELECT value FROM b)) IN (SELECT true) FROM a", Catalog, settings).UsesLegacy());
            for (const bool ansi : {false, true}) {
                settings.Binding.AnsiIn = ansi;
                const auto value = CompileNative("SELECT id IN (SELECT CAST(value AS Uint64) FROM b) AS v FROM a", Catalog, settings);
                UNIT_ASSERT(value.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Bool, ansi}));
                const auto overridden = CompileNative("PRAGMA AnsiInForEmptyOrNullableItemsCollections; SELECT id IN (SELECT CAST(value AS Uint64) FROM b) AS v FROM a", Catalog, settings);
                UNIT_ASSERT(overridden.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Bool, true}));
            }
        }
    }

    Y_UNIT_TEST(ExistsSubplansAreRequiredBooleanRelationalBindings) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto compiled = CompileNative("SELECT EXISTS(SELECT b.id, b.value FROM b WHERE b.value = a.value) AS yes, NOT EXISTS(SELECT 1 FROM b WHERE false) AS no FROM a", Catalog, settings);
            const auto& program = compiled.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(program.Subplans.size(), 2);
            for (const auto& [binding, subplan] : program.Subplans) {
                UNIT_ASSERT(subplan.Kind == ESubquery::Exists);
                UNIT_ASSERT(program.Columns[binding].Type == (TType{ETypeKind::Bool}));
            }
            for (const auto& column : program.Results[0].Columns) UNIT_ASSERT(column.Type == (TType{ETypeKind::Bool}));
            UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::Capture).Captures.size(), 1);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT EXISTS(SELECT missing FROM a)", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT a.id FROM a JOIN b ON EXISTS(SELECT 1)", Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(ScalarSubplansAreTypedRelationalBindings) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto compiled = CompileNative("SELECT (SELECT MAX(value) FROM a) AS x, (SELECT MIN(value) FROM b) AS y, (SELECT (SELECT 1) + 2) AS z", Catalog, settings);
            const auto& program = compiled.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(program.Subplans.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns.size(), 3);
            for (const auto& column : program.Results[0].Columns) {
                UNIT_ASSERT(column.Type == (TType{ETypeKind::Int32, true}));
                UNIT_ASSERT(!program.Subplans.contains(column.Column));
            }
            const auto tables = CollectTables(ParseNative("SELECT (SELECT MAX(value) FROM a), (SELECT MIN(value) FROM b)", settings));
            UNIT_ASSERT_VALUES_EQUAL(tables.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(tables[0].Name, "a");
            UNIT_ASSERT_VALUES_EQUAL(tables[1].Name, "b");
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT id, value FROM a)", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT missing FROM a)", Catalog, settings), TFrontendError);
            settings.Mode = ECompilerMode::Auto;
            const auto correlated = CompileNative("SELECT (SELECT b.value FROM b WHERE b.id = a.id) FROM a", Catalog, settings);
            UNIT_ASSERT(!correlated.UsesLegacy());
            UNIT_ASSERT_VALUES_EQUAL(Find(correlated.Program->Get(), ERelation::Capture).Captures.size(), 1);
            const auto sort = CompileNative("SELECT id FROM a ORDER BY (SELECT MAX(value) FROM b)", Catalog, settings);
            UNIT_ASSERT(sort.UsesLegacy() && sort.Unsupported->Feature == EFeature::Subquery);
            const auto group = CompileNative("SELECT k, COUNT(*) FROM a GROUP BY (SELECT MAX(value) FROM b) AS k", Catalog, settings);
            UNIT_ASSERT(group.UsesLegacy() && group.Unsupported->Feature == EFeature::Subquery);
            UNIT_ASSERT(!CompileNative("SELECT (SELECT MAX(value) FROM b) AS x FROM a ORDER BY x", Catalog, settings).UsesLegacy());
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT missing FROM a)", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(CapturesForwardThroughNestedScopesAndRespectGrouping) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto compiled = CompileNative("SELECT (SELECT (SELECT a.id + a.id)) FROM a", Catalog, settings);
            const auto& program = compiled.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(program.Subplans.size(), 2);
            std::unordered_map<TColumnId, TColumnId> captures;
            for (const auto& relation : program.Relations) for (const auto& capture : relation.Captures) {
                UNIT_ASSERT(captures.emplace(capture.Local, capture.Outer).second);
                UNIT_ASSERT(capture.Outer < capture.Local);
                UNIT_ASSERT(program.Columns[capture.Local].Type == program.Columns[capture.Outer].Type);
                UNIT_ASSERT(std::find(relation.Columns.begin(), relation.Columns.end(), capture.Local) != relation.Columns.end());
            }
            UNIT_ASSERT_VALUES_EQUAL(captures.size(), 2);
            size_t forwarded = 0;
            for (const auto& [local, outer] : captures) forwarded += captures.contains(outer);
            UNIT_ASSERT_VALUES_EQUAL(forwarded, 1);
            for (const auto* sql : {
                "SELECT (SELECT SUM(b.value) FROM b WHERE b.id > a.id) FROM a",
                "SELECT (SELECT SUM(a.value) FROM b) FROM a",
                "SELECT (SELECT a.id) FROM a GROUP BY a.id"}) {
                UNIT_ASSERT(!CompileNative(sql, Catalog, settings).UsesLegacy());
            }
            const auto shadowed = CompileNative("SELECT (SELECT id FROM b WHERE id = 1ul) FROM a", Catalog, settings);
            for (const auto& relation : shadowed.Program->Get().Relations) UNIT_ASSERT(relation.Captures.empty());
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT a.value) FROM a GROUP BY a.id", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT SUM(b.value) + a.value FROM b) FROM a", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT MAX(b.value) FROM b HAVING MAX(b.value) > a.value) FROM a", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT a.name FROM (SELECT 1 AS id) AS a) FROM a", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT (SELECT missing FROM b) FROM a", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(DirectReadFilterProjection) {
        const auto program = Compile("SELECT id, value + 1 AS next FROM a WHERE value > 0 ORDER BY next DESC LIMIT 10", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(program.Results.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns[1].Name, "next");
        UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int32, true}));
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::Read).Table, "a");
        UNIT_ASSERT(Find(program, ERelation::Sort).SortKeys[0].Descending);
        UNIT_ASSERT(Find(program, ERelation::Filter).Predicate.has_value());
        for (std::size_t i = 0; i < program.Expressions.Size(); ++i) {
            for (auto child : program.Expressions[static_cast<TExprId>(i)].Arguments) UNIT_ASSERT(child < i);
        }
    }

    Y_UNIT_TEST(AggregatesAreRelationalAndTypedOnce) {
        const auto program = Compile("SELECT name, SUM(value) + 1 AS total, COUNT(*) AS n FROM a GROUP BY name HAVING SUM(value) > 3 ORDER BY total", Catalog);
        const auto& aggregate = Find(program, ERelation::Aggregate);
        UNIT_ASSERT_VALUES_EQUAL(aggregate.GroupKeys.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(aggregate.Aggregates.size(), 2);
        UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int64, true}));
        UNIT_ASSERT(program.Results[0].Columns[2].Type == (TType{ETypeKind::Uint64}));
    }

    Y_UNIT_TEST(GroupingExpressionsAndAliases) {
        const auto program = Compile("SELECT k, COUNT(*) FROM a GROUP BY value + 1 AS k", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::Aggregate).GroupKeys.size(), 1);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT id, SUM(value) FROM a", Catalog), TFrontendError);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT SUM(MAX(value)) FROM a", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(JoinScopeAndOuterNullability) {
        const auto program = Compile("SELECT a.id AS a_id, b.id AS b_id FROM a LEFT JOIN b ON a.id == b.id", Catalog);
        UNIT_ASSERT(!program.Results[0].Columns[0].Type.OptionalDepth);
        UNIT_ASSERT(program.Results[0].Columns[1].Type.OptionalDepth);
        UNIT_ASSERT_VALUES_UNEQUAL(program.Results[0].Columns[0].Column, program.Results[0].Columns[1].Column);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT id FROM a JOIN b ON a.id == b.id", Catalog), TFrontendError);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT b.id FROM a LEFT SEMI JOIN b ON a.id == b.id", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(ScalarLambdaSpecializationAndParameters) {
        const auto program = Compile(R"(
            DECLARE $p AS Int32;
            $twice = ($x) -> { RETURN $x * 2; };
            SELECT $twice($p) AS x, $twice(1L) AS y;
        )", Catalog);
        const auto& definitions = Find(program, ERelation::Map).Definitions;
        TEvaluationContext ctx;
        ctx.Parameter = [](std::string_view name) { UNIT_ASSERT_VALUES_EQUAL(name, "p"); return TValue{{ETypeKind::Int32}, std::int64_t{3}}; };
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(program.Expressions, definitions[0].Expression, ctx).Data), 6);
        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int32}));
        UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int64}));
    }

    Y_UNIT_TEST(NamedSourcesGetDistinctBindings) {
        const auto program = Compile("$q = SELECT id FROM a; SELECT l.id, r.id FROM $q AS l JOIN $q AS r ON l.id == r.id", Catalog);
        UNIT_ASSERT_VALUES_UNEQUAL(program.Results[0].Columns[0].Column, program.Results[0].Columns[1].Column);
        UNIT_ASSERT_EXCEPTION(Compile("$q = SELECT * FROM $q; SELECT * FROM $q", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(RebindingPreservesLexicalCapturesAcrossParsers) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                $a = 1;
                $f = ($x) -> { RETURN $x + $a; };
                $g = ($x) -> { RETURN $f($x); };
                $a = $a + 9;
                $f = ($x) -> { RETURN $x + $a; };
                SELECT $g(2) AS old_value, $f(2) AS new_value, $a AS rebound;
            )", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& definitions = Find(program, ERelation::Map).Definitions;
            const std::int64_t expected[]{3, 12, 10};
            for (std::size_t i = 0; i < std::size(expected); ++i)
                UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(program.Expressions, definitions[i].Expression).Data), expected[i]);
            UNIT_ASSERT_EXCEPTION(CompileNative("$f = ($x)->{RETURN $later + $x;}; $later=1; SELECT $f(2)", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(NamedQueryCapturesAgreeWithTableDiscovery) {
        const auto syntax = ParseNative("$q = SELECT * FROM a; $saved = SELECT * FROM $q; $q = SELECT * FROM b; SELECT * FROM $saved");
        const auto tables = CollectTables(syntax);
        UNIT_ASSERT_VALUES_EQUAL(tables.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(tables[0].Name, "a");
        const auto compiled = BindNative(syntax, Catalog);
        UNIT_ASSERT_VALUES_EQUAL(Find(compiled.Get(), ERelation::Read).Table, "a");
    }

    Y_UNIT_TEST(RebindingBuildsALinearSharedGraph) {
        std::string sql = "$x = 0;";
        for (unsigned i = 0; i < 2000; ++i) sql += "$x = $x + 1;";
        sql += "SELECT $x";
        const auto program = Compile(sql, Catalog);
        UNIT_ASSERT(program.Expressions.Size() < 6010);
        const auto expression = Find(program, ERelation::Map).Definitions[0].Expression;
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(program.Expressions, expression).Data), 2000);
    }

    Y_UNIT_TEST(UnionAlignsTypesAndPreservesNames) {
        const auto program = Compile("SELECT 1 AS first UNION ALL SELECT 2L AS first", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns[0].Name, "first");
        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int64}));
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::UnionAll).Inputs.size(), 2);
        const auto named = Compile("SELECT 1 AS first UNION ALL SELECT 2L AS second", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(named.Results[0].Columns.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(named.Results[0].Columns[0].Name, "first");
        UNIT_ASSERT_VALUES_EQUAL(named.Results[0].Columns[1].Name, "second");
        UNIT_ASSERT(named.Results[0].Columns[0].Type == (TType{ETypeKind::Int32, true}));
        UNIT_ASSERT(named.Results[0].Columns[1].Type == (TType{ETypeKind::Int64, true}));
        UNIT_ASSERT_VALUES_EQUAL(Compile("SELECT 1 UNION ALL SELECT 1, 2", Catalog).Results[0].Columns.size(), 2);
    }

    Y_UNIT_TEST(SetChainsStayFlatAndBindGlobalClauses) {
        std::string sql = "WITH q AS (SELECT 1 AS x) SELECT x FROM q";
        for (unsigned i = 0; i < 1000; ++i) sql += " UNION ALL SELECT x FROM q";
        sql += " ORDER BY x LIMIT 5";
        const auto program = Compile(sql, Catalog);
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::UnionAll).Inputs.size(), 1001);
        UNIT_ASSERT(program.Relations.size() < 2010);
        UNIT_ASSERT(program.Relations[program.Results[0].Root].Kind == ERelation::Limit);
        for (const auto* operation : {"UNION", "UNION DISTINCT", "INTERSECT", "INTERSECT ALL", "EXCEPT", "EXCEPT ALL"}) {
            const auto query = Compile(std::string("SELECT CAST(NULL AS Int32) AS x ") + operation + " SELECT 1 AS x ORDER BY x", Catalog);
            UNIT_ASSERT_VALUES_EQUAL(query.Results[0].Columns.size(), 1);
            UNIT_ASSERT(query.Results[0].Columns[0].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT(query.Relations[query.Results[0].Root].Kind == ERelation::Sort);
        }
        const auto aliases = Compile("SELECT id AS x, id AS y FROM a INTERSECT SELECT id AS x, id AS y FROM a ORDER BY x, y DESC", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(Find(aliases, ERelation::Sort).SortKeys.size(), 1);
        for (auto operation : {"INTERSECT ALL", "EXCEPT ALL"}) {
            const auto body = std::string("SELECT id FROM a AS b WHERE b.id = a.id ") + operation + " SELECT id FROM a";
            UNIT_ASSERT_EXCEPTION(Compile("SELECT id FROM a WHERE EXISTS(" + body + ")", Catalog), TUnsupportedFeature);
            const auto independent = std::string("SELECT id FROM a ") + operation + " SELECT id FROM a";
            UNIT_ASSERT(!Compile("SELECT EXISTS(" + independent + ")", Catalog).Results.empty());
        }
    }

    Y_UNIT_TEST(UnknownFeaturesAreErrors) {
        for (auto text : {"PRAGMA unknown; SELECT 1", "SELECT UnknownFunction(1)", "SELECT missing FROM a", "SELECT $undeclared", "SELECT 1 LIMIT -1", "SELECT id FROM a WHERE SUM(value) > 0"}) {
            UNIT_ASSERT_EXCEPTION(Compile(text, Catalog), TFrontendError);
        }
    }

    Y_UNIT_TEST(NullInAndLazyCaseAreExecutable) {
        const auto program = Compile("SELECT 2 IN (1, NULL) AS x, CASE WHEN true THEN 7 ELSE 1/0 END AS y", Catalog);
        const auto& definitions = Find(program, ERelation::Map).Definitions;
        UNIT_ASSERT(Evaluate(program.Expressions, definitions[0].Expression).IsNull());
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(program.Expressions, definitions[1].Expression).Data), 7);
    }

    Y_UNIT_TEST(BracketInListsAreScalarOperationsWithExplicitBoundaries) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Handwritten, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto& [sql, expected] : std::initializer_list<std::pair<const char*, std::optional<bool>>>{
                    {"SELECT 1 IN [1,2,]", true}, {"SELECT 2 IN [1,NULL]", std::nullopt},
                    {"SELECT NULL IN []", false}, {"SELECT 3 NOT IN [1,2]", true},
                    {"SELECT 1 IN [,]", false}, {"SELECT 1 IN [NULL,1]", true},
                    {"SELECT 1 IN [1,2L]", true}, {"SELECT 3 IN [1,2L]", false}}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                const auto result = Evaluate(program.Expressions, Find(program, ERelation::Map).Definitions.front().Expression);
                UNIT_ASSERT_VALUES_EQUAL_C(result.IsNull(), !expected, sql);
                if (expected) UNIT_ASSERT_VALUES_EQUAL_C(std::get<bool>(result.Data), *expected, sql);
            }
            for (const auto* sql : {"SELECT id IN [id] FROM a",
                    "DECLARE $p AS Int32; SELECT 1 IN [$p]"})
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT 1 IN [1,'x']", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(RepeatedProjectionHasDistinctBindings) {
        const auto program = Compile("SELECT id AS a, id AS b FROM a", Catalog);
        UNIT_ASSERT_VALUES_UNEQUAL(program.Results[0].Columns[0].Column, program.Results[0].Columns[1].Column);
    }

    Y_UNIT_TEST(SimpleLikePatterns) {
        const auto program = Compile("SELECT 'abc' LIKE '%b%', 'abc' LIKE 'ab%', 'abc' NOT LIKE '%d'", Catalog);
        for (const auto& definition : Find(program, ERelation::Map).Definitions) {
            UNIT_ASSERT(std::get<bool>(Evaluate(program.Expressions, definition.Expression).Data));
        }
        UNIT_ASSERT(CompileNative("SELECT 'abc' LIKE 'a_c'", Catalog).Program);
    }

    Y_UNIT_TEST(PercentLikePatternsMatchIndependentWildcardTable) {
        auto words = [](std::string_view alphabet) {
            std::vector<std::string> result{std::string{}};
            for (std::size_t i = 0; i < result.size(); ++i) if (result[i].size() < 4) {
                const auto prefix = result[i];
                for (const auto ch : alphabet) result.push_back(prefix + ch);
            }
            return result;
        };
        // Dynamic programming oracle, independent of the native simplifications
        // and terminal runtime matcher.
        auto match = [](std::string_view text, std::string_view pattern) {
            std::vector<bool> previous(text.size() + 1), current(text.size() + 1);
            previous[0] = true;
            for (const char ch : pattern) {
                current[0] = ch == '%' && previous[0];
                for (std::size_t i = 1; i <= text.size(); ++i)
                    current[i] = ch == '%' ? previous[i] || current[i - 1] : previous[i - 1] && text[i - 1] == ch;
                previous.swap(current);
            }
            return bool(previous.back());
        };
        const auto inputs = words("ab");
        for (const auto parser : {EParser::Fast, EParser::Grammar}) for (const auto& pattern : words("ab%")) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto compiled = CompileNative("DECLARE $s AS String; SELECT $s LIKE '" + pattern + "' AS v", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto expression = Find(program, ERelation::Map).Definitions[0].Expression;
            UNIT_ASSERT(program.Expressions[expression].Type == (TType{ETypeKind::Bool}));
            TEvaluationContext context;
            context.Like = [&](const TValue& value, std::string_view text, std::string_view escape, bool sensitive) {
                UNIT_ASSERT(escape.empty() && sensitive);
                return match(std::get<std::string>(value.Data), text);
            };
            for (const auto& input : inputs) {
                context.Parameter = [&](auto) { return TValue{{ETypeKind::String}, input}; };
                UNIT_ASSERT_VALUES_EQUAL_C(std::get<bool>(Evaluate(program.Expressions, expression, context).Data), match(input, pattern), input + " LIKE " + pattern);
            }
        }
        UNIT_ASSERT(CompileNative("SELECT 'abc' LIKE 'a_c'", Catalog).Program);
        UNIT_ASSERT(CompileNative("DECLARE $p AS String; SELECT 'abc' LIKE $p", Catalog).Program);
    }

    Y_UNIT_TEST(ComplexLikeRetainsOnePredicateThroughBinding) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(
                "DECLARE $s AS String?; SELECT $s NOT LIKE '%special%requests%' AS v", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& root = program.Expressions[Find(program, ERelation::Map).Definitions[0].Expression];
            UNIT_ASSERT(root.Op == EOpcode::Not);
            const auto& match = program.Expressions[root.Arguments[0]];
            UNIT_ASSERT(match.Op == EOpcode::Like);
            UNIT_ASSERT(match.Type == TType{ETypeKind::Bool});
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(program.Expressions[match.Arguments[1]].Value), "%special%requests%");
        }
    }

    Y_UNIT_TEST(LinearProjectionGrowth) {
        std::string sql = "SELECT ";
        for (unsigned i = 0; i < 2000; ++i) sql += (i ? "," : "") + std::string("value + ") + std::to_string(i) + " AS c" + std::to_string(i);
        sql += " FROM a";
        const auto program = Compile(sql, Catalog);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns.size(), 2000);
        UNIT_ASSERT(program.Expressions.Size() < 12000);
    }
}
