#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <yql/essentials/public/langver/yql_langver.h>

#include <optional>
#include <tuple>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;
using namespace NYdb::NTable;

struct TResultSnapshot {
    TString Values;
    TVector<std::pair<TString, TString>> Columns;
};

TResultSnapshot SnapshotRows(const TResultSet& rows) {
    TResultSnapshot result{FormatResultSetYson(rows), {}};
    for (const auto& column : rows.GetColumnsMeta()) result.Columns.emplace_back(column.Name, column.Type.ToString());
    return result;
}

void AssertSame(const TResultSnapshot& actual, const TResultSnapshot& expected) {
    UNIT_ASSERT_VALUES_EQUAL(actual.Values, expected.Values);
    UNIT_ASSERT_VALUES_EQUAL(actual.Columns, expected.Columns);
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeAggregateFilter) {
    Y_UNIT_TEST_TWIN(MatchConditionalInputs, ColumnStore) {
        struct TCase { TString Native; TString Reference; };
        TVector<TCase> queries;
        TString aggregates, conditional;
        for (const auto& [function, argument] : TVector<std::pair<TString, TString>>{
                {"SUM", "v"}, {"COUNT", "*"}, {"COUNT", "v"}, {"AVG", "v"}, {"MIN", "v"},
                {"MAX", "v"}, {"SOME", "v"}, {"stddev_samp", "v"}, {"SUM", "DISTINCT v"}}) {
            const TString alias = " AS c" + ToString(queries.size());
            const auto value = argument == "*" ? TString("1ul") : argument == "DISTINCT v" ? TString("v") : argument;
            const TString native = function + "(" + argument + ") FILTER(WHERE keep)";
            const TString reference = function + "(" + (argument == "DISTINCT v" ? "DISTINCT " : "") +
                "IF(keep, " + value + ", NULL))";
            // SOME may choose any admitted non-null value; test it where that
            // choice is unique instead of comparing arbitrary representative rows.
            if (function != "SOME" && function != "stddev_samp") {
                if (!aggregates.empty()) { aggregates += ", "; conditional += ", "; }
                aggregates += native + alias;
                conditional += reference + alias;
            }
            const TString suffix = function == "SOME" ? " WHERE id = 1ul" : " WHERE id > 99ul";
            queries.push_back({"SELECT " + native + " AS x FROM `/Root/Filtered`" + suffix,
                "SELECT " + reference + " AS x FROM `/Root/Filtered`" + suffix});
        }
        queries.push_back({"SELECT p, " + aggregates + " FROM `/Root/Filtered` GROUP BY p ORDER BY p",
            "SELECT p, " + conditional + " FROM `/Root/Filtered` GROUP BY p ORDER BY p"});
        // Keep the existing backend restriction on combining DISTINCT with
        // sample standard deviation; exercise each with populated groups.
        queries.push_back({"SELECT p, stddev_samp(v) FILTER(WHERE keep) AS s FROM `/Root/Filtered` GROUP BY p ORDER BY p",
            "SELECT p, stddev_samp(IF(keep, v, NULL)) AS s FROM `/Root/Filtered` GROUP BY p ORDER BY p"});
        queries.push_back({"SELECT SUM(v) FILTER(WHERE FALSE) AS x, COUNT(*) FILTER(WHERE NULL) AS n, SOME(v) FILTER(WHERE FALSE) AS s FROM `/Root/Filtered`",
            "SELECT SUM(IF(FALSE, v, NULL)) AS x, COUNT(IF(NULL, 1ul, NULL)) AS n, SOME(IF(FALSE, v, NULL)) AS s FROM `/Root/Filtered`"});
        queries.push_back({"SELECT SUM(Ensure(id, id > 1ul, 'excluded')) FILTER(WHERE id > 1ul) AS x FROM `/Root/Filtered`",
            "SELECT SUM(IF(id > 1ul, Ensure(id, id > 1ul, 'excluded'), NULL)) AS x FROM `/Root/Filtered`"});
        queries.push_back({"SELECT SUM(Ensure(v, FALSE, 'excluded')) FILTER(WHERE FALSE) AS x FROM `/Root/Filtered`",
            "SELECT SUM(IF(FALSE, Ensure(v, FALSE, 'excluded'), NULL)) AS x FROM `/Root/Filtered`"});
        queries.push_back({"SELECT p, MIN_BY(id, v) FILTER(WHERE keep) AS lo, MAX_BY(id, v) FILTER(WHERE keep) AS hi "
                "FROM `/Root/Filtered` GROUP BY p ORDER BY p",
            "SELECT p, MIN_BY(IF(keep, id, NULL), IF(keep, v, NULL)) AS lo, "
                "MAX_BY(IF(keep, id, NULL), IF(keep, v, NULL)) AS hi FROM `/Root/Filtered` GROUP BY p ORDER BY p"});
        const TString window = " OVER(PARTITION BY p ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)";
        queries.push_back({"SELECT id, SUM(v) FILTER(WHERE keep)" + window + " AS s, COUNT(*) FILTER(WHERE keep)" +
                window + " AS n, SUM(v)" + window + " AS all_values FROM `/Root/Filtered` ORDER BY id",
            "SELECT id, SUM(IF(keep, v, NULL))" + window + " AS s, COUNT(IF(keep, 1ul, NULL))" + window +
                " AS n, SUM(v)" + window + " AS all_values FROM `/Root/Filtered` ORDER BY id"});
        queries.push_back({"SELECT id, SUM(v) FILTER(WHERE keep) OVER w AS s, Math::Abs(v) AS a "
                "FROM `/Root/Filtered` WINDOW w AS (PARTITION BY p ORDER BY id) ORDER BY id",
            "SELECT id, SUM(IF(keep, v, NULL)) OVER w AS s, Math::Abs(v) AS a "
                "FROM `/Root/Filtered` WINDOW w AS (PARTITION BY p ORDER BY id) ORDER BY id"});
        for (const bool blocks : {false, true}) {
            TVector<TResultSnapshot> reference;
            for (const bool native : {false, true}) {
                NKikimrConfig::TAppConfig config;
                auto* service = config.MutableTableServiceConfig();
                service->SetEnableNewRBO(native);
                service->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
                service->SetEnableFallbackToYqlOptimizer(false);
                service->SetAllowOlapDataQuery(true);
                service->SetDefaultLangVer(NYql::GetMaxLangVersion());
                TKikimrRunner runner(TKikimrSettings(config).SetWithSampleTables(false));
                auto client = runner.GetTableClient();
                auto session = client.CreateSession().GetValueSync().GetSession();
                const auto schema = session.ExecuteSchemeQuery(TString(
                    "CREATE TABLE `/Root/Filtered` (id Uint64 NOT NULL, p Int32, v Int32, keep Bool, PRIMARY KEY(id))") +
                    (ColumnStore ? " WITH (STORE = COLUMN);" : ";")).GetValueSync();
                UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
                TValueBuilder rows;
                rows.BeginList();
                using TRow = std::tuple<ui64, i32, std::optional<i32>, std::optional<bool>>;
                for (const auto& [id, p, v, keep] : TVector<TRow>{{1, 1, 10, true}, {2, 1, 20, false},
                        {3, 1, std::nullopt, true}, {4, 2, 30, std::nullopt}, {5, 2, -5, true},
                        {6, 3, std::nullopt, false}, {7, 1, 30, true}})
                    rows.AddListItem().BeginStruct().AddMember("id").Uint64(id).AddMember("p").OptionalInt32(p)
                        .AddMember("v").OptionalInt32(v).AddMember("keep").OptionalBool(keep).EndStruct();
                rows.EndList();
                const auto loaded = client.BulkUpsert("/Root/Filtered", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(loaded.IsSuccess(), loaded.GetIssues().ToString());
                const TString header = (native ? TString("PRAGMA YqlSelect='force'; ") : TString()) +
                    "PRAGMA ydb.DisableBlockExecution='" + (blocks ? "false" : "true") + "'; ";
                for (size_t i = 0; i < queries.size(); ++i) {
                    const auto sql = header + (native ? queries[i].Native : queries[i].Reference);
                    const auto result = runner.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
                    UNIT_ASSERT_C(result.IsSuccess(), sql << '\n' << result.GetIssues().ToString());
                    UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
                    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                    const auto actual = SnapshotRows(result.GetResultSet(0));
                    if (native) AssertSame(actual, reference.at(i)); else reference.push_back(actual);
                }
            }
        }
    }

}
} // namespace NKikimr::NKqp
