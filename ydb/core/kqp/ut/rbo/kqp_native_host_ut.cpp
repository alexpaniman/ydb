#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <yql/essentials/public/langver/yql_langver.h>
#include <library/cpp/json/json_reader.h>
#include <util/stream/file.h>
#include <util/string/subst.h>

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

TVector<TResultSnapshot> TpchReadQueries(TMode mode, bool newRbo) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(newRbo);
    service->SetNativeFrontendMode(mode);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetEnableInlineJoinFiltersAfterCBO(false);
    service->SetEnableFallbackOnMultipleStatements(false);
    service->SetAllowOlapDataQuery(true);
    service->SetDefaultLangVer(NYql::GetMaxLangVersion());
    service->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
    TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
    auto client = kikimr.GetTableClient();
    auto session = client.CreateSession().GetValueSync().GetSession();
    auto schema = TFileInput(SRC_("data/schema/tpch.sql")).ReadAll();
    // Same schema and storage settings as RunPerf_YqlTest.
    SubstGlobal(schema, "\n);", "\n) WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 16);");
    const auto created = session.ExecuteSchemeQuery(schema).GetValueSync();
    UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
    struct TRow {
        const char* Flag;
        const char* Status;
        const char* Quantity;
        const char* Price;
        const char* Discount;
        const char* Tax;
        const char* ShipDate;
        i64 Part = 2;
        const char* ShipMode = "AIR";
        const char* Instruction = "DELIVER IN PERSON";
    };
    const TRow data[]{
        {"A", "F", "17", "100", "0.05", "0.02", "1998-09-01"},
        {"A", "F", "20", "200", "0.10", "0.05", "1998-09-02"},
        {"A", "F", "99", "999", "0.90", "0.90", "1998-09-03"}, // outside inclusive date boundary
        {"R", "F", "20", "200", "0.10", "0.05", "1997-01-01"},
        {"R", "F", nullptr, "50", nullptr, "0.03", "1997-01-01"},
        {nullptr, "F", "1", "10", "0", "0", "1997-01-01"},
        {"N", "O", nullptr, nullptr, nullptr, nullptr, "1997-01-01"}, // all-null measures
        {"A", "F", "99", "999", "0.90", "0.90", nullptr},
        {"A", "F", "17", "100", "0.05", "0.02", "1998-09-01"}, // second line in the same order
        {"A", "F", "1", "10", "0", "0", "1999-01-01", 1}, // first Q19 branch
        {"A", "F", "25", "100", "0.10", "0", "1999-01-01", 3, "AIR REG"}, // third branch
        {"A", "F", "25", "100", "0.10", "0", "1999-01-01", 3, "TRUCK"},
        {"A", "F", "25", "100", "0.10", "0", "1999-01-01", 3, "AIR", "COLLECT COD"},
        {"A", "F", "25", "100", "0.10", "0", "1999-01-01", 999}, // unmatched part
        {"A", "F", "11", "100", "0", "0", "1999-01-01", 1}, // inclusive quantity boundary
        {"A", "F", "12", "100", "0", "0", "1999-01-01", 1},
    };
    const auto decimalType = TTypeBuilder().Decimal({12, 2}).Build();
    TValueBuilder rows;
    rows.BeginList();
    i64 key = 0;
    for (const auto& row : data) {
        ++key;
        rows.AddListItem().BeginStruct()
            .AddMember("l_orderkey").Int64(key == 9 ? 1 : key)
            .AddMember("l_linenumber").Int32(key == 9 ? 2 : 1)
            .AddMember("l_comment").OptionalUtf8(std::nullopt)
            .AddMember("l_partkey").OptionalInt64(row.Part)
            .AddMember("l_suppkey").OptionalInt64(std::nullopt)
            .AddMember("l_shipinstruct").OptionalUtf8(row.Instruction)
            .AddMember("l_shipmode").OptionalUtf8(row.ShipMode)
            .AddMember("l_commitdate").OptionalDate(std::nullopt)
            .AddMember("l_receiptdate").OptionalDate(std::nullopt)
            .AddMember("l_returnflag").OptionalUtf8(row.Flag ? std::optional<std::string>{row.Flag} : std::nullopt)
            .AddMember("l_linestatus").OptionalUtf8(row.Status)
            .AddMember("l_shipdate").OptionalDate(row.ShipDate ?
                std::optional<TInstant>{TInstant::ParseIso8601(TString(row.ShipDate) + "T00:00:00Z")} : std::nullopt);
        for (const auto [name, value] : {std::pair{"l_quantity", row.Quantity}, {"l_extendedprice", row.Price},
                {"l_discount", row.Discount}, {"l_tax", row.Tax}}) {
            rows.AddMember(name);
            if (value) rows.BeginOptional().Decimal(TDecimalValue(value, 12, 2)).EndOptional();
            else rows.EmptyOptional(decimalType);
        }
        rows.EndStruct();
    }
    rows.EndList();
    const auto inserted = client.BulkUpsert("/Root/lineitem", rows.Build()).GetValueSync();
    UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
    TValueBuilder customers, orders, parts;
    customers.BeginList();
    for (const i64 id : {101, 102, 103}) {
        customers.AddListItem().BeginStruct()
            .AddMember("c_custkey").Int64(id)
            .AddMember("c_mktsegment").OptionalUtf8(id == 103 ? std::nullopt :
                std::optional<std::string>{id == 101 ? "BUILDING" : "AUTOMOBILE"})
            .AddMember("c_acctbal").EmptyOptional(decimalType)
            .AddMember("c_address").OptionalUtf8(std::nullopt)
            .AddMember("c_comment").OptionalUtf8(std::nullopt)
            .AddMember("c_name").OptionalUtf8(std::nullopt)
            .AddMember("c_nationkey").OptionalInt32(std::nullopt)
            .AddMember("c_phone").OptionalUtf8(std::nullopt)
            .EndStruct();
    }
    customers.EndList();
    orders.BeginList();
    for (const i64 id : {1, 2, 3, 4, 5, 6, 8, 99}) {
        orders.AddListItem().BeginStruct()
            .AddMember("o_orderkey").Int64(id)
            .AddMember("o_custkey").OptionalInt64(id == 4 ? 102 : id == 5 ? 999 : id == 6 ? 103 : 101)
            .AddMember("o_orderdate").OptionalDate(id == 8 ? std::nullopt :
                std::optional<TInstant>{TInstant::ParseIso8601(id == 3 ? "1995-03-15T00:00:00Z" : "1995-03-14T00:00:00Z")})
            .AddMember("o_shippriority").OptionalInt32(0)
            .AddMember("o_clerk").OptionalUtf8(std::nullopt)
            .AddMember("o_comment").OptionalUtf8(std::nullopt)
            .AddMember("o_orderpriority").OptionalUtf8(std::nullopt)
            .AddMember("o_orderstatus").OptionalUtf8(std::nullopt)
            .AddMember("o_totalprice").EmptyOptional(decimalType)
            .EndStruct();
    }
    orders.EndList();
    parts.BeginList();
    for (const i64 id : {1, 2, 3, 4, 5, 6, 7, 8}) {
        parts.AddListItem().BeginStruct()
            .AddMember("p_partkey").Int64(id)
            .AddMember("p_brand").OptionalUtf8(id == 1 ? "Brand#12" : id == 2 ? "Brand#23" : "Brand#34")
            .AddMember("p_container").OptionalUtf8(id == 1 ? "SM BOX" : id == 2 ? "MED BOX" : "LG BOX")
            .AddMember("p_size").OptionalInt32(id < 5 ? 3 : id == 7 ? 14 : 15)
            .AddMember("p_comment").OptionalUtf8(std::nullopt)
            .AddMember("p_mfgr").OptionalUtf8("Maker")
            .AddMember("p_name").OptionalUtf8(std::nullopt)
            .AddMember("p_retailprice").EmptyOptional(decimalType)
            .AddMember("p_type").OptionalUtf8(id < 5 ? std::nullopt : std::optional<std::string>{id == 8 ? "LARGE TIN" : "LARGE BRASS"})
            .EndStruct();
    }
    parts.EndList();
    TValueBuilder regions, nations, suppliers, partsupp;
    regions.BeginList();
    for (const i32 id : {1, 2}) regions.AddListItem().BeginStruct()
        .AddMember("r_regionkey").Int32(id)
        .AddMember("r_name").OptionalUtf8(id == 1 ? "EUROPE" : "ASIA")
        .AddMember("r_comment").OptionalUtf8(std::nullopt).EndStruct();
    regions.EndList();
    nations.BeginList();
    for (const i32 id : {1, 2, 3}) nations.AddListItem().BeginStruct()
        .AddMember("n_nationkey").Int32(id)
        .AddMember("n_name").OptionalUtf8(id == 1 ? "GERMANY" : id == 2 ? "FRANCE" : "JAPAN")
        .AddMember("n_regionkey").OptionalInt32(id == 3 ? 2 : 1)
        .AddMember("n_comment").OptionalUtf8(std::nullopt).EndStruct();
    nations.EndList();
    suppliers.BeginList();
    for (const i64 id : {1, 2, 3, 4, 5}) suppliers.AddListItem().BeginStruct()
        .AddMember("s_suppkey").Int64(id)
        .AddMember("s_name").OptionalUtf8("Supplier" + std::to_string(id))
        .AddMember("s_nationkey").OptionalInt32(id == 5 ? std::nullopt : std::optional<i32>{id == 4 ? 1 : i32(id)})
        .AddMember("s_acctbal").BeginOptional().Decimal(TDecimalValue(id == 2 ? "200" : id == 3 ? "999" : "100", 12, 2)).EndOptional()
        .AddMember("s_address").OptionalUtf8("Address")
        .AddMember("s_phone").OptionalUtf8("Phone")
        .AddMember("s_comment").OptionalUtf8(std::nullopt).EndStruct();
    suppliers.EndList();
    partsupp.BeginList();
    // Tied European minima for part 5; lower Asian/missing-nation costs must
    // not affect that minimum. Part 6 has a NULL competing cost. Parts 7/8
    // fail size/type filters even though their supply costs qualify.
    for (const auto& [part, supplier, cost] : std::initializer_list<std::tuple<i64, i64, const char*>>{
            {5, 1, "10"}, {5, 2, "10"}, {5, 3, "1"}, {5, 4, "11"}, {5, 5, "0"},
            {6, 1, "20"}, {6, 2, nullptr}, {7, 1, "1"}, {8, 1, "1"}}) {
        partsupp.AddListItem().BeginStruct()
            .AddMember("ps_partkey").Int64(part).AddMember("ps_suppkey").Int64(supplier)
            .AddMember("ps_availqty").OptionalInt32(100).AddMember("ps_comment").OptionalUtf8(std::nullopt)
            .AddMember("ps_supplycost");
        if (cost) partsupp.BeginOptional().Decimal(TDecimalValue(cost, 12, 2)).EndOptional();
        else partsupp.EmptyOptional(decimalType);
        partsupp.EndStruct();
    }
    partsupp.EndList();
    for (auto& [table, values] : TVector<std::pair<TString, TValue>>{
            {"/Root/customer", customers.Build()}, {"/Root/orders", orders.Build()}, {"/Root/part", parts.Build()},
            {"/Root/region", regions.Build()}, {"/Root/nation", nations.Build()}, {"/Root/supplier", suppliers.Build()}, {"/Root/partsupp", partsupp.Build()}}) {
        const auto loaded = client.BulkUpsert(table, std::move(values)).GetValueSync();
        UNIT_ASSERT_C(loaded.IsSuccess(), loaded.GetIssues().ToString());
    }
    const TString prelude = R"($to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };
