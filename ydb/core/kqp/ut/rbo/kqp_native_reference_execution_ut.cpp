// Runtime contracts from PR #58587. Preserve the original queries, rows,
// metadata and effects checks; NativeRequired replaces fallback/log polling.
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

namespace NKikimr::NKqp {
using namespace NYdb;

Y_UNIT_TEST_SUITE(KqpNativeReferenceRuntime) {
    Y_UNIT_TEST(NativeCatalogAndResultContract) {
        NKikimrConfig::TAppConfig appConfig;
        auto* config = appConfig.MutableTableServiceConfig();
        config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
        config->SetEnableNewRBO(true);
        config->SetEnableAstCache(true);
        config->SetEnableFallbackToYqlOptimizer(false);
        config->SetEnableFallbackOnMultipleStatements(false);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto scheme = tableSession.ExecuteSchemeQuery(
            "CREATE TABLE `/Root/a` (id Int64 NOT NULL, value String, PRIMARY KEY(id));"
            "CREATE TABLE `/Root/b` (id Int64 NOT NULL, PRIMARY KEY(id));").GetValueSync();
        UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        auto rows = TValueBuilder().BeginList()
            .AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("value").OptionalString("one").EndStruct()
            .AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("value").OptionalString("two").EndStruct()
            .EndList().Build();
        auto write = kikimr.GetTableClient().BulkUpsert("/Root/a", std::move(rows)).GetValueSync();
        UNIT_ASSERT_C(write.IsSuccess(), write.GetIssues().ToString());
        auto keys = TValueBuilder().BeginList().AddListItem().BeginStruct().AddMember("id").Int64(2).EndStruct().EndList().Build();
        auto writeKeys = kikimr.GetTableClient().BulkUpsert("/Root/b", std::move(keys)).GetValueSync();
        UNIT_ASSERT_C(writeKeys.IsSuccess(), writeKeys.GetIssues().ToString());

        auto joined = session.ExecuteQuery(
            "PRAGMA TablePathPrefix = '/Root'; WITH c AS (SELECT id FROM b) "
            "SELECT a.id AS id, a.value AS value FROM a JOIN c ON a.id = c.id ORDER BY a.id;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(joined.IsSuccess(), joined.GetIssues().ToString());
        CompareYson(FormatResultSetYson(joined.GetResultSet(0)), R"([[2;["two"]]])");
        UNIT_ASSERT_VALUES_EQUAL(joined.GetResultSet(0).GetColumnsMeta()[0].Name, "id");
        UNIT_ASSERT_VALUES_EQUAL(joined.GetResultSet(0).GetColumnsMeta()[1].Name, "value");
        // Qualified stars are supported natively even though the legacy
        // forced-YqlSelect translator cannot supply a comparison result.
        auto mixedStar = session.ExecuteQuery("SELECT 1 AS z, a.* FROM `/Root/a` AS a ORDER BY id;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(mixedStar.IsSuccess(), mixedStar.GetIssues().ToString());
        CompareYson(FormatResultSetYson(mixedStar.GetResultSet(0)), R"([[1;1;["one"]];[1;2;["two"]]])");
        auto qualifiedStar = session.ExecuteQuery(
            "SELECT a.* FROM `/Root/a` AS a JOIN `/Root/b` AS b ON a.id = b.id ORDER BY a.id;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(qualifiedStar.IsSuccess(), qualifiedStar.GetIssues().ToString());
        CompareYson(FormatResultSetYson(qualifiedStar.GetResultSet(0)), R"([[2;["two"]]])");
        auto scan = kikimr.GetTableClient().StreamExecuteScanQuery(
            "SELECT id FROM `/Root/b` ORDER BY id;").GetValueSync();
        UNIT_ASSERT_C(scan.IsSuccess(), scan.GetIssues().ToString());
        CompareYson(StreamResultToYson(scan), "[[2]]");
        auto grouped = session.ExecuteQuery(
            "SELECT key, COUNT(*) AS n FROM `/Root/a` GROUP BY id % 2 AS key ORDER BY key;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(grouped.IsSuccess(), grouped.GetIssues().ToString());
        CompareYson(FormatResultSetYson(grouped.GetResultSet(0)), "[[[0];1u];[[1];1u]]");
        // Table information in the native contract must invalidate cached plans
        // after a schema change, including their cached result column metadata.
        const TString star = "SELECT * FROM `/Root/a` ORDER BY id;";
        for (size_t count : {2, 2, 3}) {
            if (count == 3) {
                auto alter = tableSession.ExecuteSchemeQuery("ALTER TABLE `/Root/a` ADD COLUMN extra String;").GetValueSync();
                UNIT_ASSERT_C(alter.IsSuccess(), alter.GetIssues().ToString());
            }
            auto result = session.ExecuteQuery(star, NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).GetColumnsMeta().size(), count);
        }

        // Unsupported semantics remain errors in NativeRequired. Subsequent
        // queries must retain valid catalog and result metadata.
        auto fallback = session.ExecuteQuery("SELECT TablePath() AS path FROM `/Root/b`;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT(!fallback.IsSuccess());
        UNIT_ASSERT_STRING_CONTAINS(fallback.GetIssues().ToString(), "TablePath");
        auto structured = session.ExecuteQuery("SELECT (AsStruct(id AS k)).k AS id FROM `/Root/b`;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(structured.IsSuccess(), structured.GetIssues().ToString());
        CompareYson(FormatResultSetYson(structured.GetResultSet(0)), "[[2]]");
        for (const auto* sql : {
                "SELECT AsStruct(id AS k, Ensure(id, FALSE, 'discarded field') AS bad).k AS id FROM `/Root/b`;",
                "SELECT s.r.k AS id FROM (SELECT AsStruct(id AS k) AS r FROM `/Root/b`) AS s;",
                "SELECT r.k AS id FROM (SELECT AsStruct(id AS k) AS r FROM `/Root/b`) AS s;"}) {
            const auto result = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), TString(sql) + result.GetIssues().ToString());
            CompareYson(FormatResultSetYson(result.GetResultSet(0)), "[[2]]");
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).GetColumnsMeta()[0].Name, "id");
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).GetColumnsMeta()[0].Type.ToString(), "Int64");
        }
        const auto named = session.ExecuteQuery(
            "SELECT s.r.k FROM (SELECT AsStruct(id AS k) AS r FROM `/Root/b`) AS s;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(named.IsSuccess(), named.GetIssues().ToString());
        CompareYson(FormatResultSetYson(named.GetResultSet(0)), "[[2]]");
        UNIT_ASSERT_VALUES_EQUAL(named.GetResultSet(0).GetColumnsMeta()[0].Name, "column0");
        for (const auto& [sql, expected] : TVector<std::pair<TString, TString>>{
                {"SELECT (SELECT AsStruct(id AS k) FROM `/Root/b`).k AS id;", "[[[2]]]"},
                {"SELECT (SELECT AsStruct(id AS k) FROM `/Root/b` WHERE id = 0).k AS id;", "[[#]]"},
                {"PRAGMA kikimr.DqHashOperatorsUseBlocks='false'; "
                 "SELECT (SELECT [AsStruct(id AS k)] FROM `/Root/b`)[0u].k AS id;", "[[[2]]]"}}) {
            const auto result = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), sql + result.GetIssues().ToString());
            CompareYson(FormatResultSetYson(result.GetResultSet(0)), expected);
        }
        const auto multipleRecords = session.ExecuteQuery(
            "SELECT (SELECT AsStruct(id AS k) FROM `/Root/a`).k AS id;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT(!multipleRecords.IsSuccess());
        UNIT_ASSERT_STRING_CONTAINS(multipleRecords.GetIssues().ToString(), "Scalar subquery returned more than one row");
        const auto checked = session.ExecuteQuery(
            "SELECT AsStruct(id AS k, Ensure(id, FALSE, 'selected field') AS bad).bad FROM `/Root/b`;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT(!checked.IsSuccess());
        UNIT_ASSERT_STRING_CONTAINS(checked.GetIssues().ToString(), "selected field");
        auto multipleScan = kikimr.GetTableClient().StreamExecuteScanQuery("SELECT 1; SELECT 2;").GetValueSync();
        TString scanIssues;
        if (multipleScan.IsSuccess()) {
            auto part = multipleScan.ReadNext().GetValueSync();
            UNIT_ASSERT(!part.IsSuccess());
            scanIssues = part.GetIssues().ToString();
        } else {
            scanIssues = multipleScan.GetIssues().ToString();
        }
        UNIT_ASSERT_STRING_CONTAINS(scanIssues, "single result set");
        auto duplicate = session.ExecuteQuery("SELECT 1 AS x, 2 AS x;", NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT(!duplicate.IsSuccess());
        auto invalid = session.ExecuteQuery("PRAGMA YqlSelect = 'invalid'; SELECT id FROM `/Root/a`;",
            NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT(!invalid.IsSuccess());
        auto missing = session.ExecuteQuery("SELECT id FROM `/Root/absent`;", NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT(!missing.IsSuccess());
        auto next = session.ExecuteQuery("SELECT id FROM `/Root/b` WHERE id = 2;", NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(next.IsSuccess(), next.GetIssues().ToString());
        CompareYson(FormatResultSetYson(next.GetResultSet(0)), "[[2]]");
    }

    Y_UNIT_TEST(QualifiedCatalogIdentitySurvivesNativeExecution) {
        NKikimrConfig::TAppConfig app;
        auto* config = app.MutableTableServiceConfig();
        config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
        config->SetEnableNewRBO(true);
        config->SetEnableFallbackToYqlOptimizer(false);
        config->SetEnableFallbackOnMultipleStatements(false);
        config->SetEnableNewRBOPhysicalStagePeephole(false);
        TKikimrRunner kikimr(TKikimrSettings(app).SetWithSampleTables(false));
        auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto schema = tableSession.ExecuteSchemeQuery(
            "CREATE TABLE `/Root/ClusterNames` (id Int64 NOT NULL, value String, PRIMARY KEY(id));"
            "CREATE TABLE `/Root/$q` (id Int64 NOT NULL, PRIMARY KEY(id));").GetValueSync();
        UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto execute = [&](const TString& sql) {
            auto result = session.ExecuteQuery(sql, NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
            return result;
        };
        // Public KQP sessions use the configured cluster "db". Qualified and
        // unqualified writes/reads must share catalog and effect identities.
        const auto result = execute(
            "UPSERT INTO db.`/Root/ClusterNames` (id, value) VALUES (1l, 'first');"
            "UPDATE `/Root/ClusterNames` SET value = 'second' WHERE id = 1l;"
            "SELECT id, value FROM db.`/Root/ClusterNames`;"
            "SELECT id, value FROM `/Root/ClusterNames`;");
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        for (size_t i = 0; i < 2; ++i)
            CompareYson(FormatResultSetYson(result.GetResultSet(i)), R"([[1;["second"]]])");
        execute("UPSERT INTO db.`/Root/$q` (id) VALUES (7l);");
        const auto quoted = execute(
            "PRAGMA TablePathPrefix='/Root'; $q = SELECT 99l AS id;"
            "SELECT id FROM db.`$q`; SELECT id FROM $q;");
        UNIT_ASSERT_VALUES_EQUAL(quoted.GetResultSets().size(), 2);
        CompareYson(FormatResultSetYson(quoted.GetResultSet(0)), "[[7]]");
        CompareYson(FormatResultSetYson(quoted.GetResultSet(1)), "[[99]]");
        for (const auto* sql : {
                "SELECT id FROM remote.`/Root/ClusterNames`;",
                "SELECT id FROM db.`/Root/absent`; SELECT id FROM remote.`/Root/ClusterNames`;"}) {
            const auto rejected = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx(),
                NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
            UNIT_ASSERT_C(!rejected.IsSuccess(), sql);
            UNIT_ASSERT_STRING_CONTAINS(rejected.GetIssues().ToString(), "requires the configured cluster");
            UNIT_ASSERT(rejected.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
        }
        const auto next = execute("SELECT id FROM db.`/Root/ClusterNames`;");
        CompareYson(FormatResultSetYson(next.GetResultSet(0)), "[[1]]");
    }

    Y_UNIT_TEST(DeclaredParametersAndNativeWritesPreserveRowsAndIndexes) {
        TString groupedBaseline;
        for (const bool enable : {false, true}) {
            NKikimrConfig::TAppConfig app;
            auto* config = app.MutableTableServiceConfig();
            config->SetNativeFrontendMode(enable ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            config->SetEnableNewRBO(enable);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableFallbackOnMultipleStatements(false);
            config->SetEnableIndexStreamWrite(true);
            config->SetEnableAstCache(false);
            config->SetCompileQueryCacheSize(0);
            auto settings = TKikimrSettings(app).SetWithSampleTables(false);
            settings.LogSettings = TTestLogSettings();
            settings.LogSettings->DefaultLogPriority = NActors::NLog::PRI_CRIT;
            TKikimrRunner kikimr(settings);
            auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            auto schema = tableSession.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/t` (id Int64 NOT NULL, value String, a Int32 NOT NULL, "
                "PRIMARY KEY(id), INDEX by_value GLOBAL ON (value));").GetValueSync();
            UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            auto params = TParamsBuilder().AddParam("$items").BeginList()
                .AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("value").OptionalString("a")
                    .AddMember("a").Int32(1).EndStruct()
                .AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("value").OptionalString("b")
                    .AddMember("a").Int32(2).EndStruct()
                .EndList().Build().Build();
            const auto insert = session.ExecuteQuery(
                "DECLARE $items AS List<Struct<'id':Int64,'value':String?,'a':Int32>>; "
                "INSERT INTO `/Root/t` SELECT * FROM AS_TABLE($items);",
                NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
            UNIT_ASSERT_C(insert.IsSuccess(), insert.GetIssues().ToString());
            UNIT_ASSERT(insert.GetResultSets().empty());
            const auto execute = [&](const TString& sql) {
                auto result = session.ExecuteQuery(sql, NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
                return result;
            };
            execute("INSERT INTO `/Root/t` (id, value, a) SELECT id + 10, value, a FROM `/Root/t`;");
            execute("UPDATE `/Root/t` SET value = 'changed', a = a + 10 WHERE id = 1;");
            execute("DELETE FROM `/Root/t` WHERE id = 2;");
            const auto rows = execute("SELECT id, value, a FROM `/Root/t` ORDER BY id;");
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(rows.GetResultSet(0)),
                "[[1;[\"changed\"];11];[11;[\"a\"];1];[12;[\"b\"];2]]");
            const auto uuids = execute("SELECT CAST(RandomUuid(id) AS Utf8) FROM `/Root/t`;");
            TResultSetParser uuidRows(uuids.GetResultSet(0));
            THashSet<TString> uniqueUuids;
            while (uuidRows.TryNextRow()) {
                const auto value = uuidRows.ColumnParser(0).GetUtf8();
                UNIT_ASSERT_VALUES_EQUAL(value.size(), 36);
                uniqueUuids.insert(TString(value));
            }
            UNIT_ASSERT_VALUES_EQUAL(uniqueUuids.size(), 3);
            const auto duplicate = session.ExecuteQuery(
                "INSERT INTO `/Root/t` SELECT 1 AS id, 'duplicate' AS value, 0 AS a;",
                NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT(!duplicate.IsSuccess());

            // Require the explicit secondary-index read through the same selected frontend.
            const auto indexed = execute("SELECT id FROM `/Root/t` VIEW by_value WHERE value = 'changed';");
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(indexed.GetResultSet(0)), "[[1]]");
            for (const TString sql : {
                "DECLARE $_ AS Int64; SELECT 1;",
                "INSERT INTO `/Root/t` SELECT 'missing key' AS value, 1 AS a;",
                "INSERT INTO `/Root/t` SELECT 'wrong type' AS id, 1 AS a;",
                "UPDATE `/Root/t` SET id = id + 1;"}) {
                const auto invalid = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx(),
                    NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
                UNIT_ASSERT_C(!invalid.IsSuccess(), sql);
            }
            execute("UPDATE `/Root/t` ON SELECT id, a + 1 AS a FROM `/Root/t` WHERE id = 1;");
            const auto updated = execute("SELECT a FROM `/Root/t` WHERE id = 1;");
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(updated.GetResultSet(0)), "[[12]]");
            auto groupParams = TParamsBuilder().AddParam("$x").Int64(5).Build().Build();
            const auto grouped = session.ExecuteQuery(
                "PRAGMA YqlSelect='force'; PRAGMA YqlSelectAllowUnnamedGroupByExpr; "
                "DECLARE $x AS Int64; SELECT $x AS k, SUM(id + $x) FROM `/Root/t` GROUP BY $x;",
                NQuery::TTxControl::BeginTx().CommitTx(), groupParams).GetValueSync();
            UNIT_ASSERT_C(grouped.IsSuccess(), grouped.GetIssues().ToString());
            if (!enable) { groupedBaseline = FormatResultSetYson(grouped.GetResultSet(0)); }
            else { UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(grouped.GetResultSet(0)), groupedBaseline); }

            const auto duplicateDeclare = session.ExecuteQuery(
                "DECLARE $x AS Int64; DECLARE $x AS String; SELECT $x + 1;",
                NQuery::TTxControl::BeginTx().CommitTx(), groupParams).GetValueSync();
            UNIT_ASSERT_C(duplicateDeclare.IsSuccess(), duplicateDeclare.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(duplicateDeclare.GetResultSet(0)), "[[6]]");

            const auto decimalSchema = tableSession.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/d` (k Decimal(7,0), id Int64 NOT NULL, PRIMARY KEY(k));").GetValueSync();
            UNIT_ASSERT_C(decimalSchema.IsSuccess(), decimalSchema.GetIssues().ToString());
            const auto decimalData = tableSession.ExecuteDataQuery(
                "UPSERT INTO `/Root/d` (k,id) VALUES (NULL,1),(Decimal('2',7,0),2);",
                NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(decimalData.IsSuccess(), decimalData.GetIssues().ToString());
            const auto nullBound = execute("SELECT id FROM `/Root/d` WHERE k IS NULL;");
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(nullBound.GetResultSet(0)), "[[1]]");
        }
    }

    Y_UNIT_TEST_QUAD(DirectRboResultsMatchLegacyFrontend, ColumnStore, PhysicalStagePeephole) {
        const TVector<TString> queries = {
            // Real rows, NULLs and duplicate groups are compared to the existing
            // frontend; NativeRequired requires every native result to use this pipeline.
            "SELECT value, SUM(id) AS s, COUNT(*), COUNT(DISTINCT id), AVG(id) FROM `/Root/t` GROUP BY value ORDER BY value;",
            "SELECT SUM(id), COUNT(*) FROM `/Root/t` WHERE id < 0;",
            "SELECT STDDEV_SAMP(id), STDDEV_SAMPLE(id) FROM `/Root/t`;",
            "SELECT STDDEV_SAMP(id) FROM `/Root/t` WHERE id = 1;",
            "SELECT STDDEV_SAMP(id) FROM `/Root/t` WHERE id < 0;",
            "SELECT id FROM `/Root/t` WHERE Re2::Match('a.*')(value) ORDER BY id;",
            "SELECT id FROM `/Root/t` WHERE Re2::Grep('a', NULL)(value) ORDER BY id;",
            "SELECT id AS n FROM `/Root/t` WHERE id > 2 UNION ALL SELECT id AS n FROM `/Root/t` WHERE id < 3 ORDER BY n DESC LIMIT 2 OFFSET 1;",
            "SELECT id FROM `/Root/t` UNION SELECT id FROM `/Root/t` ORDER BY id DESC LIMIT 1;",
            "SELECT id FROM `/Root/t` INTERSECT SELECT id FROM `/Root/t` WHERE id < 4 "
                "EXCEPT SELECT id FROM `/Root/t` WHERE id = 1 ORDER BY id DESC LIMIT 1;",
            "SELECT DISTINCT value AS z, id AS a FROM `/Root/t` UNION "
                "SELECT value AS z, id AS a FROM `/Root/t` ORDER BY a DESC, z LIMIT 2;",
            "SELECT value AS z, id AS a FROM `/Root/t` INTERSECT "
                "SELECT value AS z, id AS a FROM `/Root/t` ORDER BY a + 1 DESC, z LIMIT 2;",
            "SELECT value, GROUPING(value), SUM(id) FROM `/Root/t` GROUP BY ROLLUP(value) ORDER BY GROUPING(value), value;",
            "SELECT id, SUM(id) OVER (), RANK() OVER (ORDER BY id) FROM `/Root/t` ORDER BY id;",
            "SELECT id FROM (SELECT id FROM `/Root/t` UNION ALL SELECT id FROM `/Root/t` WHERE id <= 2) AS u ORDER BY id;",
            "SELECT l.id, (SELECT MAX(r.id) FROM `/Root/t` AS r WHERE r.id = l.id) FROM `/Root/t` AS l ORDER BY l.id;",
            "SELECT l.id FROM `/Root/t` AS l WHERE EXISTS (SELECT r.id FROM `/Root/t` AS r WHERE r.id = l.id) ORDER BY l.id;",
            "SELECT COUNT(*) FROM `/Root/t` AS l WHERE EXISTS (SELECT * FROM `/Root/t` AS r "
                "WHERE r.id = l.id AND r.value = l.value) AND NOT EXISTS "
                "(SELECT * FROM `/Root/t` AS r WHERE r.id > l.id);",
            "SELECT id FROM `/Root/t` WHERE id NOT IN (SELECT id FROM `/Root/t` WHERE id > 2) ORDER BY id;",
            "SELECT id FROM `/Root/t` WHERE id IN ((SELECT id FROM `/Root/t` WHERE id > 2)) ORDER BY id;",
            "SELECT id + 1 AS n, value FROM `/Root/t` WHERE id > 0 ORDER BY id DESC LIMIT 2 OFFSET 1;",
            "SELECT id FROM `/Root/t` WHERE value IS NULL ORDER BY id;",
            "SELECT id, id IN (1), id NOT IN (2), value IN ('one'), NULL IN (1), 1 IN (NULL), "
                "(1, 2) IN ((1, 2)) FROM `/Root/t` ORDER BY id;",
            "SELECT id, (id, value) IN ((1, 'a'), (3, NULL)), "
                "(id, value) NOT IN ((2, 'a'), (4, NULL)), "
                "((id, value), id) IN (((1, 'a'), 1), ((3, NULL), 3)) "
                "FROM `/Root/t` ORDER BY id;",
            "SELECT DISTINCT value AS v FROM `/Root/t` ORDER BY v;",
            "SELECT value, COUNT(DISTINCT id), SUM(id) FROM `/Root/t` GROUP BY value "
                "HAVING SUM(id) > 0 ORDER BY COUNT(DISTINCT id), value;",
            "SELECT id FROM `/Root/t` WHERE CAST(id AS Decimal(12, 2)) "
                "BETWEEN Decimal('1.00', 12, 2) AND Decimal('2.00', 12, 2) ORDER BY id;",
            // Chained DISTINCT set operations must preserve positional values
            // and NULL equality when their internal output IDs are reordered.
            "SELECT * FROM (SELECT value AS z, id AS a FROM `/Root/t` "
                "INTERSECT SELECT value AS z, id AS a FROM `/Root/t` WHERE id < 4 "
                "EXCEPT SELECT value AS z, id AS a FROM `/Root/t` WHERE id = 1) AS u ORDER BY a;",
            "SELECT d.k FROM (SELECT id + 2 AS k FROM `/Root/t`) AS d ORDER BY d.k;",
            "WITH c AS (SELECT id + 1 AS k FROM `/Root/t`) SELECT l.k AS a, r.k AS b "
                "FROM c AS l LEFT JOIN c AS r ON l.k = r.k ORDER BY a;",
            "SELECT l.id AS a, r.id AS b FROM `/Root/t` AS l LEFT JOIN `/Root/t` AS r ON l.id = r.id AND r.id > 2 ORDER BY a;",
            "SELECT * FROM `/Root/t` ORDER BY id;",
            "SELECT l.id, r.id FROM `/Root/t` AS l JOIN `/Root/t` AS r ON l.id = r.id ORDER BY l.id;",
            "SELECT [1, 2][0u] AS v, CAST(3 AS Int64) + 2 AS n;",
            "SELECT id, [10, 20][id - 2], [10, 20][CAST(id - 2 AS Uint32?)], "
                "[10, 20][CAST(id AS Double) / 2] FROM `/Root/t` ORDER BY id;",
            "SELECT 1, 2 AS column0, 3;",
            "SELECT 1 AS a; SELECT id AS b FROM `/Root/t` ORDER BY id LIMIT 2;",
            // Original workload helper declarations now pass through every
            // native phase, even when only one helper is used by the query.
            "$to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); }; "
                "$to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); }; "
                "$round = ($x, $y) -> { return $x; }; "
                "SELECT $round($to_decimal(id), 2) AS n FROM `/Root/t` ORDER BY id;",
            "$n = 1; $f = ($x) -> { $v = $x + $n; return $v; }; $n = 100; "
                "$q = (SELECT $f(id) AS k FROM `/Root/t`); $alias = $q; "
                "SELECT l.k AS a, r.k AS b FROM $q AS l LEFT JOIN $alias AS r ON l.k = r.k ORDER BY a;",
            "PRAGMA AnsiImplicitCrossJoin; $q = SELECT id FROM `/Root/t`; $r = (SELECT id + 1 AS k FROM $q); "
                "WITH c AS (SELECT k FROM $r) SELECT k FROM c ORDER BY k;",
            "$id = ($x) -> ($x); $make = ($x) -> (($y) -> ($x + $y)); "
                "SELECT $id(id) AS n, $id(value) AS v, $make(id)(2) AS k FROM `/Root/t` ORDER BY n;",
        };
        TVector<TVector<TString>> baseline(queries.size());
        TString failures;
        for (const bool enable : {false, true}) {
            NKikimrConfig::TAppConfig appConfig;
            auto* config = appConfig.MutableTableServiceConfig();
            config->SetNativeFrontendMode(enable ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            config->SetEnableAstCache(enable);
            config->SetEnableNewRBO(enable);
            config->SetEnableNewRBOPhysicalStagePeephole(PhysicalStagePeephole);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableFallbackOnMultipleStatements(false);
            config->SetAllowOlapDataQuery(true);
            // A service cap smaller than the SQL LIMIT checks that host wrapping
            // preserves the SQL OFFSET and applies the external cap exactly once.
            config->MutableQueryLimits()->SetResultRowsLimit(2);
            auto settings = TKikimrSettings(appConfig).SetWithSampleTables(false);
            settings.LogSettings = TTestLogSettings();
            settings.LogSettings->DefaultLogPriority = NActors::NLog::PRI_CRIT;
            TKikimrRunner kikimr(settings);
            auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            auto scheme = tableSession.ExecuteSchemeQuery(TStringBuilder()
                << "CREATE TABLE `/Root/t` (id Int64 NOT NULL, value String, PRIMARY KEY(id))"
                << (ColumnStore ? " WITH (STORE = COLUMN);" : ";")).GetValueSync();
            UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            auto rows = TValueBuilder().BeginList()
                .AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("value").OptionalString("a").EndStruct()
                .AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("value").OptionalString("a").EndStruct()
                .AddListItem().BeginStruct().AddMember("id").Int64(3).AddMember("value").EmptyOptional(EPrimitiveType::String).EndStruct()
                .AddListItem().BeginStruct().AddMember("id").Int64(4).AddMember("value").OptionalString("b").EndStruct()
                .EndList().Build();
            auto data = kikimr.GetTableClient().BulkUpsert("/Root/t", std::move(rows)).GetValueSync();
            UNIT_ASSERT_C(data.IsSuccess(), data.GetIssues().ToString());
            for (size_t i = 0; i < queries.size(); ++i) {
                const auto result = session.ExecuteQuery("PRAGMA YqlSelect='force'; " + queries[i], NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
                if (!result.IsSuccess()) {
                    failures += TStringBuilder() << "native=" << enable << " query=" << i << ": " << queries[i]
                        << "\n" << result.GetIssues().ToString() << "\n";
                    session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
                    continue;
                }
                UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, queries[i]);
                TVector<TString> values;
                for (const auto& set : result.GetResultSets()) {
                    auto compared = set;
                    if (i == 10 || i == 11 || i == 27) {
                        // The old optimizer alphabetizes these DISTINCT set
                        // outputs. Native preserves the SQL projection order.
                        // Pin each public order, then compare every value and
                        // type in SQL order rather than treating reordering as
                        // an expression or set-membership mismatch.
                        UNIT_ASSERT_VALUES_EQUAL(set.GetColumnsMeta().size(), 2);
                        UNIT_ASSERT_VALUES_EQUAL(set.GetColumnsMeta()[0].Name, enable ? "z" : "a");
                        UNIT_ASSERT_VALUES_EQUAL(set.GetColumnsMeta()[1].Name, enable ? "a" : "z");
                        if (!enable) {
                            auto proto = TProtoAccessor::GetProto(set);
                            proto.mutable_columns()->SwapElements(0, 1);
                            for (auto& row : *proto.mutable_rows()) row.mutable_items()->SwapElements(0, 1);
                            compared = TResultSet(std::move(proto));
                        }
                    }
                    values.push_back(FormatResultSetYson(compared));
                    for (const auto& column : compared.GetColumnsMeta()) {
                        values.push_back(TStringBuilder() << column.Name << ":" << column.Type.ToString());
                    }
                }
                if (!enable) { baseline[i] = std::move(values); }
                else if (values != baseline[i]) {
                    failures += TStringBuilder() << "query=" << i << ": " << queries[i] << "\n";
                    for (const auto& value : values) failures += "native: " + value + "\n";
                    for (const auto& value : baseline[i]) failures += "legacy: " + value + "\n";
                }
            }
            // Supply parameter types through the SDK rather than a DECLARE preamble,
            // so this query exercises native parameter binding and scalar annotation.
            auto params = TParamsBuilder().AddParam("$offset").Int64(10).Build().Build();
            const auto parameterized = session.ExecuteQuery(
                "SELECT id + $offset AS n FROM `/Root/t` WHERE id + $offset = 11;",
                NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
            UNIT_ASSERT_C(parameterized.IsSuccess(), parameterized.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(parameterized.GetResultSet(0)), "[[11]]");
            const auto capped = tableSession.ExecuteDataQuery(
                "SELECT id FROM `/Root/t` ORDER BY id LIMIT 4 OFFSET 1;",
                NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(capped.IsSuccess(), capped.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(capped.GetResultSet(0)), "[[2];[3]]");
            UNIT_ASSERT(capped.GetResultSet(0).Truncated());
            if (enable) {
                // The legacy frontend currently duplicates an endpoint for
                // these independently sorted named queries. Check SQL results
                // directly instead of treating that behavior as the oracle.
                const auto localLimits = session.ExecuteQuery(
                    "$first = SELECT id FROM `/Root/t` ORDER BY id DESC LIMIT 1; "
                    "$last = SELECT id FROM `/Root/t` ORDER BY id LIMIT 1; "
                    "SELECT id FROM $first UNION ALL SELECT id FROM $last ORDER BY id;",
                    NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
                UNIT_ASSERT_C(localLimits.IsSuccess(), localLimits.GetIssues().ToString());
                UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(localLimits.GetResultSet(0)), "[[1];[4]]");
                // Legacy RBO import does not consistently widen set operands.
                // Verify values, NULLs and projection order on the native path.
                const auto widened = session.ExecuteQuery(
                    "SELECT DISTINCT value AS z, id AS a FROM `/Root/t` UNION "
                    "SELECT value AS z, CAST(id AS Int32) AS a FROM `/Root/t` ORDER BY a DESC, z LIMIT 2;",
                    NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
                UNIT_ASSERT_C(widened.IsSuccess(), widened.GetIssues().ToString());
                const auto& widenedSet = widened.GetResultSet(0);
                UNIT_ASSERT_VALUES_EQUAL(widenedSet.GetColumnsMeta()[0].Name, "z");
                UNIT_ASSERT_VALUES_EQUAL(widenedSet.GetColumnsMeta()[1].Name, "a");
                TResultSetParser parser(widenedSet);
                UNIT_ASSERT(parser.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(*parser.ColumnParser("z").GetOptionalString(), "b");
                UNIT_ASSERT_VALUES_EQUAL(*parser.ColumnParser("a").GetOptionalInt64(), 4);
                UNIT_ASSERT(parser.TryNextRow());
                UNIT_ASSERT(!parser.ColumnParser("z").GetOptionalString());
                UNIT_ASSERT_VALUES_EQUAL(*parser.ColumnParser("a").GetOptionalInt64(), 3);
                UNIT_ASSERT(!parser.TryNextRow());
            }
        }
        UNIT_ASSERT_C(failures.empty(), failures);
    }

}
} // namespace NKikimr::NKqp
