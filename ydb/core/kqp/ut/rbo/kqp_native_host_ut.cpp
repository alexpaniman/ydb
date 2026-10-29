#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

namespace NKikimr::NKqp {
namespace {

using namespace NYdb;
using namespace NYdb::NTable;
using TMode = decltype(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);

TKikimrSettings NativeSettings(TMode mode) {
    NKikimrConfig::TAppConfig config;
    config.MutableTableServiceConfig()->SetEnableNewRBO(mode != NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    config.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
    config.MutableTableServiceConfig()->SetNativeFrontendMode(mode);
    return TKikimrSettings(config).SetWithSampleTables(false);
}

struct TResultSnapshot {
    TString Values;
    TVector<std::pair<TString, TString>> Columns;
};

TResultSnapshot SnapshotRows(const TResultSet& rows) {
    TResultSnapshot snapshot{FormatResultSetYson(rows), {}};
    for (const auto& column : rows.GetColumnsMeta()) {
        snapshot.Columns.emplace_back(column.Name, column.Type.ToString());
    }
    return snapshot;
}

TResultSnapshot Snapshot(const TDataQueryResult& result) {
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
    UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
    return SnapshotRows(result.GetResultSet(0));
}

void AssertSame(const TResultSnapshot& native, const TResultSnapshot& legacy) {
    UNIT_ASSERT_VALUES_EQUAL(native.Values, legacy.Values);
    UNIT_ASSERT_VALUES_EQUAL(native.Columns.size(), legacy.Columns.size());
    for (size_t i = 0; i < native.Columns.size(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(native.Columns[i].first, legacy.Columns[i].first);
        UNIT_ASSERT_VALUES_EQUAL(native.Columns[i].second, legacy.Columns[i].second);
    }
}

TResultSnapshot ConstantQuery(TMode mode) {
    TKikimrRunner kikimr(NativeSettings(mode));
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    return Snapshot(session.ExecuteDataQuery(
        "SELECT 1 AS answer, 'hello' AS label, NULL IS NULL AS is_null",
        TTxControl::BeginTx().CommitTx()).GetValueSync());
}

TVector<TResultSnapshot> ParameterQueries(TMode mode) {
    TKikimrRunner kikimr(NativeSettings(mode));
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    auto prepared = session.PrepareDataQuery(
        "DECLARE $x AS Int32; SELECT $x + 3 AS answer, $x > 0 AS positive").GetValueSync();
    UNIT_ASSERT_C(prepared.IsSuccess(), prepared.GetIssues().ToString());
    auto query = prepared.GetQuery();
    const auto types = query.GetParameterTypes();
    UNIT_ASSERT_VALUES_EQUAL(types.size(), 1);
    UNIT_ASSERT_VALUES_EQUAL(types.at("$x").ToString(), "Int32");
    TVector<TResultSnapshot> results;
    for (const i32 value : {7, -5}) {
        auto params = query.GetParamsBuilder().AddParam("$x").Int32(value).Build().Build();
        results.push_back(Snapshot(query.Execute(TTxControl::BeginTx().CommitTx(), params).GetValueSync()));
    }
    return results;
}

TResultSnapshot TableQuery(TMode mode) {
    TKikimrRunner kikimr(NativeSettings(mode));
    auto client = kikimr.GetTableClient();
    auto session = client.CreateSession().GetValueSync().GetSession();
    const auto schema = session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/NativeRows` (
            id Uint64 NOT NULL,
            value Int32,
            PRIMARY KEY (id)
        )
    )").GetValueSync();
    UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
    TValueBuilder rows;
    rows.BeginList()
        .AddListItem().BeginStruct()
            .AddMember("id").Uint64(1)
            .AddMember("value").OptionalInt32(10)
        .EndStruct()
        .AddListItem().BeginStruct()
            .AddMember("id").Uint64(2)
            .AddMember("value").OptionalInt32(std::nullopt)
        .EndStruct()
    .EndList();
    const auto inserted = client.BulkUpsert("/Root/NativeRows", rows.Build()).GetValueSync();
    UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
    return Snapshot(session.ExecuteDataQuery(
        "SELECT id, value, value + 1 AS incremented FROM `/Root/NativeRows` WHERE id >= 1ul ORDER BY id",
        TTxControl::BeginTx().CommitTx()).GetValueSync());
}

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeHost) {
    Y_UNIT_TEST(ParserDispatchRemainsNative) {
        TKikimrRunner kikimr(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        for (const auto& [sql, parser] : {std::pair{"SELECT 1 AS answer", "handwritten"},
                                        {"SELECT 'привет' AS answer", "grammar"}}) {
            const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
            Snapshot(result);
            const auto explained = session.ExplainDataQuery(sql).GetValueSync();
            UNIT_ASSERT_C(explained.IsSuccess(), explained.GetIssues().ToString());
            UNIT_ASSERT_C(explained.GetAst().find(std::string("native parser: ") + parser) != std::string::npos,
                explained.GetAst());
        }
    }

    Y_UNIT_TEST(ScalarSemanticsMatchLegacyAcrossReusedSessions) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        TString failures;
        for (const auto* sql : {
            "SELECT 1",
            "--!ansi_lexer\nSELECT 'a''b'",
            "SELECT a.x FROM (SELECT 1 AS x) AS a",
            "SELECT 1 = NULL, NULL = NULL, NULL IN (1, NULL)",
            "SELECT SUM(x) AS total, COUNT(*) AS n FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q",
            "SELECT a.x, b.x AS matched FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS a LEFT JOIN (SELECT 1 AS x) AS b ON a.x = b.x ORDER BY a.x",
            "SELECT DISTINCT x FROM (SELECT 1 AS x UNION ALL SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q ORDER BY x",
            "SELECT COUNT(DISTINCT x) AS n FROM (SELECT 1 AS x UNION ALL SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q",
            "SELECT k, COUNT(*) AS n FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q GROUP BY x % 2 AS k HAVING COUNT(*) > 0ul ORDER BY k",
            "SELECT x FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q ORDER BY x LIMIT 1 OFFSET 1",
            "SELECT 1 + 2, Length('abc')",
            "SELECT Length(Utf8('hello')), StartsWith('abc', 'a'), EndsWith('abc', 'c'), String::AsciiToLower('ABC'), String::AsciiToUpper('abc')",
            "SELECT -128t, -2147483648, -9223372036854775808, -1u",
            "SELECT 7 / 2, 7 % 2, 1 / 0, 1 % 0",
            "SELECT CAST(1 AS Int64), CAST(300 AS Uint8), CAST(-1 AS Uint32), CAST('123' AS Int32)",
            "SELECT Int32('-2147483648'), Uint64('18446744073709551615'), Utf8('hello'), Date('2024-02-29')",
            "SELECT Datetime('1970-01-01T01:30:00+01:30'), Timestamp('1970-01-01T00:00:01.23456789Z')",
            "SELECT IF(TRUE, 1, 2), IF(FALSE, 1), IF(NULL, 1, 2)",
            "SELECT CASE WHEN 1 = NULL THEN 1 ELSE 2 END, CASE WHEN FALSE THEN 1 ELSE NULL END",
            "SELECT COALESCE(NULL, 1), COALESCE(1 / 0, 2), COALESCE(IF(FALSE, 1), 3)",
            "SELECT 'abc' LIKE 'a%', 'abc' LIKE '%c', 'abc' LIKE '%b%', 'a' || 'b'",
            "SELECT IF(FALSE, 'abc') LIKE 'a%', IF(FALSE, 'a') || 'b'"
        }) {
            Cerr << "Native/legacy parity SQL: " << sql << Endl;
            try {
                const auto actual = native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
                UNIT_ASSERT_C(actual.IsSuccess(), TStringBuilder() << sql << '\n' << actual.GetIssues().ToString());
                const auto expected = legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
                UNIT_ASSERT_C(expected.IsSuccess(), TStringBuilder() << sql << '\n' << expected.GetIssues().ToString());
                AssertSame(Snapshot(actual), Snapshot(expected));
            } catch (const std::exception& error) {
                failures += TStringBuilder() << sql << '\n' << error.what() << '\n';
            }
        }
        UNIT_ASSERT_C(failures.empty(), failures);
    }

    Y_UNIT_TEST(ConstantResultsMatchLegacy) {
        AssertSame(ConstantQuery(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED),
            ConstantQuery(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
    }

    Y_UNIT_TEST(GenericMultipleResultsAndScanMatchLegacy) {
        TVector<TResultSnapshot> previous;
        TString previousScan;
        for (const auto mode : {NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY,
                               NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED}) {
            TKikimrRunner kikimr(NativeSettings(mode));
            const auto result = kikimr.GetQueryClient().ExecuteQuery(
                "SELECT 1 AS first; SELECT IF(FALSE, 2) AS second;",
                NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
            for (size_t i = 0; i < 2; ++i) {
                auto snapshot = SnapshotRows(result.GetResultSet(i));
                if (mode == NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY) previous.push_back(std::move(snapshot));
                else AssertSame(snapshot, previous[i]);
            }
            auto scan = kikimr.GetTableClient().StreamExecuteScanQuery(
                "SELECT 1 AS value UNION ALL SELECT 2 AS value").GetValueSync();
            UNIT_ASSERT_C(scan.IsSuccess(), scan.GetIssues().ToString());
            auto values = StreamResultToYson(scan);
            if (mode == NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY) previousScan = std::move(values);
            else UNIT_ASSERT_VALUES_EQUAL(values, previousScan);
        }
    }

    Y_UNIT_TEST(PreparedParameterTypesAndRepeatedExecutionMatchLegacy) {
        const auto native = ParameterQueries(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
        const auto legacy = ParameterQueries(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
        UNIT_ASSERT_VALUES_EQUAL(native.size(), legacy.size());
        for (size_t i = 0; i < native.size(); ++i) {
            AssertSame(native[i], legacy[i]);
        }
    }

    Y_UNIT_TEST(TableResultNamesTypesAndNullsMatchLegacy) {
        AssertSame(TableQuery(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED),
            TableQuery(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
    }

    Y_UNIT_TEST(AutoModeReportsUnsupportedFallback) {
        TKikimrRunner kikimr(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_AUTO));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto result = session.ExecuteDataQuery("SELECT ListLength([1, 2]) AS n",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") != std::string::npos, result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), 1);
        const auto next = session.ExecuteDataQuery("SELECT 3 AS n",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL(Snapshot(next).Values, "[[3]]");
    }

    Y_UNIT_TEST(RequiredModeRejectsUnsupportedWithoutFallback) {
        TKikimrRunner kikimr(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        for (const auto* sql : {"SELECT ListLength([1, 2]) AS n", "SELECT 'a''b'", "SELECT 1 AS 'answer'", "SELECT * FROM 'table'"}) {
            const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(!result.IsSuccess(), sql);
            UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(AutoModeDoesNotFallbackForInvalidQueries) {
        TKikimrRunner kikimr(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_AUTO));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        for (const auto* sql : {"SELECT missing_column", "SELECT FROM", "SELECT NOT NOT true", "SELECT * AS x FROM t",
                               "$unused = *; SELECT 1"}) {
            const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(!result.IsSuccess(), sql);
            UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
        }
    }
}

} // namespace NKikimr::NKqp