$to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };
$round = ($x,$y) -> {return $x;};
)";
    TVector<TResultSnapshot> snapshots;
    for (const auto [query, expectedRows] : {std::pair{1, 4}, {2, 3}, {3, 2}, {17, 1}, {19, 1}}) {
        Cerr << "TPC-H Q" << query << ", native mode " << int(mode) << ", new RBO " << newRbo << Endl;
        const auto sql = prelude + TFileInput(SRC_(TStringBuilder() << "data/yql-tpch/q" << query << ".yql")).ReadAll();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), expectedRows);
        if (query == 2) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto [part, supplier] : {std::pair{5, "Supplier2"}, {5, "Supplier1"}, {6, "Supplier1"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("p_partkey").GetInt64(), part);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("s_name").GetOptionalUtf8().value(), supplier);
            }
        }
        if (query == 17) {
            TResultSetParser values(result.GetResultSet(0));
            UNIT_ASSERT(values.TryNextRow());
            const auto average = values.ColumnParser("avg_yearly").GetOptionalDecimal();
            UNIT_ASSERT(average);
            UNIT_ASSERT_VALUES_EQUAL(average->ToString(), "1.43");
        }
        if (query == 3 || query == 19) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto* expected : query == 3 ? TVector<const char*>{"190", "180"} : TVector<const char*>{"750"}) {
                UNIT_ASSERT(values.TryNextRow());
                const auto revenue = values.ColumnParser(query == 3 ? 1 : 0).GetOptionalDecimal();
                UNIT_ASSERT(revenue);
                UNIT_ASSERT_VALUES_EQUAL(revenue->ToString(), expected);
            }
        }
        if (newRbo) {
            const auto explain = kikimr.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx(),
                NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
            UNIT_ASSERT_C(explain.IsSuccess(), explain.GetIssues().ToString());
            UNIT_ASSERT(explain.GetStats() && explain.GetStats()->GetPlan());
            const TString plan{*explain.GetStats()->GetPlan()};
            NJson::TJsonValue root;
            NJson::ReadJsonTree(plan, &root, true);
            const auto& stats = root.GetMapSafe().at("SimplifiedPlan").GetMapSafe().at("OptimizerStats").GetMapSafe();
            const auto total = stats.at("CBOTreesTotal").GetUIntegerSafe();
            UNIT_ASSERT_VALUES_EQUAL_C(stats.at("CBOTreesOptimized").GetUIntegerSafe(), total, plan);
            if (query != 1) UNIT_ASSERT_C(total > 0, plan);
        }
        snapshots.push_back(SnapshotRows(result.GetResultSet(0)));
    }
    return snapshots;
}

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeHost) {
    Y_UNIT_TEST_TWIN(ExistsSubqueriesMatchLegacyOnPopulatedTables, EqualNullsJoinKeys) {
        struct TCase {
            const char* Sql;
            TVector<ui64> Ids;
            TVector<bool> Values;
        };
        const TVector<TCase> cases{
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE EXISTS(SELECT b.value FROM `/Root/ExistsRows` AS b WHERE b.value = a.value) ORDER BY a.id", {1, 3, 4, 6}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE NOT EXISTS(SELECT b.value FROM `/Root/ExistsRows` AS b WHERE b.value = a.value) ORDER BY a.id", {2, 5}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE EXISTS(SELECT b.id FROM `/Root/ExistsRows` AS b WHERE b.id < a.id) ORDER BY a.id", {2, 3, 4, 5, 6}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE EXISTS(SELECT b.id FROM `/Root/ExistsRows` AS b WHERE b.id = a.id) AND NOT EXISTS(SELECT id FROM `/Root/ExistsRows` WHERE id = 99ul) ORDER BY a.id", {1, 2, 3, 4, 5, 6}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE a.id = 1ul OR EXISTS(SELECT b.id FROM `/Root/ExistsRows` AS b WHERE b.value < a.value) ORDER BY a.id", {1, 3, 4}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE EXISTS(SELECT b.id FROM `/Root/ExistsRows` AS b WHERE b.id < a.id AND EXISTS(SELECT c.id FROM `/Root/ExistsRows` AS c WHERE c.id = b.id)) ORDER BY a.id", {2, 3, 4, 5, 6}},
            {"SELECT a.id, EXISTS(SELECT b.id, b.value FROM `/Root/ExistsRows` AS b WHERE b.value = a.value) AS v FROM `/Root/ExistsRows` AS a ORDER BY a.id", {1, 2, 3, 4, 5, 6}, {true, false, true, true, false, true}},
            {"SELECT a.id, NOT EXISTS(SELECT id FROM `/Root/ExistsRows` WHERE id = 99ul) AS v FROM `/Root/ExistsRows` AS a ORDER BY a.id", {1, 2, 3, 4, 5, 6}, {true, true, true, true, true, true}},
            {"SELECT a.id, EXISTS(SELECT COUNT(*) FROM `/Root/ExistsRows` AS b WHERE b.id < a.id) AS v FROM `/Root/ExistsRows` AS a ORDER BY a.id", {1, 2, 3, 4, 5, 6}, {true, true, true, true, true, true}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE EXISTS(SELECT id FROM `/Root/ExistsRows` LIMIT 0) ORDER BY a.id", {}},
            {"SELECT a.id FROM `/Root/ExistsRows` AS a WHERE EXISTS(SELECT id FROM `/Root/ExistsRows` WHERE id = 99ul UNION ALL SELECT id FROM `/Root/ExistsRows` WHERE id = 1ul) ORDER BY a.id", {1, 2, 3, 4, 5, 6}},
        };
        TVector<std::optional<TResultSnapshot>> reference(cases.size());
        for (const int strategy : {1, 2, 0}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(strategy != 0);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetEnableBlockHashJoinEqualNulls(EqualNullsJoinKeys);
            service->SetNativeFrontendMode(strategy == 2 ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED :
                NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            service->SetDefaultLangVer(NYql::GetMaxLangVersion());
            service->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
            TKikimrRunner runner(TKikimrSettings(config).SetWithSampleTables(false));
            auto client = runner.GetTableClient();
            auto session = client.CreateSession().GetValueSync().GetSession();
            const auto schema = session.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/ExistsRows` (id Uint64 NOT NULL, value Int32, PRIMARY KEY(id))").GetValueSync();
            UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
            TValueBuilder rows;
            rows.BeginList();
            const std::optional<i32> values[]{10, std::nullopt, 30, 10, std::nullopt, 0};
            for (ui64 id = 1; id <= 6; ++id) rows.AddListItem().BeginStruct()
                .AddMember("id").Uint64(id).AddMember("value").OptionalInt32(values[id - 1]).EndStruct();
            rows.EndList();
            const auto inserted = client.BulkUpsert("/Root/ExistsRows", rows.Build()).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
            for (size_t index = 0; index < cases.size(); ++index) {
                const auto& test = cases[index];
                Cerr << "EXISTS strategy=" << strategy << ", case=" << index << ": " << test.Sql << Endl;
                const auto result = session.ExecuteDataQuery(TString("PRAGMA YqlSelect='force'; ") + test.Sql,
                    TTxControl::BeginTx().CommitTx()).GetValueSync();
                // Original new RBO does not inline projected EXISTS calls. Preserve
                // these baseline failures; only the native Map path is enabled here.
                if (strategy == 1 && index >= 6 && index <= 8) {
                    UNIT_ASSERT(!result.IsSuccess());
                    UNIT_ASSERT_C(result.GetIssues().ToString().find(index == 6 ? "Unknown operator encountered: AddDependencies" : "column not found") != std::string::npos,
                        result.GetIssues().ToString());
                    session = client.CreateSession().GetValueSync().GetSession();
                    continue;
                }
                const auto actual = Snapshot(result);
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), test.Ids.size());
                TResultSetParser parser(result.GetResultSet(0));
                for (size_t row = 0; row < test.Ids.size(); ++row) {
                    UNIT_ASSERT(parser.TryNextRow());
                    UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser("id").GetUint64(), test.Ids[row]);
                    if (!test.Values.empty()) {
                        // The old optimizer loses the single global-aggregate row
                        // for an empty correlated input. Native EXISTS is true.
                        const bool expected = strategy == 0 && index == 8 && row == 0 ? false : test.Values[row];
                        UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser("v").GetBool(), expected);
                    }
                }
                if (!reference[index]) reference[index] = actual;
                else if (strategy == 0 && index == 8) {
                    UNIT_ASSERT(actual.Columns == reference[index]->Columns);
                } else AssertSame(actual, *reference[index]);
            }
        }
    }

    Y_UNIT_TEST_TWIN(CorrelatedScalarSubqueriesMatchLegacyOnPopulatedTables, EqualNullsJoinKeys) {
        TVector<std::optional<TResultSnapshot>> reference;
        TString failures;
        for (const int strategy : {1, 2, 0}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(strategy != 0);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetEnableBlockHashJoinEqualNulls(EqualNullsJoinKeys);
            service->SetNativeFrontendMode(strategy == 2 ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED :
                NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            service->SetDefaultLangVer(NYql::GetMaxLangVersion());
            service->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
            TKikimrRunner runner(TKikimrSettings(config).SetWithSampleTables(false));
            auto client = runner.GetTableClient();
            auto session = client.CreateSession().GetValueSync().GetSession();
            const auto schema = session.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/CorrelatedRows` (id Uint64 NOT NULL, value Int32, PRIMARY KEY(id))").GetValueSync();
            UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
            const std::optional<i32> values[]{10, {}, 30, 10, {}, 0};
            TValueBuilder rows;
            rows.BeginList();
            for (ui64 i = 0; i < std::size(values); ++i) rows.AddListItem().BeginStruct()
                .AddMember("id").Uint64(i + 1).AddMember("value").OptionalInt32(values[i]).EndStruct();
            rows.EndList();
            const auto inserted = client.BulkUpsert("/Root/CorrelatedRows", rows.Build()).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
            const std::pair<const char*, size_t> cases[]{
                {"SELECT a.id, (SELECT b.value FROM `/Root/CorrelatedRows` AS b WHERE b.id = a.id) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT MAX(b.value) FROM `/Root/CorrelatedRows` AS b WHERE b.value = a.value) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT MAX(b.value) FROM `/Root/CorrelatedRows` AS b WHERE b.id < a.id) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT COUNT(*) FROM `/Root/CorrelatedRows` AS b WHERE b.id < a.id) AS n FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT COUNT(*) FROM `/Root/CorrelatedRows` AS b WHERE b.value = a.value) AS n FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT (SELECT a.value)) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT MAX(b.value) + a.value FROM `/Root/CorrelatedRows` AS b) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT SUM(a.value) FROM `/Root/CorrelatedRows` AS b WHERE b.id < 3ul) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT MAX(b.value) FROM `/Root/CorrelatedRows` AS b HAVING MAX(b.value) > a.value) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.id, (SELECT b.value FROM `/Root/CorrelatedRows` AS b WHERE b.id < a.id ORDER BY b.id DESC LIMIT 1) AS v FROM `/Root/CorrelatedRows` AS a ORDER BY a.id", 6},
                {"SELECT a.value, (SELECT COUNT(*) FROM `/Root/CorrelatedRows` AS b WHERE b.value = a.value) AS n FROM `/Root/CorrelatedRows` AS a GROUP BY a.value ORDER BY a.value", 4},
                {"SELECT a.id FROM `/Root/CorrelatedRows` AS a WHERE a.value > (SELECT AVG(b.value) FROM `/Root/CorrelatedRows` AS b WHERE b.id <= a.id) ORDER BY a.id", 1},
            };
            size_t index = 0;
            for (const auto& [sql, rowCount] : cases) {
                Cerr << "Correlated scalar strategy=" << strategy << ", case=" << index << ": " << sql << Endl;
                const auto result = session.ExecuteDataQuery(TString("PRAGMA YqlSelect='force'; ") + sql,
                    TTxControl::BeginTx().CommitTx()).GetValueSync();
                if (strategy == 1) reference.emplace_back();
                // The existing SQL frontend cannot resolve this grandparent
                // reference through a SELECT without FROM. Its failure is not
                // a native execution reference; the result is checked below.
                if (index == 5 && strategy != 2) {
                    UNIT_ASSERT(!result.IsSuccess());
                    UNIT_ASSERT_C(result.GetIssues().ToString().find("No such column: value") != std::string::npos, result.GetIssues().ToString());
                    ++index;
                    continue;
                }
                if (index == 6 || index == 8) {
                    UNIT_ASSERT(!result.IsSuccess());
                    const auto* message = strategy == 2 ? "outer references after aggregation" : "must appear in GROUP BY";
                    UNIT_ASSERT_C(result.GetIssues().ToString().find(message) != std::string::npos, result.GetIssues().ToString());
                    ++index;
                    continue;
                }
                if (index == 9 && strategy == 0) {
                    UNIT_ASSERT(!result.IsSuccess());
                    UNIT_ASSERT_C(result.GetIssues().ToString().find("Member not found: _alias_b.id") != std::string::npos, result.GetIssues().ToString());
                    ++index;
                    continue;
                }
                if (!result.IsSuccess()) {
                    failures += TStringBuilder() << "strategy=" << strategy << ", case=" << index << ": " << result.GetIssues().ToString() << '\n';
                    session = client.CreateSession().GetValueSync().GetSession();
                    ++index;
                    continue;
                }
                const auto actual = Snapshot(result);
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), rowCount);
                if (index == 0 || index == 3 || index == 4 || index == 5 || index == 10) {
                    TResultSetParser data(result.GetResultSet(0));
                    const ui64 counts[]{2, 0, 1, 2, 0, 1};
                    const std::optional<i32> groups[]{std::nullopt, 0, 10, 30};
                    const ui64 groupCounts[]{0, 1, 2, 1};
                    for (ui64 i = 0; i < rowCount; ++i) {
                        UNIT_ASSERT(data.TryNextRow());
                        if (index == 0 || index == 5) UNIT_ASSERT(data.ColumnParser("v").GetOptionalInt32() == values[i]);
                        else {
                            if (index == 10) UNIT_ASSERT(data.ColumnParser("value").GetOptionalInt32() == groups[i]);
                            else UNIT_ASSERT_VALUES_EQUAL(data.ColumnParser("id").GetUint64(), i + 1);
                            const auto count = index == 3 ? i : index == 4 ? counts[i] : groupCounts[i];
                            // The legacy optimizer loses empty correlated COUNT groups.
                            // Existing new RBO restores zero, which native must preserve.
                            const auto expected = strategy == 0 && count == 0 ? std::optional<ui64>{} : std::optional<ui64>{count};
                            UNIT_ASSERT_C(data.ColumnParser("n").GetOptionalUint64() == expected,
                                TStringBuilder() << "strategy=" << strategy << ", case=" << index << ", row=" << i);
                        }
                    }
                }
                if (strategy == 1) reference[index] = actual;
                else if (reference.at(index)) {
                    if (strategy == 0 && (index == 3 || index == 4 || index == 10)) UNIT_ASSERT(actual.Columns == reference[index]->Columns);
                    else AssertSame(actual, *reference[index]);
                }
                ++index;
            }
            const auto invalid = session.ExecuteDataQuery(
                "PRAGMA YqlSelect='force'; SELECT (SELECT b.id FROM `/Root/CorrelatedRows` AS b WHERE b.value = a.value) AS invalid FROM `/Root/CorrelatedRows` AS a",
                TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT(!invalid.IsSuccess());
            if (strategy != 0) UNIT_ASSERT_C(invalid.GetIssues().ToString().find("more than one row") != std::string::npos, invalid.GetIssues().ToString());
            for (const auto* sql : {
                "SELECT (SELECT a.value FROM (SELECT 1 AS id) AS a) FROM `/Root/CorrelatedRows` AS a",
                "SELECT (SELECT a.value) FROM `/Root/CorrelatedRows` AS a GROUP BY a.id"}) {
                const auto invalidScope = session.ExecuteDataQuery(TString("PRAGMA YqlSelect='force'; ") + sql,
                    TTxControl::BeginTx().CommitTx()).GetValueSync();
                UNIT_ASSERT_C(!invalidScope.IsSuccess(), sql);
            }
        }
        UNIT_ASSERT_C(failures.empty(), failures);
    }

    Y_UNIT_TEST(ScalarSubqueriesMatchLegacyOnPopulatedTables) {
        struct TCase {
            const char* Sql;
            size_t Rows = 1;
            // Expected pre-existing failures, or explicit native rejection.
            const char* Errors[3] = {};
            std::optional<i64> Total;
        };
        TVector<std::optional<TResultSnapshot>> reference;
        for (const int strategy : {1, 0, 2}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(strategy != 0);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetNativeFrontendMode(strategy == 2 ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED :
                NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            service->SetDefaultLangVer(NYql::GetMaxLangVersion());
            service->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
            TKikimrRunner runner(TKikimrSettings(config).SetWithSampleTables(false));
            auto client = runner.GetTableClient();
            auto session = client.CreateSession().GetValueSync().GetSession();
            const auto schema = session.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/ScalarRows` (id Uint64 NOT NULL, value Int32, PRIMARY KEY(id))").GetValueSync();
            UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
            TValueBuilder rows;
            rows.BeginList();
            for (ui64 id = 1; id <= 3; ++id) rows.AddListItem().BeginStruct()
                .AddMember("id").Uint64(id)
                .AddMember("value").OptionalInt32(id == 2 ? std::nullopt : std::optional<i32>(id * 10))
                .EndStruct();
            rows.EndList();
            const auto inserted = client.BulkUpsert("/Root/ScalarRows", rows.Build()).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
            size_t index = 0;
            for (const auto& test : std::initializer_list<TCase>{
                {"SELECT (SELECT id FROM `/Root/ScalarRows` WHERE id = 1ul) AS first_value, (SELECT id FROM `/Root/ScalarRows` WHERE id = 3ul) AS last_value", 1},
                {"SELECT (SELECT value FROM `/Root/ScalarRows` WHERE id = 99ul) AS empty, (SELECT value FROM `/Root/ScalarRows` WHERE id = 2ul) AS null_value", 1},
                {"SELECT id, value FROM `/Root/ScalarRows` WHERE value > (SELECT AVG(value) FROM `/Root/ScalarRows`) ORDER BY id", 1},
                {"SELECT id, (SELECT MAX(value) FROM `/Root/ScalarRows`) AS maximum FROM `/Root/ScalarRows` ORDER BY id", 3},
                {"SELECT (SELECT (SELECT 1) + 2) AS nested", 1},
                {"$q = SELECT MAX(value) AS v FROM `/Root/ScalarRows`; SELECT (SELECT v FROM $q) AS maximum", 1},
                {"SELECT (SELECT COUNT(*) FROM `/Root/ScalarRows` WHERE id > 99ul) AS zero", 1},
                {"SELECT SUM(value) AS total FROM `/Root/ScalarRows` HAVING SUM(value) > (SELECT MIN(value) FROM `/Root/ScalarRows`)", 1},
                {"SELECT SUM((SELECT MAX(value) FROM `/Root/ScalarRows`)) AS total FROM `/Root/ScalarRows`", 1, {"Unexpected node in results", nullptr, nullptr}, 90},
                {"SELECT id FROM `/Root/ScalarRows` ORDER BY (SELECT MAX(value) FROM `/Root/ScalarRows`), id", 3, {"Unexpected node in results", "typeIt!=typeItems.end()", "outside the native profile"}},
                {"SELECT k, COUNT(*) AS n FROM `/Root/ScalarRows` GROUP BY (SELECT MAX(value) FROM `/Root/ScalarRows`) AS k", 1,
                    {"Translation of the statement to YqlSelect was forced, but unsupported", "Translation of the statement to YqlSelect was forced, but unsupported", "outside the native profile"}},
            }) {
                const TString query = TString("PRAGMA YqlSelect='force'; ") + test.Sql;
                Cerr << "Scalar subquery strategy=" << strategy << ": " << test.Sql << Endl;
                const auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync();
                if (strategy == 1) reference.emplace_back();
                if (const auto* error = test.Errors[strategy]) {
                    UNIT_ASSERT_C(!result.IsSuccess(), test.Sql);
                    UNIT_ASSERT_C(result.GetIssues().ToString().find(error) != std::string::npos, result.GetIssues().ToString());
                    // A pre-existing fatal backend error can close its session.
                    session = client.CreateSession().GetValueSync().GetSession();
                    ++index;
                    continue;
                }
                const auto actual = Snapshot(result);
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), test.Rows);
                if (test.Total) {
                    TResultSetParser values(result.GetResultSet(0));
                    UNIT_ASSERT(values.TryNextRow());
                    UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("total").GetOptionalInt64().value(), *test.Total);
                }
                if (index == 0) {
                    TResultSetParser values(result.GetResultSet(0));
                    UNIT_ASSERT(values.TryNextRow());
                    UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("first_value").GetOptionalUint64().value(), 1);
                    UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("last_value").GetOptionalUint64().value(), 3);
                }
                if (strategy == 1) reference[index] = actual;
                else {
                    UNIT_ASSERT(reference.at(index));
                    AssertSame(actual, *reference.at(index));
                }
                ++index;
            }
            const auto invalid = session.ExecuteDataQuery(
                "PRAGMA YqlSelect='force'; SELECT (SELECT id FROM `/Root/ScalarRows`) AS invalid",
                TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT(!invalid.IsSuccess());
            if (strategy != 0) UNIT_ASSERT_C(invalid.GetIssues().ToString().find("more than one row") != std::string::npos, invalid.GetIssues().ToString());
        }
    }

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
            "$a = 1; $f = ($x)->{RETURN $x + $a;}; $a = 10; SELECT $f(2), $a",
            "$f = ($x)->{RETURN $x + 1;}; $g = ($x)->{RETURN $f($x);}; $f = ($x)->{RETURN $x + 10;}; SELECT $g(2), $f(2)",
            "$q = SELECT 1 AS x; $saved = SELECT x FROM $q; $q = SELECT 2 AS x; SELECT x FROM $saved",
            "$x = 1; $x = $x + 1; SELECT $x",
            "SELECT 1 = NULL, NULL = NULL, NULL IN (1, NULL)",
            "SELECT SUM(x) AS total, COUNT(*) AS n FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q",
            "SELECT a.x, b.x AS matched FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS a LEFT JOIN (SELECT 1 AS x) AS b ON a.x = b.x ORDER BY a.x",
            "SELECT DISTINCT x FROM (SELECT 1 AS x UNION ALL SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q ORDER BY x",
            "SELECT COUNT(DISTINCT x) AS n FROM (SELECT 1 AS x UNION ALL SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q",
            "SELECT k, COUNT(*) AS n FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q GROUP BY x % 2 AS k HAVING COUNT(*) > 0ul ORDER BY k",
            "SELECT x FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) AS q ORDER BY x LIMIT 1 OFFSET 1",
            "SELECT Decimal('1.25',8,2), CAST(1 AS Decimal(8,2)), CAST('1.25' AS Decimal(8,2))",
            "SELECT Decimal('9.5',8,2) BETWEEN 1 AND 10, Decimal('1.25',8,2) = Decimal('1.250',6,3)",
            "SELECT IF(true, Decimal('1.25',8,2), 1L), IF(false, Decimal('1.25',8,2), Decimal('1.251',6,3))",
            "SELECT IF(false, Decimal('1.25',8,2), 18446744073709551615ul)",
            "SELECT Decimal('1.25',8,2) AS x UNION ALL SELECT 1L AS x",

            "SELECT Decimal('1',8,2) / Decimal('0',8,2), Decimal('0',8,2) / Decimal('0',8,2)",
            "SELECT -Decimal('nan',8,2), Abs(Decimal('-inf',8,2)), Decimal('inf',8,2) - Decimal('inf',8,2)",
            "SELECT CAST(Decimal('1.5',8,2) AS Int32), CAST(Decimal('9.9',2,1) AS Int32), CAST(Decimal('1.50',8,2) AS String)",
            "SELECT CAST(Decimal('1.25',8,2) AS Float), CAST(Decimal('1.25',8,2) AS Double), CAST(Decimal('99.9',3,1) AS Decimal(2,1))",
            "SELECT 1 + 2, Length('abc')",
            "SELECT Length(Utf8('hello')), StartsWith('abc', 'a'), EndsWith('abc', 'c'), String::AsciiToLower('ABC'), String::AsciiToUpper('abc')",
            "SELECT -128t, -2147483648, -9223372036854775808, -1u",
            "SELECT 7 / 2, 7 % 2, 1 / 0, 1 % 0",
            "SELECT 7.0 / 2.0, 1.0 / 0.0, 0.0 / 0.0, -1.0 / 0.0, 1.0 / Double('-0')",
            "SELECT Float('1') / Float('0'), Float('0') / Float('0'), IF(false, 1.0) / 2.0",
            "SELECT CAST(42 AS Double), CAST(42 AS Float), CAST(1.5 AS Float), CAST(Float('1.5') AS Double)",
            "SELECT CAST(9223372586610589697ul AS Float), CAST(4611686293305294849L AS Float)",
            "SELECT CAST(Double('-4.9406564584124654e-324') AS Uint64), CAST(Float('-1e-40') AS Uint8)",
            "SELECT CAST(1.5 AS Int32), CAST(127.5 AS Int8), CAST(-0.5 AS Uint64), CAST(Double('nan') AS Int32)",
            "SELECT CAST(42 AS Bool), CAST(0 AS Bool), CAST(true AS Int32), CAST(false AS Double), CAST(Double('nan') AS Bool)",
            "SELECT CAST(42 AS String), CAST(true AS Utf8), CAST('abc' AS Utf8), CAST(Utf8('abc') AS String)",
            "SELECT CAST(' 1.5' AS Double), CAST('0x10' AS Float), CAST('1e9999' AS Double), CAST('-1e-9999' AS Double)",
            "SELECT CAST('TRUE' AS Bool), CAST('yes' AS Bool), CAST('1' AS Bool), CAST('nan' AS Double)",
            "SELECT Substring('abcdef', 2), Substring('abcdef', 2, 3), Substring('abcdef', 99), Substring('abcdef', 0, 0)",
            "SELECT Substring('abcdef', NULL), Substring('abcdef', NULL, 2), Substring('abcdef', 2, NULL), Substring(NULL, 2)",
            "SELECT Substring(IF(false, 'abcdef'), 2), Substring('abcdef', IF(false, 2u), IF(false, 3u))",
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

    Y_UNIT_TEST(RuntimeFloatingCastsAndSubstringMatchLegacy) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        const TString sql = R"(
            DECLARE $x AS Double;
            DECLARE $y AS Double?;
            DECLARE $text AS String?;
            DECLARE $start AS Uint32?;
            DECLARE $count AS Uint32?;
            SELECT $x / $y AS ratio, CAST($x AS Int32) AS integral,
                CAST($x AS Float) AS rounded, Substring($text, $start, $count) AS part
        )";
        for (const double x : {7.5, -0.5, 2147483648.0}) for (const unsigned scenario : {0, 1, 2, 3}) {
            const auto params = TParamsBuilder()
                .AddParam("$x").Double(x).Build()
                .AddParam("$y").OptionalDouble(scenario == 0 ? std::nullopt : std::optional<double>{scenario == 1 ? 0.0 : 2.0}).Build()
                .AddParam("$text").OptionalString(scenario == 3 ? std::nullopt : std::optional<TString>{"abcdef"}).Build()
                .AddParam("$start").OptionalUint32(scenario == 1 ? std::nullopt : std::optional<ui32>{2}).Build()
                .AddParam("$count").OptionalUint32(scenario == 2 ? std::nullopt : std::optional<ui32>{3}).Build()
                .Build();
            AssertSame(Snapshot(native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()),
                Snapshot(legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()));
        }
    }

    Y_UNIT_TEST(RuntimeDecimalMatchesLegacy) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        const TString sql = R"(
            DECLARE $a AS Decimal(8,2);
            DECLARE $b AS Decimal(8,2)?;
            DECLARE $i AS Int64;
            SELECT $a > $i, $a = $i, IF($a > $i, $a, $i), COALESCE($b, CAST($i AS Decimal(8,2))),
                $a + $b, $a - $b, $a * $b, $a / $b, $a % $b,
                $a = $b, $a != $b, $a < $b, $a <= $b, -$a, Abs($a),
                CAST($a AS Int32), CAST($a AS Float), CAST($a AS Double),
                CAST($a AS String), CAST($a AS Decimal(2,1))
        )";
        for (const auto [a, b] : {std::pair{"1.25", "2.00"}, {"999999.99", "999999.99"},
                {"-1.25", "0"}, {"0", "0"}, {"nan", "inf"}, {"inf", "inf"}, {"-inf", "inf"},
                {"1.25", static_cast<const char*>(nullptr)}}) {
            auto builder = TParamsBuilder();
            builder.AddParam("$a").Decimal(TDecimalValue(a, 8, 2)).Build();
            builder.AddParam("$i").Int64(-9007199254740993LL).Build();
            auto& optional = builder.AddParam("$b");
            if (b) optional.BeginOptional().Decimal(TDecimalValue(b, 8, 2)).EndOptional().Build();
            else optional.EmptyOptional(TTypeBuilder().Decimal({8, 2}).Build()).Build();
            const auto params = builder.Build();
            AssertSame(Snapshot(native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()),
                Snapshot(legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()));
        }
    }

    Y_UNIT_TEST(TpchReadQueriesMatchLegacyOnPopulatedColumnTables) {
        const auto legacy = TpchReadQueries(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY, false);
        for (const auto mode : {NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY,
                               NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED}) {
            const auto actual = TpchReadQueries(mode, true);
            UNIT_ASSERT_VALUES_EQUAL(actual.size(), legacy.size());
            for (std::size_t i = 0; i < legacy.size(); ++i) AssertSame(actual[i], legacy[i]);
        }
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
