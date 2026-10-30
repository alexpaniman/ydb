#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <yql/essentials/public/langver/yql_langver.h>
#include <library/cpp/json/json_reader.h>
#include <util/stream/file.h>
#include <util/string/subst.h>

#include <limits>

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
    // Date-dependent queries share actual joined rows, inclusive/exclusive
    // boundaries, failed predicates and NULLs. Each row is a separate order;
    // excluded Q13 comments and Brand#45 keep the earlier cases independent.
    struct TCalendarRow {
        const char* OrderDate;
        const char* ShipDate;
        const char* CommitDate;
        const char* ReceiptDate;
        const char* Priority;
        const char* ShipMode;
        const char* Quantity;
        const char* Discount;
        i64 Customer;
        i64 Supplier;
        i64 Part = 50;
    };
    const TCalendarRow calendarRows[]{
        {"1993-07-01", "1993-07-02", "1993-07-03", "1993-07-04", "1-URGENT", "MAIL", "23", ".06", 103, 3},
        {"1993-09-30", "1993-10-01", "1993-10-02", "1993-10-03", "2-HIGH", "MAIL", "23", ".06", 103, 3},
        {"1993-10-01", "1993-10-02", "1993-10-03", "1993-10-04", "1-URGENT", "MAIL", "23", ".05", 102, 2},
        {"1993-07-02", "1993-07-03", "1993-07-04", "1993-07-04", "2-HIGH", "MAIL", "23", ".06", 103, 3},
        {"1993-06-30", "1993-12-30", "1993-12-31", "1994-01-01", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1993-08-01", "1993-08-02", "1993-08-03", nullptr, "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1993-12-31", "1994-01-01", "1994-01-02", "1994-01-03", "3-MEDIUM", "MAIL", "23", ".07", 102, 2},
        {"1993-10-02", "1993-10-03", "1993-10-04", "1993-10-05", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1994-01-01", "1994-01-01", "1994-01-02", "1994-01-03", "1-URGENT", "MAIL", "23", ".05", 103, 3},
        {"1994-12-31", "1994-12-31", "1995-01-01", "1995-01-02", "3-MEDIUM", "MAIL", "23", ".07", 103, 3},
        {"1995-01-01", "1995-01-01", "1995-01-02", "1995-01-03", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1994-01-02", "1994-01-02", "1994-01-03", "1994-01-04", "2-HIGH", "MAIL", "24", ".06", 103, 3},
        {"1994-01-03", "1994-01-03", "1994-01-04", "1994-01-05", "3-MEDIUM", "MAIL", "23", ".04", 103, 3},
        {"1994-01-04", "1994-01-04", "1994-01-05", "1994-01-06", nullptr, "MAIL", "23", ".08", 103, 3},
        {"1994-01-05", "1994-01-05", "1994-01-06", "1994-01-07", "1-URGENT", "SHIP", nullptr, ".06", 103, 3},
        {"1994-01-06", "1994-01-06", "1994-01-07", "1994-01-08", "3-MEDIUM", "SHIP", "23", nullptr, 103, 3},
        {"1994-01-07", "1994-01-07", nullptr, "1994-01-09", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1994-01-08", "1994-01-08", "1994-01-08", "1994-01-09", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1994-01-09", "1994-01-09", "1994-01-10", "1994-01-10", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1994-12-31", "1995-01-01", "1995-01-02", "1995-01-03", "3-MEDIUM", "MAIL", "23", ".06", 102, 2},
        {"1995-12-31", "1996-01-01", "1996-01-02", "1996-01-03", "3-MEDIUM", "MAIL", "23", ".06", 102, 2},
        {"1996-03-30", "1996-03-31", "1996-04-01", "1996-04-02", "3-MEDIUM", "MAIL", "23", ".06", 104, 1},
        {"1996-12-30", "1996-12-31", "1997-01-01", "1997-01-02", "3-MEDIUM", "MAIL", "23", ".06", 104, 1},
        {"1996-12-31", "1997-01-01", "1997-01-02", "1997-01-03", "3-MEDIUM", "MAIL", "23", ".06", 102, 2},
        {"1996-03-31", "1996-04-01", "1996-04-02", "1996-04-03", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {"1995-12-30", "1995-12-31", "1996-01-01", "1996-01-02", "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        {nullptr, nullptr, nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 103, 3},
        // Q8: Brazilian/non-Brazilian volume in both years, at both order-date boundaries.
        {"1995-01-01", "1995-08-31", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 8},
        {"1995-08-01", "1995-08-31", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 9},
        {"1996-01-01", "1996-07-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 8},
        {"1996-06-01", "1996-07-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 9},
        {"1996-12-31", "1997-01-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 9},
        // Q14: promotional rows at the lower, last included and excluded upper ship dates.
        {"1995-08-31", "1995-09-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 9, 51},
        {"1996-08-30", "1996-08-31", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 9, 51},
        {"1996-08-31", "1996-09-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "23", ".06", 108, 9, 51},
        // Q20: two qualifying parts for one supplier, equality excluded for another,
        // and supplier 11 with no matching lines (NULL aggregate, not zero).
        {"1994-01-01", "1994-01-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "100", ".06", 108, 9},
        {"1994-12-30", "1994-12-31", nullptr, nullptr, "3-MEDIUM", "MAIL", "100", ".06", 108, 9, 51},
        {"1994-07-01", "1994-07-01", nullptr, nullptr, "3-MEDIUM", "MAIL", "200", ".06", 108, 10},
    };
    auto optionalDate = [](const char* text) -> std::optional<TInstant> {
        return text ? std::optional<TInstant>{TInstant::ParseIso8601(TString(text) + "T00:00:00Z")} : std::nullopt;
    };
    auto optionalText = [](const char* text) -> std::optional<std::string> {
        return text ? std::optional<std::string>{text} : std::nullopt;
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
    // Q21: duplicate qualifying late lines; two late suppliers; one supplier
    // only; an unfinished order; a NULL competitor date; and a missing nation.
    // Q18: sums above, equal to, and below 300, including an all-NULL group.
    for (const auto& [order, line, supplier, receipt, quantity] : std::initializer_list<std::tuple<i64, i32, i64, const char*, const char*>>{
            {100, 1, 6, "1999-01-03", "150"}, {100, 2, 6, "1999-01-03", "151"}, {100, 3, 1, "1999-01-02", nullptr},
            {101, 1, 6, "1999-01-03", "150"}, {101, 2, 7, "1999-01-03", "150"},
            {102, 1, 6, "1999-01-03", "151"}, {102, 2, 6, "1999-01-03", "151"},
            {103, 1, 7, "1999-01-03", "149"}, {103, 2, 1, "1999-01-02", "150"},
            {104, 1, 6, "1999-01-03", "301"}, {104, 2, 1, "1999-01-02", nullptr},
            {105, 1, 6, "1999-01-03", nullptr}, {105, 2, 1, nullptr, nullptr},
            {106, 1, 5, "1999-01-03", nullptr}, {106, 2, 6, "1999-01-02", nullptr}}) {
        rows.AddListItem().BeginStruct()
            .AddMember("l_orderkey").Int64(order).AddMember("l_linenumber").Int32(line)
            .AddMember("l_suppkey").OptionalInt64(supplier).AddMember("l_partkey").OptionalInt64(std::nullopt)
            .AddMember("l_commitdate").OptionalDate(TInstant::ParseIso8601("1999-01-02T00:00:00Z"))
            .AddMember("l_receiptdate").OptionalDate(receipt ? std::optional<TInstant>{TInstant::ParseIso8601(TString(receipt) + "T00:00:00Z")} : std::nullopt)
            .AddMember("l_shipdate").OptionalDate(TInstant::ParseIso8601("1999-01-01T00:00:00Z"));
        rows.AddMember("l_quantity");
        if (quantity) rows.BeginOptional().Decimal(TDecimalValue(quantity, 12, 2)).EndOptional();
        else rows.EmptyOptional(decimalType);
        for (const auto* column : {"l_extendedprice", "l_discount", "l_tax"}) rows.AddMember(column).EmptyOptional(decimalType);
        for (const auto* column : {"l_comment", "l_shipinstruct", "l_shipmode", "l_returnflag", "l_linestatus"}) rows.AddMember(column).OptionalUtf8(std::nullopt);
        rows.EndStruct();
    }
    for (std::size_t i = 0; i < std::size(calendarRows); ++i) {
        const auto& row = calendarRows[i];
        rows.AddListItem().BeginStruct()
            .AddMember("l_orderkey").Int64(200 + i).AddMember("l_linenumber").Int32(1)
            .AddMember("l_suppkey").OptionalInt64(row.Supplier).AddMember("l_partkey").OptionalInt64(row.Part)
            .AddMember("l_commitdate").OptionalDate(optionalDate(row.CommitDate))
            .AddMember("l_receiptdate").OptionalDate(optionalDate(row.ReceiptDate))
            .AddMember("l_shipdate").OptionalDate(optionalDate(row.ShipDate))
            .AddMember("l_comment").OptionalUtf8(std::nullopt)
            .AddMember("l_shipinstruct").OptionalUtf8("DELIVER IN PERSON")
            .AddMember("l_shipmode").OptionalUtf8(row.ShipMode)
            .AddMember("l_returnflag").OptionalUtf8("R")
            .AddMember("l_linestatus").OptionalUtf8("F");
        for (const auto [name, value] : {std::pair{"l_quantity", row.Quantity}, {"l_extendedprice", "100"},
                {"l_discount", row.Discount}, {"l_tax", "0"}}) {
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
    // Q22 positive in-country balances average 180. Customers 105 and 113
    // exceed it without orders; 106 exceeds it but has orders. NULLs, zero,
    // negative balances and excluded phone prefixes must not skew that mean.
    for (const auto& [id, phone, balance] : std::initializer_list<std::tuple<i64, const char*, const char*>>{
            {101, nullptr, nullptr}, {102, nullptr, nullptr}, {103, nullptr, nullptr},
            {104, "131234", "100"}, {105, "139876", "200"}, {106, "311234", "300"},
            {107, "231234", "50"}, {108, "991234", "1000"}, {109, nullptr, "100"},
            {110, "311234", nullptr}, {111, "311234", "-10"}, {112, "131234", "0"}, {113, "319876", "250"}}) {
        customers.AddListItem().BeginStruct()
            .AddMember("c_custkey").Int64(id)
            .AddMember("c_mktsegment").OptionalUtf8(id == 103 ? std::nullopt :
                std::optional<std::string>{id == 101 ? "BUILDING" : "AUTOMOBILE"})
            .AddMember("c_acctbal");
        if (balance) customers.BeginOptional().Decimal(TDecimalValue(balance, 12, 2)).EndOptional();
        else customers.EmptyOptional(decimalType);
        customers.AddMember("c_address").OptionalUtf8(std::nullopt)
            .AddMember("c_comment").OptionalUtf8(std::nullopt)
            .AddMember("c_name").OptionalUtf8(id == 106 ? std::optional<std::string>{"Customer106"} : std::nullopt)
            .AddMember("c_nationkey").OptionalInt32(id == 102 ? std::optional<i32>{1} : id == 103 ?
                std::optional<i32>{3} : id == 104 ? std::optional<i32>{2} : id == 108 ? std::optional<i32>{5} : std::nullopt)
            .AddMember("c_phone").OptionalUtf8(phone ? std::optional<std::string>{phone} : std::nullopt)
            .EndStruct();
    }
    customers.EndList();
    orders.BeginList();
    for (const i64 id : {1, 2, 3, 4, 5, 6, 8, 99, 100, 101, 102, 103, 104, 105, 106}) {
        orders.AddListItem().BeginStruct()
            .AddMember("o_orderkey").Int64(id)
            .AddMember("o_custkey").OptionalInt64(id >= 100 ? 106 : id == 4 ? 102 : id == 5 ? 999 : id == 6 ? 103 : 101)
            .AddMember("o_orderdate").OptionalDate(id == 8 ? std::nullopt :
                std::optional<TInstant>{TInstant::ParseIso8601(id == 102 ? "1999-01-02T00:00:00Z" : id >= 100 ? "1999-01-01T00:00:00Z" : id == 3 ? "1995-03-15T00:00:00Z" : "1995-03-14T00:00:00Z")})
            .AddMember("o_shippriority").OptionalInt32(0)
            .AddMember("o_clerk").OptionalUtf8(std::nullopt)
            .AddMember("o_comment").OptionalUtf8(id == 6 ? std::nullopt : std::optional<std::string>{
                id == 1 ? "special big requests" : id == 4 ? "specialrequests" : id == 8 ? "special requests special" :
                id == 99 ? "special and requests" : id == 3 ? "requests before special" : "ordinary"})
            .AddMember("o_orderpriority").OptionalUtf8(std::nullopt)
            .AddMember("o_orderstatus").OptionalUtf8(id < 100 ? std::nullopt : std::optional<std::string>{id == 104 ? "O" : "F"})
            .AddMember("o_totalprice");
        if (id == 100 || id == 102 || id == 104) orders.BeginOptional().Decimal(TDecimalValue(id == 104 ? "500" : "900", 12, 2)).EndOptional();
        else orders.EmptyOptional(decimalType);
        orders.EndStruct();
    }
    for (std::size_t i = 0; i < std::size(calendarRows); ++i) {
        const auto& row = calendarRows[i];
        orders.AddListItem().BeginStruct()
            .AddMember("o_orderkey").Int64(200 + i).AddMember("o_custkey").OptionalInt64(row.Customer)
            .AddMember("o_orderdate").OptionalDate(optionalDate(row.OrderDate))
            .AddMember("o_shippriority").OptionalInt32(0)
            .AddMember("o_clerk").OptionalUtf8(std::nullopt)
            .AddMember("o_comment").OptionalUtf8("special requests")
            .AddMember("o_orderpriority").OptionalUtf8(optionalText(row.Priority))
            .AddMember("o_orderstatus").OptionalUtf8(std::nullopt)
            .AddMember("o_totalprice").EmptyOptional(decimalType).EndStruct();
    }
    orders.EndList();
    parts.BeginList();
    for (const i64 id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 50, 51}) {
        parts.AddListItem().BeginStruct()
            .AddMember("p_partkey").Int64(id)
            .AddMember("p_brand").OptionalUtf8(id == 1 ? "Brand#12" : id == 2 ? "Brand#23" : (id == 11 || id >= 50) ? "Brand#45" : "Brand#34")
            .AddMember("p_container").OptionalUtf8(id == 1 ? "SM BOX" : id == 2 ? "MED BOX" : "LG BOX")
            .AddMember("p_size").OptionalInt32(id < 5 || id >= 50 ? 3 : id == 7 || id >= 9 ? 14 : 15)
            .AddMember("p_comment").OptionalUtf8(std::nullopt)
            .AddMember("p_mfgr").OptionalUtf8("Maker")
            .AddMember("p_name").OptionalUtf8(id == 50 ? std::optional<std::string>{"forest green"} : id == 51 ? std::optional<std::string>{"forest brown"} : std::nullopt)
            .AddMember("p_retailprice").EmptyOptional(decimalType)
            .AddMember("p_type").OptionalUtf8(id < 5 ? std::nullopt : std::optional<std::string>{id == 50 ? "ECONOMY ANODIZED STEEL" : id == 51 ? "PROMO NICKEL" : id == 10 ? "MEDIUM POLISHED BRASS" : id == 8 || id == 12 ? "LARGE TIN" : "LARGE BRASS"})
            .EndStruct();
    }
    parts.EndList();
    TValueBuilder regions, nations, suppliers, partsupp;
    regions.BeginList();
    for (const i32 id : {1, 2, 3}) regions.AddListItem().BeginStruct()
        .AddMember("r_regionkey").Int32(id)
        .AddMember("r_name").OptionalUtf8(id == 1 ? "EUROPE" : id == 2 ? "ASIA" : "AMERICA")
        .AddMember("r_comment").OptionalUtf8(std::nullopt).EndStruct();
    regions.EndList();
    nations.BeginList();
    for (const i32 id : {1, 2, 3, 4, 5, 6}) nations.AddListItem().BeginStruct()
        .AddMember("n_nationkey").Int32(id)
        .AddMember("n_name").OptionalUtf8(id == 1 ? "GERMANY" : id == 2 ? "FRANCE" : id == 3 ? "JAPAN" : id == 4 ? "SAUDI ARABIA" : id == 5 ? "BRAZIL" : "CANADA")
        .AddMember("n_regionkey").OptionalInt32(id >= 5 ? 3 : id >= 3 ? 2 : 1)
        .AddMember("n_comment").OptionalUtf8(std::nullopt).EndStruct();
    nations.EndList();
    suppliers.BeginList();
    for (const i64 id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) suppliers.AddListItem().BeginStruct()
        .AddMember("s_suppkey").Int64(id)
        .AddMember("s_name").OptionalUtf8("Supplier" + std::to_string(id))
        .AddMember("s_nationkey").OptionalInt32(id == 5 ? std::nullopt : std::optional<i32>{id >= 9 ? 6 : id == 8 ? 5 : id >= 6 ? 4 : id == 4 ? 1 : i32(id)})
        .AddMember("s_acctbal").BeginOptional().Decimal(TDecimalValue(id == 2 ? "200" : id == 3 ? "999" : "100", 12, 2)).EndOptional()
        .AddMember("s_address").OptionalUtf8("Address")
        .AddMember("s_phone").OptionalUtf8("Phone")
        .AddMember("s_comment").OptionalUtf8(id == 1 || id == 5 ? std::nullopt : std::optional<std::string>{
            id == 3 ? "Customer repeated Complaints" : id == 6 ? "CustomerComplaints" : id == 4 ? "Complaints Customer" : "ordinary"}).EndStruct();
    suppliers.EndList();
    partsupp.BeginList();
    // Tied European minima for part 5; lower Asian/missing-nation costs must
    // not affect that minimum. Part 6 has a NULL competing cost. Parts 7/8
    // fail size/type filters even though their supply costs qualify.
    for (const auto& [part, supplier, cost] : std::initializer_list<std::tuple<i64, i64, const char*>>{
            {5, 1, "10"}, {5, 2, "10"}, {5, 3, "1"}, {5, 4, "11"}, {5, 5, "0"},
            {6, 1, "20"}, {6, 2, nullptr}, {7, 1, "1"}, {8, 1, "1"},
            // Q16: distinct suppliers across parts, excluded complaint suppliers,
            // a reversed comment pattern, and filtered brand/type/size values.
            {7, 2, "9"}, {7, 3, "8"}, {9, 1, "9"}, {9, 4, "10"}, {9, 6, "10"},
            {10, 1, "10"}, {11, 1, "10"}, {12, 2, "10"}, {50, 1, "2"}, {50, 2, "2"}, {50, 3, "2"},
            {50, 8, "2"}, {50, 9, "2"}, {50, 10, "2"}, {50, 11, "2"}, {51, 9, "2"},
            // Q11 must exclude NULL, zero and negative grouped values.
            {1, 1, nullptr}, {3, 1, "-1"}, {4, 1, "0"}}) {
        partsupp.AddListItem().BeginStruct()
            .AddMember("ps_partkey").Int64(part).AddMember("ps_suppkey").Int64(supplier)
            // Distinct positive totals make Q11's ORDER BY value deterministic.
            .AddMember("ps_availqty").OptionalInt32(part == 8 || part == 11 ? 99 : 100).AddMember("ps_comment").OptionalUtf8(std::nullopt)
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
    for (const auto [query, expectedRows] : {std::pair{1, 4}, {2, 3}, {3, 2}, {4, 2}, {5, 1}, {6, 1}, {7, 3}, {8, 2}, {9, 15}, {10, 2},
            {11, 8}, {12, 2}, {13, 4}, {14, 1}, {15, 2}, {16, 2}, {17, 1}, {18, 3}, {19, 1}, {20, 1}, {21, 2}, {22, 2}}) {
        Cerr << "TPC-H Q" << query << ", native mode " << int(mode) << ", new RBO " << newRbo << Endl;
        const auto sql = prelude + TFileInput(SRC_(TStringBuilder() << "data/yql-tpch/q" << query << ".yql")).ReadAll();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetResultSet(0).RowsCount(), expectedRows, SnapshotRows(result.GetResultSet(0)).Values);
        if (query == 2) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto [part, supplier] : {std::pair{5, "Supplier2"}, {5, "Supplier1"}, {6, "Supplier1"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("p_partkey").GetInt64(), part);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("s_name").GetOptionalUtf8().value(), supplier);
            }
        }
        if (query == 4) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto* priority : {"1-URGENT", "2-HIGH"}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("o_orderpriority").GetOptionalUtf8().value(), priority);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("order_count").GetUint64(), 1);
            }
        }
        if (query == 5 || query == 6) {
            TResultSetParser values(result.GetResultSet(0));
            UNIT_ASSERT(values.TryNextRow());
            if (query == 5) UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("n_name").GetOptionalUtf8().value(), "JAPAN");
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("revenue").GetOptionalDecimal()->ToString(), query == 5 ? "846" : "37");
        }
        if (query == 7) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [supplier, customer, year, revenue] : std::initializer_list<std::tuple<const char*, const char*, ui16, const char*>>{
                    {"FRANCE", "GERMANY", 1995, "94"}, {"FRANCE", "GERMANY", 1996, "94"}, {"GERMANY", "FRANCE", 1996, "188"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("supp_nation").GetOptionalUtf8().value(), supplier);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("cust_nation").GetOptionalUtf8().value(), customer);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("l_year").GetOptionalUint16().value(), year);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("revenue").GetOptionalDecimal()->ToString(), revenue);
            }
        }
        if (query == 8) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [year, share] : {std::pair{1995, "0.5"}, {1996, "0.33"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("o_year").GetOptionalUint16().value(), year);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("mkt_share").GetOptionalDecimal()->ToString(), share);
            }
        }
        if (query == 9) {
            TResultSetParser values(result.GetResultSet(0));
            THashMap<TString, TString> expected{{"BRAZIL/1995", "48"}, {"BRAZIL/1996", "48"},
                {"CANADA/1994", "-412"}, {"CANADA/1995", "48"}, {"CANADA/1996", "96"}, {"FRANCE/1996", "48"}, {"FRANCE/1995", "48"}, {"FRANCE/1994", "48"},
                {"FRANCE/1993", "96"}, {"GERMANY/1996", "96"}, {"JAPAN/1996", "48"}, {"JAPAN/1995", "96"},
                {"JAPAN/1994", "382"}, {"JAPAN/1993", "288"}, {"JAPAN/null", "48"}};
            while (values.TryNextRow()) {
                const auto year = values.ColumnParser("o_year").GetOptionalUint16();
                const TString key = TString(values.ColumnParser("nation").GetOptionalUtf8().value()) + "/" + (year ? ToString(*year) : "null");
                UNIT_ASSERT_C(expected.contains(key), key);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("sum_profit").GetOptionalDecimal()->ToString(), expected.at(key));
                expected.erase(key);
            }
            UNIT_ASSERT(expected.empty());
        }
        if (query == 10) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [customer, revenue] : {std::pair{102, "188"}, {103, "94"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("c_custkey").GetInt64(), customer);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("revenue").GetOptionalDecimal()->ToString(), revenue);
            }
        }
        if (query == 11) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [part, value] : {std::pair{5, "2100"}, {6, "2000"}, {9, "1900"}, {10, "1000"},
                    {11, "990"}, {50, "200"}, {7, "100"}, {8, "99"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("ps_partkey").GetInt64(), part);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("value").GetOptionalDecimal()->ToString(), value);
            }
        }
        if (query == 12) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [mode, high, low] : {std::tuple{"MAIL", 2, 3}, {"SHIP", 1, 1}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("l_shipmode").GetOptionalUtf8().value(), mode);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("high_line_count").GetInt64(), high);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("low_line_count").GetInt64(), low);
            }
        }
        if (query == 14) {
            TResultSetParser values(result.GetResultSet(0));
            UNIT_ASSERT(values.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("promo_revenue").GetOptionalDecimal()->ToString(), "25");
        }
        if (query == 15) {
            TResultSetParser values(result.GetResultSet(0));
            for (const i64 supplier : {1, 2}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("s_suppkey").GetInt64(), supplier);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("total_revenue").GetOptionalDecimal()->ToString(), "94");
            }
        }
        if (query == 13) {
            TResultSetParser values(result.GetResultSet(0));
            // YQL's complex LIKE returns false for NULL, so customer 103's
            // NULL-comment order satisfies NOT LIKE and contributes one.
            for (const auto& [orders, customers] : {std::pair{0, 10}, {7, 1}, {2, 1}, {1, 1}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("c_count").GetUint64(), orders);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("custdist").GetUint64(), customers);
            }
        }
        if (query == 16) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [type, suppliers] : {std::pair{"LARGE BRASS", 3}, {"LARGE TIN", 1}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("p_brand").GetOptionalUtf8().value(), "Brand#34");
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("p_type").GetOptionalUtf8().value(), type);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("p_size").GetOptionalInt32().value(), 14);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("supplier_cnt").GetUint64(), suppliers);
            }
        }
        if (query == 17) {
            TResultSetParser values(result.GetResultSet(0));
            UNIT_ASSERT(values.TryNextRow());
            const auto average = values.ColumnParser("avg_yearly").GetOptionalDecimal();
            UNIT_ASSERT(average);
            UNIT_ASSERT_VALUES_EQUAL(average->ToString(), "1.43");
        }
        if (query == 18) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [order, quantity] : {std::pair{100, "301"}, {102, "302"}, {104, "301"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("c_name").GetOptionalUtf8().value(), "Customer106");
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("c_custkey").GetInt64(), 106);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("o_orderkey").GetInt64(), order);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("o_totalprice").GetOptionalDecimal()->ToString(), order == 104 ? "500" : "900");
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser(5).GetOptionalDecimal()->ToString(), quantity);
            }
        }
        if (query == 20) {
            TResultSetParser values(result.GetResultSet(0));
            UNIT_ASSERT(values.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("s_name").GetOptionalUtf8().value(), "Supplier9");
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("s_address").GetOptionalUtf8().value(), "Address");
        }
        if (query == 21) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [supplier, count] : {std::pair{"Supplier6", 3}, {"Supplier7", 1}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("s_name").GetOptionalUtf8().value(), supplier);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("numwait").GetUint64(), count);
            }
        }
        if (query == 22) {
            TResultSetParser values(result.GetResultSet(0));
            for (const auto& [country, total] : {std::pair{"13", "200"}, {"31", "250"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("cntrycode").GetOptionalString().value(), country);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("numcust").GetUint64(), 1);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("totacctbal").GetOptionalDecimal()->ToString(), total);
            }
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
            if (query != 1 && query != 6) UNIT_ASSERT_C(total > 0, plan);
        }
        snapshots.push_back(SnapshotRows(result.GetResultSet(0)));
    }
    return snapshots;
}

