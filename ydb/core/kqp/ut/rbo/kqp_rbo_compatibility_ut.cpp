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
    Y_UNIT_TEST(RequiredDecimalSetOperationsUseValueIdentity) {
        struct TCase {
            ui8 LeftPrecision, LeftScale, RightPrecision, RightScale;
            std::vector<std::string> Left, Right, Intersection, Difference;
        };
        // Exercise both input orientations after common-type alignment.
        // Set identity compares those converted values, including NaN peers;
        // the final public cast is an exact widening for these fixture values.
        const std::vector<TCase> cases{
            {7, 0, 7, 0,
                {"nan", "nan", "7", "7", "8", "inf", "-inf"},
                {"nan", "7", "inf", "-inf", "9"},
                {"nan", "7", "inf", "-inf"}, {"8"}},
            {7, 0, 7, 0,
                {"nan", "nan", "7", "7", "8", "inf", "-inf"},
                {"7", "inf", "-inf", "9"},
                {"7", "inf", "-inf"}, {"nan", "8"}},
            {7, 0, 10, 0,
                {"nan", "nan", "7", "7", "8", "inf", "-inf"},
                {"nan", "7", "-inf", "12345678"},
                {"nan", "7", "-inf"}, {"8", "inf"}},
            {10, 0, 7, 0,
                {"nan", "nan", "7", "7", "12345678", "inf", "-inf"},
                {"nan", "7", "inf", "-inf", "8"},
                {"nan", "7", "inf", "-inf"}, {"12345678"}},
            {7, 0, 10, 1,
                {"nan", "nan", "7", "7", "8", "1", "10", "inf", "-inf"},
                {"nan", "7.0", "7.5", "1.0", "10.5", "inf", "-inf", "9.0"},
                {"nan", "7", "1", "inf", "-inf"}, {"8", "10"}},
            {10, 1, 7, 0,
                {"nan", "nan", "7.0", "7.0", "7.5", "1.0", "10.5", "inf", "-inf", "9.0"},
                {"nan", "7", "8", "1", "10", "inf", "-inf"},
                {"nan", "7.0", "1.0", "inf", "-inf"}, {"7.5", "10.5", "9.0"}},
        };
        for (const std::string mode : {"grace", "block", "map"}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(true);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetEnableFallbackOnDML(false);
            service->SetEnableFallbackOnMultipleStatements(false);
            service->SetDefaultLangVer(202501);
            service->SetDefaultCostBasedOptimizationLevel(0);
            service->SetUseBlockHashJoin(mode == "block");
            TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
            const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
            const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
            const auto values = [](const std::vector<std::string>& input, ui8 precision, ui8 scale) {
                TValueBuilder result;
                result.BeginList();
                for (const auto& value : input) {
                    result.AddListItem().BeginStruct().AddMember("D")
                        .Decimal(TDecimalValue(value, precision, scale)).EndStruct();
                }
                return result.EndList().Build();
            };
            bool observedNewRbo = false;
            for (const auto& item : cases) {
                const auto params = TParamsBuilder()
                    .AddParam("$lhs", values(item.Left, item.LeftPrecision, item.LeftScale))
                    .AddParam("$rhs", values(item.Right, item.RightPrecision, item.RightScale)).Build();
                for (const bool difference : {false, true}) {
                    const TString query = TStringBuilder()
                        << "PRAGMA YqlSelect = 'force'; PRAGMA ydb.CostBasedOptimizationLevel = '0'; "
                        << "PRAGMA ydb.HashJoinMode = '" << (mode == "map" ? "map" : "grace") << "'; "
                        << "DECLARE $lhs AS List<Struct<D:Decimal(" << ui32(item.LeftPrecision) << ',' << ui32(item.LeftScale) << ")>>; "
                        << "DECLARE $rhs AS List<Struct<D:Decimal(" << ui32(item.RightPrecision) << ',' << ui32(item.RightScale) << ")>>; "
                        << "$set = (SELECT D FROM AS_TABLE($lhs) " << (difference ? "EXCEPT" : "INTERSECT")
                        << " SELECT D FROM AS_TABLE($rhs)); "
                        << "SELECT CAST(D AS Decimal(12,1)) AS D FROM $set;";
                    const auto beforeSuccess = successes->Val();
                    const auto beforeFailure = failures->Val();
                    const auto explained = session.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params,
                        NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
                    UNIT_ASSERT_C(explained.IsSuccess(), query << "\n" << explained.GetIssues().ToString());
                    if (!observedNewRbo) {
                        UNIT_ASSERT(successes->Val() > beforeSuccess);
                        observedNewRbo = true;
                    }
                    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
                    UNIT_ASSERT(explained.GetStats() && explained.GetStats()->GetAst());
                    const TString ast{*explained.GetStats()->GetAst()};
                    const TString core = mode == "block" ? "BlockHashJoinCore" : mode == "map" ? "MapJoinCore" : "GraceJoinCore";
                    Cerr << "[DECIMAL-SET-IDENTITY] mode=" << mode << "\n" << query << "\n" << ast << Endl;
                    UNIT_ASSERT_C(ast.Contains(core) && ast.Contains("StablePickle"), ast);
                    UNIT_ASSERT_C(ast.Contains("(declare $lhs ") && ast.Contains("(declare $rhs "), ast);
                    const auto result = session.ExecuteQuery(query,
                        NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
                    UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString());
                    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
                    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                    const auto& rs = result.GetResultSet(0);
                    UNIT_ASSERT(!rs.Truncated());
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 1);
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Name, "D");
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Type.ToString(), "Decimal(12,1)");
                    std::vector<std::string> actual, expected;
                    TResultSetParser parser(rs);
                    while (parser.TryNextRow()) { actual.push_back(parser.ColumnParser(0).GetDecimal().ToString()); }
                    for (const auto& value : difference ? item.Difference : item.Intersection) {
                        expected.push_back(TDecimalValue(value, 12, 1).ToString());
                    }
                    std::sort(actual.begin(), actual.end());
                    std::sort(expected.begin(), expected.end());
                    UNIT_ASSERT_VALUES_EQUAL_C(actual, expected, query << "\n" << FormatResultSetYson(rs) << "\n" << ast);
                }
            }
        }
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
