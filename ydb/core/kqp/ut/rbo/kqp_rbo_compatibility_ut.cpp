#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>
#include <string>
#include <vector>

namespace NKikimr::NKqp {
namespace {

using namespace NYdb;

Y_UNIT_TEST_SUITE(KqpRboCompatibility) {
    // NUL is an admitted one-byte ASCII SQL ESCAPE. Literal and parameter
    // patterns must preserve the same complete bag, including binary subjects.
    Y_UNIT_TEST_TWIN(LikeNulEscapeLiteralAndParameter, Lang202603) {
        NKikimrConfig::TAppConfig config;
        auto* service = config.MutableTableServiceConfig();
        service->SetEnableNewRBO(true);
        service->SetEnableFallbackToYqlOptimizer(false);
        service->SetEnableFallbackOnDML(false);
        service->SetEnableFallbackOnMultipleStatements(false);
        service->SetDefaultLangVer(Lang202603 ? 202603 : 202501);
        TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
        auto tableClient = kikimr.GetTableClient();
        auto tableSession = tableClient.CreateSession().GetValueSync().GetSession();
        const auto created = tableSession.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/LikeNulEscapeRows` (
                Id Int32 NOT NULL, Subject String NOT NULL, PRIMARY KEY (Id)
            );
        )").GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
        struct TRow { i32 Id; std::string Subject; };
        const std::vector<TRow> rows{
            {1, "%"}, {2, "_"}, {3, "x"}, {4, ""}, {5, "%%"}, {6, "__"},
            {7, std::string("\0%", 2)}, {8, std::string("\0_", 2)},
            {9, std::string("\0x", 2)}, {10, std::string("\0", 1)},
        };
        TValueBuilder values;
        values.BeginList();
        for (const auto& row : rows) {
            values.AddListItem().BeginStruct().AddMember("Id").Int32(row.Id)
                .AddMember("Subject").String(row.Subject).EndStruct();
        }
        const auto seeded = tableClient.BulkUpsert(
            "/Root/LikeNulEscapeRows", values.EndList().Build()).GetValueSync();
        UNIT_ASSERT_C(seeded.IsSuccess(), seeded.GetIssues().ToString());
        const TString query = R"SQL(
PRAGMA YqlSelect='force';
PRAGMA AnsiLike;
DECLARE $p AS String;
DECLARE $one AS String;
SELECT Id, Subject,
    Subject LIKE '\x00%'s ESCAPE '\x00's AS LiteralPercent,
    Subject LIKE $p ESCAPE '\x00's AS ParameterPercent,
    Subject LIKE '\x00_'s ESCAPE '\x00's AS LiteralUnderscore,
    Subject LIKE $one ESCAPE '\x00's AS ParameterUnderscore
FROM `/Root/LikeNulEscapeRows`;
        )SQL";
        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        bool first = true;
        TStringBuilder mismatches;
        size_t mismatchedBindings = 0;
        for (const bool escapedPatterns : {true, false}) {
            const auto params = TParamsBuilder()
                .AddParam("$p").String(escapedPatterns ? std::string("\0%", 2) : "x").Build()
                .AddParam("$one").String(escapedPatterns ? std::string("\0_", 2) : "x").Build().Build();
            const auto beforeSuccess = successes->Val(), beforeFailure = failures->Val();
            const auto explain = session.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params,
                NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
            UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
            if (first) { UNIT_ASSERT(successes->Val() > beforeSuccess); first = false; }
            UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
            UNIT_ASSERT(explain.GetStats() && explain.GetStats()->GetAst());
            const TString ast{*explain.GetStats()->GetAst()};
            UNIT_ASSERT_C(ast.Contains("Re2.PatternFromLike") && ast.Contains("Parameter"), ast);
            Cerr << "[LIKE-NUL-ESCAPE] lang=" << (Lang202603 ? 202603 : 202501)
                 << " pattern_bytes=" << (escapedPatterns ? "00 25 / 00 5f" : "78 / 78")
                 << "\n" << query << "\n" << ast << Endl;
            const auto result = session.ExecuteQuery(query,
                NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
            const auto& rs = result.GetResultSet(0);
            UNIT_ASSERT(!rs.Truncated());
            const std::vector<std::string> names{
                "Id", "Subject", "LiteralPercent", "ParameterPercent", "LiteralUnderscore", "ParameterUnderscore"};
            UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[i].Name, names[i]);
                UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[i].Type.ToString(), i == 0 ? "Int32" : i == 1 ? "String" : "Bool");
            }
            std::vector<std::vector<i32>> actual, expected;
            TResultSetParser parser(rs);
            while (parser.TryNextRow()) {
                const auto id = parser.ColumnParser(0).GetInt32();
                const auto found = std::find_if(rows.begin(), rows.end(), [&](const TRow& row) { return row.Id == id; });
                UNIT_ASSERT(found != rows.end());
                UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser(1).GetString(), found->Subject);
                actual.push_back({id, parser.ColumnParser(2).GetBool(), parser.ColumnParser(3).GetBool(),
                    parser.ColumnParser(4).GetBool(), parser.ColumnParser(5).GetBool()});
            }
            for (const auto& row : rows) {
                expected.push_back({row.Id, row.Subject == "%", row.Subject == (escapedPatterns ? "%" : "x"),
                    row.Subject == "_", row.Subject == (escapedPatterns ? "_" : "x")});
            }
            std::sort(actual.begin(), actual.end());
            std::sort(expected.begin(), expected.end());
            if (actual != expected) {
                ++mismatchedBindings;
                mismatches << "pattern_bytes=" << (escapedPatterns ? "00 25 / 00 5f" : "78 / 78")
                    << "\n" << query << "\n" << ast << "\n" << FormatResultSetYson(rs) << "\n";
            }
        }
        UNIT_ASSERT_VALUES_EQUAL_C(mismatchedBindings, 0, mismatches);
    }

    Y_UNIT_TEST_TWIN(SafeCastOptionalResults, PhysicalStagePeephole) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        appConfig.MutableTableServiceConfig()->SetEnableNewRBOPhysicalStagePeephole(PhysicalStagePeephole);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));

        const auto params = TParamsBuilder()
            .AddParam("$rows")
                .BeginList()
                    .AddListItem().BeginStruct()
                        .AddMember("value").Decimal(TDecimalValue("42", 7, 0))
                        .AddMember("number").Int32(42)
                        .AddMember("text").String("42")
                        .AddMember("nullable").OptionalInt32(42)
                    .EndStruct()
                    .AddListItem().BeginStruct()
                        .AddMember("value").Decimal(TDecimalValue("42", 7, 0))
                        .AddMember("number").Int32(-7)
                        .AddMember("text").String("invalid")
                        .AddMember("nullable").OptionalInt32(std::nullopt)
                    .EndStruct()
                .EndList()
            .Build()
            .Build();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(R"(
            DECLARE $rows AS List<Struct<value:Decimal(7,0),number:Int32,text:String,nullable:Int32?>>;
            SELECT
                CAST(value AS Decimal(7,0)?) AS value,
                CAST(number AS Decimal(7,0)?) AS numeric_decimal,
                CAST(number AS String?) AS numeric_string,
                CAST(text AS Decimal(7,0)?) AS parsed_decimal,
                CAST(value AS Decimal(7,0)??) AS nested_decimal,
                CAST(nullable AS String??) AS nested_string
            FROM AS_TABLE($rows) ORDER BY number DESC;
        )", NQuery::TTxControl::NoTx(), params).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        const auto& resultSet = result.GetResultSet(0);
        UNIT_ASSERT_VALUES_EQUAL(resultSet.RowsCount(), 2);
        const TVector<TString> types = {"Decimal(7,0)?", "Decimal(7,0)?", "String?",
            "Decimal(7,0)?", "Decimal(7,0)??", "String??"};
        for (size_t column = 0; column < types.size(); ++column) {
            UNIT_ASSERT_VALUES_EQUAL(resultSet.GetColumnsMeta()[column].Type.ToString(), types[column]);
        }
        TResultSetParser parser(resultSet);
        for (const bool present : {true, false}) {
            UNIT_ASSERT(parser.TryNextRow());
            const auto value = parser.ColumnParser(0).GetOptionalDecimal();
            UNIT_ASSERT(value);
            UNIT_ASSERT_VALUES_EQUAL(value->ToString(), "42");
            const auto numeric = parser.ColumnParser(1).GetOptionalDecimal();
            UNIT_ASSERT(numeric);
            UNIT_ASSERT_VALUES_EQUAL(numeric->ToString(), present ? "42" : "-7");
            const auto text = parser.ColumnParser(2).GetOptionalString();
            UNIT_ASSERT(text);
            UNIT_ASSERT_VALUES_EQUAL(*text, present ? "42" : "-7");
            const auto parsed = parser.ColumnParser(3).GetOptionalDecimal();
            UNIT_ASSERT_VALUES_EQUAL(parsed.has_value(), present);
            if (parsed) {
                UNIT_ASSERT_VALUES_EQUAL(parsed->ToString(), "42");
            }
            auto& nestedDecimal = parser.ColumnParser(4);
            nestedDecimal.OpenOptional();
            UNIT_ASSERT(!nestedDecimal.IsNull());
            const auto decimal = nestedDecimal.GetOptionalDecimal();
            UNIT_ASSERT(decimal);
            UNIT_ASSERT_VALUES_EQUAL(decimal->ToString(), "42");
            auto& nestedString = parser.ColumnParser(5);
            nestedString.OpenOptional();
            UNIT_ASSERT(!nestedString.IsNull());
            const auto string = nestedString.GetOptionalString();
            UNIT_ASSERT_VALUES_EQUAL(string.has_value(), present);
            if (string) {
                UNIT_ASSERT_VALUES_EQUAL(*string, "42");
            }
        }
    }

    Y_UNIT_TEST(ManyJoinsWithDecimalRanges) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        appConfig.MutableTableServiceConfig()->SetEnableNewRBOPhysicalStagePeephole(false);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));

        // Filtering each joined table by a non-nullable decimal key produces
        // optional casts in the read ranges.
        constexpr ui32 JoinCount = 90;
        TStringBuilder schema;
        for (ui32 i = 0; i <= JoinCount; ++i) {
            schema << "CREATE TABLE `/Root/t" << i << "` ("
                << "scope Decimal(7,0) NOT NULL, key Uint64 NOT NULL, value String,"
                << "PRIMARY KEY (scope, key));\n";
        }
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto created = session.ExecuteSchemeQuery(schema).GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());

        TStringBuilder query;
        query << "SELECT t0.key";
        for (ui32 i = 1; i <= JoinCount; ++i) {
            query << ", t" << i << ".value AS value" << i;
        }
        query << " FROM `/Root/t0` AS t0\n";
        for (ui32 i = 1; i <= JoinCount; ++i) {
            query << "LEFT JOIN (SELECT key, value FROM `/Root/t" << i
                << "` WHERE scope = CAST('0' AS Decimal(7,0))) AS t" << i
                << " ON t0.key = t" << i << ".key\n";
        }
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        const auto successesBefore = successes->Val();
        const auto failuresBefore = failures->Val();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(query, NQuery::TTxControl::NoTx(),
            NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT(result.GetStats() && result.GetStats()->GetPlan());
        UNIT_ASSERT(!result.GetStats()->GetPlan()->empty());
        UNIT_ASSERT_VALUES_EQUAL(successes->Val(), successesBefore + 1);
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failuresBefore);
    }
}

} // namespace
} // namespace NKikimr::NKqp