TVector<TResultSnapshot> TpcdsReadQueries(TMode mode, bool newRbo) {
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
    auto schema = TFileInput(SRC_("data/schema/tpcds.sql")).ReadAll();
    SubstGlobal(schema, "\n);", "\n) WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 16);");
    const auto created = session.ExecuteSchemeQuery(schema).GetValueSync();
    UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
    auto load = [&](const char* table, TValueBuilder& rows) {
        rows.EndList();
        const TString path = TString("/Root/test/ds/") + table;
        const auto described = session.DescribeTable(path).GetValueSync();
        UNIT_ASSERT_C(described.IsSuccess(), described.GetIssues().ToString());
        auto values = rows.Build();
        auto& rowType = *values.GetType().GetProto().mutable_list_type()->mutable_item()->mutable_struct_type();
        // Column-table bulk upserts require every column. Keep the original
        // benchmark schema and explicitly fill unused nullable fields.
        for (const auto& column : described.GetTableDescription().GetColumns()) {
            if (std::any_of(rowType.members().begin(), rowType.members().end(),
                    [&](const auto& member) { return member.name() == column.Name; })) continue;
            UNIT_ASSERT_C(column.Type.GetProto().has_optional_type(), column.Name);
            auto* member = rowType.add_members();
            member->set_name(column.Name);
            *member->mutable_type() = column.Type.GetProto();
            for (auto& row : *values.GetProto().mutable_items()) {
                row.add_items()->set_null_flag_value(google::protobuf::NULL_VALUE);
            }
        }
        const auto result = client.BulkUpsert(path, std::move(values)).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    };
    auto itemName = [](i64 id) { return "Item" + std::to_string(1000 + id).substr(1); };
    const auto decimalType = TTypeBuilder().Decimal({7, 2}).Build();
    auto decimal = [&](TValueBuilder& builder, const char* text) {
        if (text) builder.BeginOptional().Decimal(TDecimalValue(text, 7, 2)).EndOptional();
        else builder.EmptyOptional(decimalType);
    };
    TValueBuilder dates;
    dates.BeginList();
    for (const auto& [id, text] : std::initializer_list<std::pair<i64, const char*>>{
            {1, "2001-06-02"}, {2, "2001-08-01"}, {3, "2001-08-02"}, {4, nullptr}, {5, "2001-2-30"}, {6, "2001-06-01"}, {7, "2001-7-1"},
            {10, "2002-05-30"}, {11, "2002-07-29"}, {12, "2002-07-30"}, {13, "2002-05-29"}, {14, "2002-6-1"},
            {20, "1998-03-19"}, {21, "1998-04-07"}, {22, "1998-04-08"}, {23, "1998-04-28"}, {24, "1998-03-18"}, {25, "1998-04-29"},
            {30, "1998-06-16"}, {31, "1998-06-17"}, {32, "1998-03-17"}, {33, "1998-5-1"},
            {40, "1998-03-09"}, {41, "1998-05-08"}, {42, "1998-03-08"}, {43, "1998-05-09"}}) {
        dates.AddListItem().BeginStruct().AddMember("d_date_sk").Int64(id)
            .AddMember("d_date_id").Utf8("Date" + std::to_string(id))
            .AddMember("d_date").OptionalUtf8(text ? std::optional<std::string>{text} : std::nullopt).EndStruct();
    }
    load("date_dim", dates);
    TValueBuilder items;
    items.BeginList();
    for (const auto& [id, price, manufacturer] : std::initializer_list<std::tuple<i64, const char*, i64>>{
            {1, "22", 678}, {2, "52", 964}, {3, "35", 918}, {4, "21.99", 678}, {5, "52.01", 849}, {6, "35", 999},
            {7, "35", 849}, {8, "35", 849}, {9, "35", 849}, {10, "35", 849}, {11, "35", 849}, {12, "35", 849},
            {13, "35", 849}, {14, nullptr, 849}, {15, "30", 849},
            {21, "30", 437}, {22, "60", 129}, {23, "43", 727}, {24, "29.99", 437}, {25, "60.01", 663}, {26, "43", 999},
            {27, "43", 663}, {28, "43", 663}, {29, "43", 663}, {30, "43", 663}, {31, "43", 663}, {32, "43", 663},
            {33, "43", 663}, {34, nullptr, 663}, {35, "38", 663},
            {101, "1", 0}, {102, "1.49", 0}, {103, "1", 0}, {104, "1", 0}, {105, "1", 0}, {106, "0.98", 0}, {107, "1.50", 0}, {108, "1", 0},
            {300, "10", 269}, {301, "20", 269}, {302, "25", 269}, {303, "25", 999}, {304, "25", 269}, {305, "25", 269}}) {
        items.AddListItem().BeginStruct().AddMember("i_item_sk").Int64(id).AddMember("i_item_id").Utf8(itemName(id))
            .AddMember("i_item_desc").OptionalUtf8("Description" + std::to_string(id))
            .AddMember("i_manufact_id").OptionalInt64(manufacturer).AddMember("i_current_price");
        decimal(items, price);
        items.EndStruct();
    }
    load("item", items);
    TValueBuilder warehouse;
    warehouse.BeginList().AddListItem().BeginStruct().AddMember("w_warehouse_sk").Int64(1)
        .AddMember("w_warehouse_id").Utf8("WH001").AddMember("w_warehouse_name").OptionalUtf8("Warehouse A")
        .AddMember("w_state").OptionalUtf8("CA").EndStruct();
    load("warehouse", warehouse);
    TValueBuilder inventory;
    inventory.BeginList();
    // Q37/Q82: both date/price/quantity boundaries, invalid and NULL dates,
    // missing sales, duplicate sales, and wrong manufacturers.
    for (const auto& [date, item, quantity] : std::initializer_list<std::tuple<i64, i64, std::optional<i64>>>{
            {1, 1, 100}, {2, 2, 500}, {3, 3, 200}, {1, 4, 200}, {1, 5, 200}, {1, 6, 200},
            {1, 7, 99}, {1, 8, 501}, {1, 9, std::nullopt}, {4, 10, 200}, {5, 11, 200}, {1, 12, 200},
            {6, 13, 200}, {1, 14, 200}, {7, 15, 200},
            {10, 21, 100}, {11, 22, 500}, {12, 23, 200}, {10, 24, 200}, {10, 25, 200}, {10, 26, 200},
            {10, 27, 99}, {10, 28, 501}, {10, 29, std::nullopt}, {4, 30, 200}, {5, 31, 200}, {10, 32, 200},
            {13, 33, 200}, {10, 34, 200}, {14, 35, 200},
            // Q21: inventory on both sides of the pivot and inclusive bounds.
            {20, 101, 10}, {21, 101, 1}, {22, 101, 1}, {23, 101, 10}, {24, 101, 999}, {25, 101, 999}, {4, 101, 999}, {5, 101, 999},
            {20, 102, 10}, {23, 102, 20}, {20, 103, 10}, {23, 103, 5}, {20, 104, 0}, {23, 104, 10},
            {20, 105, std::nullopt}, {23, 105, 10}, {20, 106, 10}, {23, 106, 10}, {20, 107, 10}, {23, 107, 10},
            {20, 108, 10}, {23, 108, 10}}) {
        inventory.AddListItem().BeginStruct().AddMember("inv_date_sk").Int64(date).AddMember("inv_item_sk").Int64(item)
            .AddMember("inv_warehouse_sk").Int64(item == 108 ? 999 : 1)
            .AddMember("inv_quantity_on_hand").OptionalInt64(quantity).EndStruct();
    }
    load("inventory", inventory);
    struct TSale {
        i64 Item;
        i64 Order;
        i64 Date;
        i64 Warehouse;
        const char* Price;
        const char* Discount;
    };
    TVector<TSale> catalog, store, web;
    for (i64 item = 1; item <= 15; ++item) {
        if (item == 12) continue;
        catalog.push_back({item, item * 10, 1, 1, "1", nullptr});
        store.push_back({item + 20, item * 10, 10, 1, "1", nullptr});
    }
    catalog.push_back({2, 21, 1, 1, "1", nullptr});
    store.push_back({22, 21, 10, 1, "1", nullptr});
    i64 order = 3000;
    // Q32/Q92: correlated averages, strict threshold equality, all-NULL groups,
    // excluded manufacturers, both date bounds and dates outside either end.
    for (const auto& [item, date, catalogDiscount, webDiscount] : std::initializer_list<std::tuple<i64, i64, const char*, const char*>>{
            {300, 24, "1", "2"}, {300, 33, "1", "2"}, {300, 30, "10", "20"}, {300, 24, nullptr, nullptr},
            {300, 31, "999", "1998"}, {300, 32, "999", "1998"}, {300, 4, "999", "1998"}, {300, 5, "999", "1998"},
            {301, 24, "2", "4"}, {301, 33, "2", "4"}, {301, 30, "2", "4"}, {302, 24, nullptr, nullptr}, {303, 24, "100", "200"},
            {304, 24, "2", "4"}, {304, 30, "8", "16"}, {305, 24, "3.5", "7"}, {305, 30, "6.5", "13"}}) {
        catalog.push_back({item, order, date, 1, nullptr, catalogDiscount});
        web.push_back({item, order, date, 1, nullptr, webDiscount});
        ++order;
    }
    // Q40: returns, missing returns, NULL refund/price, negative net sales,
    // inclusive dates, excluded dates and a missing warehouse.
    for (const TSale row : {TSale{101, 4001, 40, 1, "100", nullptr}, {101, 4002, 21, 1, "50", nullptr},
            {101, 4003, 22, 1, "200", nullptr}, {101, 4004, 41, 1, "100", nullptr},
            {101, 4005, 42, 1, "999", nullptr}, {101, 4006, 43, 1, "999", nullptr},
            {101, 4007, 4, 1, "999", nullptr}, {101, 4008, 5, 1, "999", nullptr}, {101, 4009, 22, 999, "999", nullptr},
            {102, 4010, 21, 1, "40", nullptr}, {102, 4011, 22, 1, nullptr, nullptr}, {104, 4012, 21, 1, nullptr, nullptr},
            {106, 4013, 21, 1, "100", nullptr}, {107, 4014, 21, 1, "100", nullptr}}) catalog.push_back(row);
    auto loadSales = [&](const char* table, const std::string& prefix, const char* number, const TVector<TSale>& data) {
        TValueBuilder rows;
        rows.BeginList();
        for (const auto& row : data) {
            rows.AddListItem().BeginStruct().AddMember(prefix + "item_sk").Int64(row.Item)
                .AddMember(number).Int64(row.Order).AddMember(prefix + "sold_date_sk").OptionalInt64(row.Date);
            if (prefix == "cs_") rows.AddMember("cs_warehouse_sk").OptionalInt64(row.Warehouse);
            rows.AddMember(prefix + "sales_price"); decimal(rows, row.Price);
            rows.AddMember(prefix + "ext_discount_amt"); decimal(rows, row.Discount);
            rows.EndStruct();
        }
        load(table, rows);
    };
    loadSales("catalog_sales", "cs_", "cs_order_number", catalog);
    loadSales("store_sales", "ss_", "ss_ticket_number", store);
    loadSales("web_sales", "ws_", "ws_order_number", web);
    TValueBuilder returns;
    returns.BeginList();
    for (const auto& [item, order, refund] : std::initializer_list<std::tuple<i64, i64, const char*>>{
            {101, 4001, "20"}, {101, 4003, nullptr}, {101, 4004, "25"}, {102, 4010, "50"}, {102, 4011, "10"}}) {
        returns.AddListItem().BeginStruct().AddMember("cr_item_sk").Int64(item).AddMember("cr_order_number").Int64(order)
            .AddMember("cr_refunded_cash");
        decimal(returns, refund);
        returns.EndStruct();
    }
    load("catalog_returns", returns);
    const TString prelude = R"($to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };
$to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };
$round = ($x,$y) -> {return $x;};
)";
    TVector<TResultSnapshot> snapshots;
    for (const auto [query, expectedRows] : {std::pair{21, 1}, {32, 1}, {37, 3}, {40, 3}, {82, 3}, {92, 1}}) {
        Cerr << "TPC-DS Q" << query << ", native mode " << int(mode) << ", new RBO " << newRbo << Endl;
        const auto sql = prelude + TFileInput(SRC_(TStringBuilder() << "data/yql-tpcds/q" << query << ".yql")).ReadAll();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetResultSet(0).RowsCount(), expectedRows, SnapshotRows(result.GetResultSet(0)).Values);
        TResultSetParser values(result.GetResultSet(0));
        if (query == 21) {
            const TVector<TString> columnNames = newRbo ?
                TVector<TString>{"w_warehouse_name", "i_item_id", "inv_before", "inv_after"} :
                TVector<TString>{"i_item_id", "inv_after", "inv_before", "w_warehouse_name"};
            const auto& columns = result.GetResultSet(0).GetColumnsMeta();
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), columnNames.size());
            for (std::size_t i = 0; i < columnNames.size(); ++i) UNIT_ASSERT_VALUES_EQUAL(columns[i].Name, columnNames[i]);
            UNIT_ASSERT(values.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("w_warehouse_name").GetOptionalUtf8().value(), "Warehouse A");
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("i_item_id").GetUtf8(), "Item101");
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("inv_before").GetOptionalInt64().value(), 11);
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("inv_after").GetOptionalInt64().value(), 11);
        }
        if (query == 32 || query == 92) {
            UNIT_ASSERT(values.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser(0).GetOptionalDecimal()->ToString(), query == 32 ? "18" : "36");
        }
        if (query == 37 || query == 82) {
            for (const auto& [item, price] : query == 37 ? TVector<std::pair<i64, const char*>>{{1, "22"}, {2, "52"}, {15, "30"}} :
                    TVector<std::pair<i64, const char*>>{{21, "30"}, {22, "60"}, {35, "38"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("i_item_id").GetUtf8(), itemName(item));
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("i_item_desc").GetOptionalUtf8().value(), "Description" + std::to_string(item));
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("i_current_price").GetOptionalDecimal()->ToString(), price);
            }
        }
        if (query == 40) {
            for (const auto& [item, before, after] : std::initializer_list<std::tuple<i64, const char*, const char*>>{
                    {101, "130", "275"}, {102, "-10", "0"}, {104, nullptr, "0"}}) {
                UNIT_ASSERT(values.TryNextRow());
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("w_state").GetOptionalUtf8().value(), "CA");
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("i_item_id").GetUtf8(), itemName(item));
                const auto actualBefore = values.ColumnParser("sales_before").GetOptionalDecimal();
                if (before) UNIT_ASSERT_VALUES_EQUAL(actualBefore->ToString(), before);
                else UNIT_ASSERT(!actualBefore);
                UNIT_ASSERT_VALUES_EQUAL(values.ColumnParser("sales_after").GetOptionalDecimal()->ToString(), after);
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
            UNIT_ASSERT_C(total > 0, plan);
            UNIT_ASSERT_VALUES_EQUAL_C(stats.at("CBOTreesOptimized").GetUIntegerSafe(), total, plan);
        }
        snapshots.push_back(SnapshotRows(result.GetResultSet(0)));
    }
    return snapshots;
}

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeHost) {
    Y_UNIT_TEST_TWIN(InSubqueriesMatchLegacyOnPopulatedTables, EqualNullsJoinKeys) {
        using TBool = std::optional<bool>;
        const TBool n = std::nullopt;
        struct TCase {
            const char* Expression;
            TVector<TBool> Ansi;
            TVector<TBool> Legacy;
        };
        const TVector<TCase> cases{
            {"a.id IN (SELECT b.id FROM `/Root/InRows` AS b WHERE b.id < 3ul)", {true, true, false, false, false, false}, {}},
            {"a.value IN (SELECT b.value FROM `/Root/InRows` AS b WHERE b.id <= 2ul)", {true, n, n, true, n, n}, {true, n, false, true, n, false}},
            {"a.value NOT IN (SELECT b.value FROM `/Root/InRows` AS b WHERE b.id <= 2ul)", {false, n, n, false, n, n}, {false, n, true, false, n, true}},
            {"a.value IN (SELECT b.value FROM `/Root/InRows` AS b WHERE b.id = 99ul)", {false, false, false, false, false, false}, {false, n, false, false, n, false}},
            {"a.value IN (SELECT b.value FROM `/Root/InRows` AS b WHERE b.id < a.id)", {false, n, n, true, n, n}, {false, n, false, true, n, false}},
            {"CAST(a.id AS Int32) IN (SELECT b.value FROM `/Root/InRows` AS b)", {n, n, n, n, n, n}, {false, false, false, false, false, false}},
            {"a.value + 1 IN (SELECT b.value + 1 FROM `/Root/InRows` AS b WHERE b.id <= 2ul)", {true, n, n, true, n, n}, {true, n, false, true, n, false}},
            {"(a.value IN (SELECT b.value FROM `/Root/InRows` AS b WHERE b.id <= 2ul)) IN (SELECT true)", {true, n, n, true, n, n}, {true, n, false, true, n, false}},
            {"a.value IN (SELECT CAST(b.value AS Int64) FROM `/Root/InRows` AS b WHERE b.id <= 2ul)", {true, n, n, true, n, n}, {true, n, false, true, n, false}},
            {"CAST(a.value AS Decimal(8,2)) IN (SELECT CAST(b.value AS Decimal(10,3)) FROM `/Root/InRows` AS b WHERE b.id <= 2ul)", {true, n, n, true, n, n}, {true, n, false, true, n, false}},
            {"NULL IN (SELECT b.value FROM `/Root/InRows` AS b WHERE b.id = 99ul)", {false, false, false, false, false, false}, {n, n, n, n, n, n}},
            {"a.value IN (10, NULL)", {true, n, n, true, n, n}, {true, n, false, true, n, false}},
            {"CAST(a.id AS String) IN (SELECT CAST(b.id AS Utf8) FROM `/Root/InRows` AS b WHERE b.id < 3ul)", {true, true, false, false, false, false}, {}},
        };
        TVector<std::optional<TResultSnapshot>> reference(cases.size() * 8);
        for (const int strategy : {0, 2, 1}) {
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
                "CREATE TABLE `/Root/InRows` (id Uint64 NOT NULL, value Int32, PRIMARY KEY(id))").GetValueSync();
            UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
            TValueBuilder rows;
            rows.BeginList();
            const std::optional<i32> values[]{10, std::nullopt, 30, 10, std::nullopt, 0};
            for (ui64 id = 1; id <= 6; ++id) rows.AddListItem().BeginStruct()
                .AddMember("id").Uint64(id).AddMember("value").OptionalInt32(values[id - 1]).EndStruct();
            rows.EndList();
            const auto inserted = client.BulkUpsert("/Root/InRows", rows.Build()).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
            for (size_t api = 0; api < 4; ++api) {
                const bool generic = api % 2;
                const bool ansi = api == 1 || api == 2;
                for (size_t index = 0; index < cases.size(); ++index) for (const bool projection : {false, true}) {
                    const auto& test = cases[index];
                    const auto& expected = !ansi && !test.Legacy.empty() ? test.Legacy : test.Ansi;
                    const auto key = (api * cases.size() + index) * 2 + projection;
                    const TString context = TStringBuilder() << "IN strategy=" << strategy << ", api=" << api << ", case=" << index << ", projection=" << projection;
                    TString sql = "PRAGMA YqlSelect='force'; ";
                    if (api == 2) sql += "PRAGMA AnsiInForEmptyOrNullableItemsCollections; ";
                    if (api == 3) sql += "PRAGMA DisableAnsiInForEmptyOrNullableItemsCollections; ";
                    sql += projection ? TStringBuilder() << "SELECT a.id, " << test.Expression << " AS v FROM `/Root/InRows` AS a ORDER BY a.id" :
                        TStringBuilder() << "SELECT a.id FROM `/Root/InRows` AS a WHERE " << test.Expression << " ORDER BY a.id";
                    Cerr << context << ": " << sql << Endl;
                    const char* expectedError = nullptr;
                    // Preserve existing frontend/backend boundaries. Native cases
                    // always execute, with their values checked independently.
                    if (strategy == 0) {
                        if (index == 4) expectedError = "Member not found: _yql_join_sublink_0__alias_a.value";
                        if (index == 7) expectedError = "Expected persistable data, but got: Unit";
                        if (index == 10) expectedError = "Only SqlIn for any/all is supported on YqlSelect";
                    } else if (strategy == 1) {
                        if (index == 7) expectedError = "Expected persistable data, but got: Unit";
                        else if (projection && index == 0) expectedError = "column not found";
                        else if (projection && index != 11 && index != 12 && !(index == 10 && !ansi))
                            expectedError = "Rewrite error, type should be";
                        else if (index == 5 || index == 6 || index == 9 || index == 10 || index == 12)
                            expectedError = "Only a single column reference in the IN clause is supported";
                    }
                    auto inspect = [&](const auto& result) {
                        if (expectedError) {
                            UNIT_ASSERT_C(!result.IsSuccess(), context);
                            UNIT_ASSERT_C(result.GetIssues().ToString().find(expectedError) != std::string::npos, context + ": " + result.GetIssues().ToString());
                            return;
                        }
                        UNIT_ASSERT_C(result.IsSuccess(), context + ": " + result.GetIssues().ToString());
                        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, context);
                        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                        const auto& resultSet = result.GetResultSet(0);
                        const auto actual = SnapshotRows(resultSet);
                        TVector<ui64> ids;
                        for (size_t row = 0; row < expected.size(); ++row)
                            if (projection || expected[row].value_or(false)) ids.push_back(row + 1);
                        // Non-ANSI NOT IN on this nullable input is inconsistent
                        // in the old filter rewrites: the old optimizer keeps NULL
                        // lookups, and old RBO uses ANSI NULL-item semantics.
                        const bool legacyNotInBoundary = !projection && !ansi && index == 2;
                        if (legacyNotInBoundary && strategy != 2) ids = strategy == 0 ? TVector<ui64>{2, 3, 5, 6} : TVector<ui64>{};
                        UNIT_ASSERT_VALUES_EQUAL_C(resultSet.RowsCount(), ids.size(), context);
                        {
                            TResultSetParser parser(resultSet);
                            for (const auto id : ids) {
                                UNIT_ASSERT(parser.TryNextRow());
                                UNIT_ASSERT_VALUES_EQUAL_C(parser.ColumnParser("id").GetUint64(), id, context);
                                if (projection) {
                                    TTypeParser type(resultSet.GetColumnsMeta()[1].Type);
                                    const TBool value = type.GetKind() == TTypeParser::ETypeKind::Optional ? parser.ColumnParser("v").GetOptionalBool() : TBool{parser.ColumnParser("v").GetBool()};
                                    UNIT_ASSERT_C(value == expected[id - 1], context + ": " + actual.Values);
                                }
                            }
                        }
                        if (strategy == 0) reference[key] = actual;
                        else if (reference[key]) {
                            if (legacyNotInBoundary) UNIT_ASSERT(actual.Columns == reference[key]->Columns);
                            else AssertSame(actual, *reference[key]);
                        }
                    };
                    if (generic) inspect(runner.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync());
                    else {
                        const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
                        inspect(result);
                        if (!result.IsSuccess()) session = client.CreateSession().GetValueSync().GetSession();
                    }
                }
            }
        }
    }

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

    Y_UNIT_TEST(RuntimeDateParsingAndIntervalsMatchLegacy) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        for (const bool utf8 : {false, true}) {
            const TString sql = TStringBuilder() << "DECLARE $s AS " << (utf8 ? "Utf8" : "String") << R"(?;
                DECLARE $amount AS Int32?;
                SELECT CAST($s AS Date), DateTime::IntervalFromDays($amount),
                    DateTime::IntervalFromHours($amount), DateTime2::IntervalFromMinutes($amount),
                    CAST($s AS Date) + DateTime::IntervalFromDays($amount)
            )";
            for (const auto& text : TVector<std::optional<std::string>>{std::nullopt, "", "1970-1-1", "1998-3-18", "2000-02-29",
                    "2100-02-29", "2105-12-31", "2106-1-1", "1998-003-18", " 1998-03-18", "1998-03-18T00:00:00Z"}) {
                for (const auto amount : std::initializer_list<std::optional<i32>>{std::nullopt, std::numeric_limits<i32>::lowest(),
                        -213503983, -213503982, -49673, -49672, -1, 0, 1, 49672, 49673, 213503982, 213503983, std::numeric_limits<i32>::max()}) {
                    auto builder = TParamsBuilder();
                    if (utf8) builder.AddParam("$s").OptionalUtf8(text).Build();
                    else builder.AddParam("$s").OptionalString(text).Build();
                    builder.AddParam("$amount").OptionalInt32(amount).Build();
                    const auto params = builder.Build();
                    AssertSame(Snapshot(native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()),
                        Snapshot(legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()));
                }
            }
        }
    }

    Y_UNIT_TEST(RuntimeCalendarOperationsMatchLegacy) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        const TString sql = R"(
            DECLARE $date AS Date?;
            DECLARE $time AS Datetime?;
            DECLARE $stamp AS Timestamp?;
            DECLARE $shift AS Int32;
            $calendar = DateTime::ShiftMonths($stamp, $shift);
            SELECT DateTime::GetYear($stamp), DateTime::GetYear($calendar),
                DateTime::MakeTimestamp($calendar), DateTime::MakeDate($calendar),
                DateTime::MakeDate(DateTime::ShiftYears($date, $shift)),
                DateTime::MakeDatetime(DateTime::ShiftMonths($time, $shift)),
                DateTime::MakeTimestamp(DateTime::ShiftYears($calendar, $shift)),
                DateTime::MakeDate(DateTime::Split($stamp)),
                CAST($date AS Timestamp), CAST($stamp AS Date), CAST($stamp AS Datetime)
        )";
        for (const auto* text : {static_cast<const char*>(nullptr), "1970-01-01T00:00:00Z", "2000-02-29T12:34:56.123456Z",
                "2020-01-31T23:59:59.999999Z", "2020-03-31T12:34:56.123456Z", "2105-12-31T23:59:59.999999Z"}) {
            for (const i32 shift : {-49152, -4096, -13, -1, 0, 1, 12, 100, 4096, 49152}) {
                const auto instant = text ? std::optional<TInstant>{TInstant::ParseIso8601(text)} : std::nullopt;
                auto builder = TParamsBuilder();
                builder.AddParam("$date").OptionalDate(instant).Build();
                builder.AddParam("$time").OptionalDatetime(instant).Build();
                builder.AddParam("$stamp").OptionalTimestamp(instant).Build();
                builder.AddParam("$shift").Int32(shift).Build();
                const auto params = builder.Build();
                AssertSame(Snapshot(native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()),
                    Snapshot(legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()));
            }
        }
        for (const auto* query : {
                "SELECT DateTime::GetYear(Date('2020-02-29')), DateTime::MakeTimestamp(DateTime::Split(Date('2020-02-29')))",
                "SELECT DateTime::MakeDate(Date('2020-02-29')), DateTime::MakeDatetime(Timestamp('2020-02-29T12:34:56.123456Z'))",
                "SELECT CAST(Date('2020-01-01') AS Timestamp), CAST(Timestamp('2020-01-01T23:59:59.999999Z') AS Datetime), CAST(Timestamp('2020-01-01T23:59:59.999999Z') AS Date)",
                "SELECT CAST(1 AS Date), CAST(1 AS Datetime), CAST(1 AS Timestamp), CAST(-1 AS Date), CAST(49673 AS Date), CAST(4291747200ul AS Datetime), CAST(4291747200000000ul AS Timestamp)",
                "SELECT CAST(Uint8('1') AS Date), CAST(Uint16('1') AS Datetime), CAST(Uint32('1') AS Timestamp), CAST('2020-02-29' AS Date), CAST('2020-02-30' AS Date), CAST(NULL AS Timestamp)",
                "SELECT DateTime::GetYear(CAST(NULL AS Date)), DateTime::MakeDate(DateTime::ShiftMonths(CAST(NULL AS Timestamp), 1))"}) {
            AssertSame(Snapshot(native.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync()),
                Snapshot(legacy.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync()));
        }
        // Bare NULL has no temporal type for these polymorphic operations.
        for (const auto* query : {"SELECT DateTime::GetYear(NULL)", "SELECT DateTime::MakeDate(DateTime::ShiftMonths(NULL, 1))"}) {
            UNIT_ASSERT(!native.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());
            UNIT_ASSERT(!legacy.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());
        }
    }

    Y_UNIT_TEST(RuntimePercentLikePatternsMatchLegacy) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        const char* patterns[]{"", "%", "%%", "a%b", "%a%b%", "a%b%", "%a%b", "ab%b", "%ab%ba%", "a%%b%a", "%Customer%Complaints%", "%é%ж%"};
        for (const bool utf8 : {false, true}) {
            TVector<std::optional<std::string>> inputs{std::nullopt, "", "a", "ab", "abb", "abc", "aba", "abba", "aaabbb", "a\nb", std::string{"a\0b", 3}, "Customer Complaints", "Complaints Customer", "é_ж"};
            if (!utf8) {
                inputs.push_back(std::string{"a\xff" "b", 3});
                inputs.push_back(std::string{"\xc0\xaf" "ab", 4});
            }
            TString sql = utf8 ? "DECLARE $s AS Utf8?; SELECT " : "DECLARE $s AS String?; SELECT ";
            for (std::size_t i = 0; i < std::size(patterns); ++i) {
                if (i) sql += ", ";
                sql += TStringBuilder() << "$s LIKE '" << patterns[i] << "' AS v" << i;
            }
            for (const auto& input : inputs) {
                auto builder = TParamsBuilder();
                auto& value = builder.AddParam("$s");
                if (utf8) value.OptionalUtf8(input).Build();
                else value.OptionalString(input).Build();
                const auto params = builder.Build();
                const auto expected = legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync();
                UNIT_ASSERT_C(expected.IsSuccess(), expected.GetIssues().ToString());
                const auto actual = native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync();
                UNIT_ASSERT_C(actual.IsSuccess(), actual.GetIssues().ToString());
                UNIT_ASSERT_C(actual.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, actual.GetIssues().ToString());
                AssertSame(Snapshot(actual), Snapshot(expected));
            }
        }
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

    Y_UNIT_TEST(RuntimeDecimalIntegerProductsMatchLegacy) {
        TKikimrRunner nativeRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED));
        TKikimrRunner legacyRunner(NativeSettings(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY));
        auto native = nativeRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto legacy = legacyRunner.GetTableClient().CreateSession().GetValueSync().GetSession();
        const TString sql = R"(
            DECLARE $a AS Decimal(3,2)?;
            DECLARE $i AS Int64?;
            DECLARE $u AS Uint64?;
            SELECT $a * CAST($i AS Int8), $a * CAST($i AS Int16), $a * CAST($i AS Int32), $a * $i, $i * $a,
                $a * CAST($u AS Uint8), $a * CAST($u AS Uint16), $a * CAST($u AS Uint32), $a * $u, $u * $a
        )";
        for (const auto* decimal : {static_cast<const char*>(nullptr), "0", "0.01", "-0.01", "9.99", "-9.99", "inf", "-inf", "nan"}) {
            for (const auto& [signedValue, unsignedValue] : std::initializer_list<std::pair<std::optional<i64>, std::optional<ui64>>>{
                    {std::nullopt, std::nullopt}, {0, 0}, {1, 1}, {-2, 2}, {127, 255}, {-128, 256},
                    {32767, 65535}, {-32768, 65536}, {2147483647, 4294967295ULL},
                    {std::numeric_limits<i64>::lowest(), std::numeric_limits<ui64>::max()}}) {
                auto builder = TParamsBuilder();
                auto& value = builder.AddParam("$a");
                if (decimal) value.BeginOptional().Decimal(TDecimalValue(decimal, 3, 2)).EndOptional().Build();
                else value.EmptyOptional(TTypeBuilder().Decimal({3, 2}).Build()).Build();
                builder.AddParam("$i").OptionalInt64(signedValue).Build();
                builder.AddParam("$u").OptionalUint64(unsignedValue).Build();
                const auto params = builder.Build();
                AssertSame(Snapshot(native.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()),
                    Snapshot(legacy.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync()));
            }
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

    Y_UNIT_TEST(TpcdsReadQueriesMatchLegacyOnPopulatedColumnTables) {
        const auto legacy = TpcdsReadQueries(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY, false);
        const auto rbo = TpcdsReadQueries(NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY, true);
        const auto native = TpcdsReadQueries(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED, true);
        UNIT_ASSERT_VALUES_EQUAL(rbo.size(), legacy.size());
        UNIT_ASSERT_VALUES_EQUAL(native.size(), legacy.size());
        for (std::size_t i = 0; i < legacy.size(); ++i) {
            AssertSame(native[i], rbo[i]);
            // Q21's SELECT * already has different output order in the two
            // existing compilers. The fixture pins both orders and checks every
            // value by name; native must preserve the existing RBO contract.
            if (i != 0) AssertSame(native[i], legacy[i]);
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
