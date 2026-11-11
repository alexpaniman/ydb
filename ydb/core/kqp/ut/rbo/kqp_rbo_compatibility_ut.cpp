#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/resource/resource.h>
#include <yql/essentials/public/langver/yql_langver.h>
#include <util/stream/file.h>
#include <util/string/subst.h>
#include <regex>
#include <limits>

namespace NKikimr::NKqp {
namespace {

using namespace NYdb;

Y_UNIT_TEST_SUITE(KqpRboCompatibility) {
    // PR #58587 catalog/name-resolution cases, using required native routing
    // instead of polling asynchronous logs for an admission marker.
    Y_UNIT_TEST(NativeResolvesNamesUsingLoadedMetadata) {
        for (const bool native : {false, true}) {
            NKikimrConfig::TAppConfig app;
            auto* config = app.MutableTableServiceConfig();
            config->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            config->SetEnableNewRBO(native);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableNewRBOPhysicalStagePeephole(false);
            TKikimrRunner kikimr(TKikimrSettings(app).SetWithSampleTables(false));
            auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            const auto scheme = tableSession.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/names` (id Int64 NOT NULL, value String, amount Decimal(12,2), PRIMARY KEY(id));").GetValueSync();
            UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            const auto good = [&](const TString& sql, const TParams& params = TParamsBuilder().Build()) {
                const auto result = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx(), params).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
                UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, sql);
            };
            const auto bad = [&](const TString& sql, const TString& diagnostic) {
                const auto result = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(!result.IsSuccess(), sql);
                if (native) UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), diagnostic);
            };
            good(TString(native ? "PRAGMA YqlSelect = 'force'; " : "") +
                "SELECT n.id, n.value FROM `/Root/names` AS n WHERE n.id > 0 ORDER BY n.id;");
            good("SELECT id + $offset, amount FROM `/Root/names` LIMIT 5;",
                TParamsBuilder().AddParam("$offset").Int64(3).Build().Build());
            bad("SELECT n.id + n.value FROM `/Root/names` AS n;", "No common type");
            good("SELECT r.id FROM `/Root/names` AS l LEFT JOIN `/Root/names` AS r ON l.id = r.id;");
            good("SELECT COUNT(*) FROM `/Root/names`;");
            bad("SELECT n.absent FROM `/Root/names` AS n;", "unknown or ungrouped column: n.absent");
            bad("SELECT id FROM `/Root/names` AS l JOIN `/Root/names` AS r ON l.id = r.id;", "ambiguous column: id");
            // A failed binding must not contaminate the next compilation.
            good("SELECT id FROM `/Root/names` WHERE id = 7;");
        }
    }

    Y_UNIT_TEST(NativeSelectAndSyntaxErrors) {
        for (const bool native : {false, true}) {
            NKikimrConfig::TAppConfig app;
            auto* config = app.MutableTableServiceConfig();
            config->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            config->SetEnableNewRBO(native);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableNewRBOPhysicalStagePeephole(false);
            TKikimrRunner kikimr(TKikimrSettings(app).SetWithSampleTables(false));
            auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            const auto scheme = tableSession.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/t1` (a Int64 NOT NULL, PRIMARY KEY(a));").GetValueSync();
            UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
            const auto success = counters->GetCounter("Compilation/NewRBO/Success");
            const auto failure = counters->GetCounter("Compilation/NewRBO/Failed");
            const auto beforeSuccess = success->Val(), beforeFailure = failure->Val();
            for (const auto* sql : {"SELECT COUNT(*) FROM `/Root/t1`;",
                    "--!ansi_lexer\nSELECT COUNT(*) AS \"count\" FROM `/Root/t1`;"}) {
                const auto result = session.ExecuteQuery(sql, NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
                UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[0u]]");
                UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, sql);
            }
            UNIT_ASSERT_VALUES_EQUAL(failure->Val(), beforeFailure);
            UNIT_ASSERT_VALUES_EQUAL(success->Val() - beforeSuccess, native ? 2 : 0);
            const auto invalid = session.ExecuteQuery("SELECT (", NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT(!invalid.IsSuccess());
            UNIT_ASSERT(!invalid.GetIssues().Empty());
        }
    }


    Y_UNIT_TEST(NativeScalarSettingsAndBitCastMatchLegacy) {
        TParamsBuilder parameters;
        auto& rows = parameters.AddParam("$rows").BeginList();
        const TVector<std::optional<i64>> signedValues{std::nullopt, 0, 1, -1, std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max()};
        const TVector<std::optional<ui64>> unsignedValues{std::nullopt, 0, 1, std::numeric_limits<ui64>::max()};
        ui32 id = 0;
        for (const auto a : signedValues) for (const auto b : signedValues) {
            rows.AddListItem().BeginStruct().AddMember("id").Uint32(id)
                .AddMember("a").OptionalInt64(a).AddMember("b").OptionalInt64(b)
                .AddMember("u").OptionalUint64(unsignedValues[id % unsignedValues.size()])
                .AddMember("v").OptionalUint64(unsignedValues[(id / unsignedValues.size()) % unsignedValues.size()])
                .EndStruct();
            ++id;
        }
        rows.EndList().Build();
        const auto params = parameters.Build();
        const TString declaration = "DECLARE $rows AS List<Struct<id:Uint32,a:Int64?,b:Int64?,u:Uint64?,v:Uint64?>>; ";
        const TVector<TString> queries{
            declaration + R"(PRAGMA CheckedOps='true'; SELECT id, a+b, a-b, a*b, a/b, a%b, -a,
                u+v, u-v, u*v, u/v, u%v, -u, a+u, a-u, a*u, a/u, a%u FROM AS_TABLE($rows) ORDER BY id;)",
            declaration + R"(PRAGMA ClassicDivision='false'; PRAGMA CheckedOps='true';
                SELECT id, a/b, u/v, a/u FROM AS_TABLE($rows) ORDER BY id;)",
            declaration + R"(SELECT id, BITCAST(a AS Uint8), BITCAST(a AS Uint64), BITCAST(u AS Int8),
                BITCAST(u AS Int64), BITCAST(b AS Int32) FROM AS_TABLE($rows) ORDER BY id;)",
            R"(PRAGMA UnicodeLiterals; PRAGMA CheckedOps='true'; PRAGMA ClassicDivision='false';
                $q = (SELECT 'text' AS s, 1+2 AS n, 4/2 AS d);
                $f = ($x) -> { RETURN $x+1; };
                PRAGMA DisableUnicodeLiterals; PRAGMA CheckedOps='false'; PRAGMA ClassicDivision='true';
                SELECT s, n, d FROM $q; SELECT $f(1), 'text', 4/2;
                PRAGMA UnicodeLiterals; SELECT '\xff's, 'text'u, 'text';)",
            // The old SQL parser rejects minimum signed literals before
            // arithmetic. Typed constructors keep this oracle about arithmetic;
            // native signed-literal parsing has separate reference tests.
            R"(PRAGMA CheckedOps='true'; SELECT Int64('-9223372036854775808'), -1u, 127t+1t,
                0ut-1ut, 255ut*2ut, Int8('-128') / -1t, Int8('-128') % -1t;)",
        };
        TVector<TVector<TString>> expectedRows, expectedTypes;
        for (const bool native : {false, true}) {
            NKikimrConfig::TAppConfig app;
            auto* config = app.MutableTableServiceConfig();
            config->SetEnableNewRBO(native);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableNewRBOPhysicalStagePeephole(false);
            config->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            TKikimrRunner kikimr(TKikimrSettings(app).SetWithSampleTables(false));
            for (std::size_t i = 0; i < queries.size(); ++i) {
                const auto result = kikimr.GetQueryClient().ExecuteQuery(queries[i], NQuery::TTxControl::NoTx(),
                    i < 3 ? params : TParamsBuilder().Build()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), queries[i] << ": " << result.GetIssues().ToString());
                UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, queries[i]);
                TVector<TString> values, types;
                for (const auto& output : result.GetResultSets()) {
                    values.push_back(FormatResultSetYson(output));
                    for (const auto& column : output.GetColumnsMeta()) types.push_back(column.Type.ToString());
                }
                if (!native) { expectedRows.push_back(std::move(values)); expectedTypes.push_back(std::move(types)); }
                else {
                    UNIT_ASSERT_VALUES_EQUAL_C(values.size(), expectedRows[i].size(), queries[i]);
                    for (std::size_t j = 0; j < values.size(); ++j) UNIT_ASSERT_VALUES_EQUAL_C(values[j], expectedRows[i][j], queries[i]);
                    UNIT_ASSERT_VALUES_EQUAL_C(types.size(), expectedTypes[i].size(), queries[i]);
                    for (std::size_t j = 0; j < types.size(); ++j) UNIT_ASSERT_VALUES_EQUAL_C(types[j], expectedTypes[i][j], queries[i]);
                }
            }
        }
    }

    Y_UNIT_TEST(NativeBitwiseAndNullSafeResultsMatchLegacy) {
        TParamsBuilder parameters;
        auto& rows = parameters.AddParam("$rows").BeginList();
        const TVector<std::optional<double>> values{std::nullopt, 0.0, -0.0, 2.0,
            std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
        ui32 id = 0;
        for (const auto a : values) for (const auto b : values) {
            rows.AddListItem().BeginStruct()
                .AddMember("id").Uint32(id++)
                .AddMember("a").OptionalDouble(a)
                .AddMember("b").OptionalDouble(b)
                .AddMember("f").OptionalFloat(b ? std::optional<float>{static_cast<float>(*b)} : std::nullopt)
                .AddMember("x").OptionalInt64(a ? std::optional<i64>{-1} : std::nullopt)
                .AddMember("u").OptionalUint64(a ? std::optional<ui64>{255} : std::nullopt)
                .AddMember("n").Uint8((id % 3) == 0 ? 64 : (id % 3) == 1 ? 1 : 0)
                .EndStruct();
        }
        rows.EndList().Build();
        const auto params = parameters.Build();
        const auto* query = R"(
            DECLARE $rows AS List<Struct<id:Uint32,a:Double?,b:Double?,f:Float?,x:Int64?,u:Uint64?,n:Uint8>>;
            SELECT id, a IS NOT DISTINCT FROM b AS same, a IS DISTINCT FROM b AS different,
                a IS NOT DISTINCT FROM f AS mixed, a IS DISTINCT FROM f AS mixed_different,
                x << n AS shl, x >> n AS shr, ~u AS inverted,
                u & 3u AS masked, u | 3ul AS combined, u ^ 3u AS toggled
            FROM AS_TABLE($rows) ORDER BY id;
        )";
        TString expected;
        TVector<TString> expectedTypes;
        for (const bool native : {false, true}) {
            NKikimrConfig::TAppConfig app;
            auto* config = app.MutableTableServiceConfig();
            // The legacy compiler is an independent SQL semantics oracle.
            config->SetEnableNewRBO(native);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableNewRBOPhysicalStagePeephole(false);
            config->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            TKikimrRunner kikimr(TKikimrSettings(app).SetWithSampleTables(false));
            const auto result = kikimr.GetQueryClient().ExecuteQuery(query, NQuery::TTxControl::NoTx(), params).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
            const auto& output = result.GetResultSet(0);
            UNIT_ASSERT_VALUES_EQUAL(output.RowsCount(), values.size() * values.size());
            TVector<TString> types;
            for (const auto& column : output.GetColumnsMeta()) types.push_back(column.Type.ToString());
            if (!native) {
                expected = FormatResultSetYson(output);
                expectedTypes = std::move(types);
            } else {
                UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(output), expected);
                UNIT_ASSERT(types == expectedTypes);
            }
        }
    }

    void RunSafeCastOptionalResults(bool PhysicalStagePeephole, bool native) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        appConfig.MutableTableServiceConfig()->SetEnableNewRBOPhysicalStagePeephole(PhysicalStagePeephole);
        if (native) appConfig.MutableTableServiceConfig()->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
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
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
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

    Y_UNIT_TEST_TWIN(SafeCastOptionalResults, PhysicalStagePeephole) { RunSafeCastOptionalResults(PhysicalStagePeephole, false); }
    Y_UNIT_TEST_TWIN(NativeSafeCastOptionalResults, PhysicalStagePeephole) { RunSafeCastOptionalResults(PhysicalStagePeephole, true); }

    Y_UNIT_TEST(NativeNullableDecimalBoundsPreserveTypeParameters) {
        for (const bool indexed : {false, true}) {
            NKikimrConfig::TAppConfig appConfig;
            auto* config = appConfig.MutableTableServiceConfig();
            config->SetEnableNewRBO(true);
            config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableNewRBOPhysicalStagePeephole(false);
            TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));
            auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            const TString schema = indexed
                ? "CREATE TABLE `/Root/decimal_keys` (id Uint64 NOT NULL, d Decimal(9,2), m String NOT NULL, payload String, "
                  "INDEX by_d GLOBAL UNIQUE SYNC ON (d, m), PRIMARY KEY(id));"
                : "CREATE TABLE `/Root/decimal_keys` (id Uint64 NOT NULL, d Decimal(9,2), m String NOT NULL, payload String, PRIMARY KEY(d,id));";
            const auto created = session.ExecuteSchemeQuery(schema).GetValueSync();
            UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
            auto client = kikimr.GetQueryClient();
            const auto inserted = client.ExecuteQuery(R"(
                UPSERT INTO `/Root/decimal_keys` (id,d,m,payload) VALUES
                    (1ul,NULL,'a','null-a'), (2ul,CAST('1.25' AS Decimal(9,2)),'a','finite'),
                    (3ul,NULL,'b','null-b'), (4ul,CAST('0.50' AS Decimal(9,2)),'b','lower');
            )", NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
            const TVector<std::pair<TString, TVector<ui64>>> cases{
                {"d IS NULL", {1,3}}, {"d IS NULL AND m = 'a'", {1}},
                {"d < CAST('1.25' AS Decimal(9,2))", {4}},
                {"d = CAST('1.25' AS Decimal(9,2))", {2}},
                {"d IS NULL OR d = CAST('1.25' AS Decimal(9,2))", {1,2,3}}};
            for (const auto& [predicate, expected] : cases) {
                const auto query = TStringBuilder() << "SELECT id,payload FROM `/Root/decimal_keys` WHERE " << predicate << " ORDER BY id;";
                const auto result = client.ExecuteQuery(query, NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), query << ": " << result.GetIssues().ToString());
                UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
                TResultSetParser rows(result.GetResultSet(0));
                TVector<ui64> actual;
                while (rows.TryNextRow()) actual.push_back(rows.ColumnParser("id").GetUint64());
                UNIT_ASSERT_VALUES_EQUAL(actual, expected);
            }
        }
    }

    Y_UNIT_TEST(NativeNullableParameterRangesExecuteWithoutPeephole) {
        for (const bool nullableKey : {false, true}) {
            NKikimrConfig::TAppConfig appConfig;
            auto* config = appConfig.MutableTableServiceConfig();
            config->SetEnableNewRBO(true);
            config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
            config->SetEnableFallbackToYqlOptimizer(false);
            config->SetEnableNewRBOPhysicalStagePeephole(false);
            TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));
            auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            const auto created = session.ExecuteSchemeQuery(TStringBuilder()
                << "CREATE TABLE `/Root/parameter_keys` (id Int32 " << (nullableKey ? "" : "NOT NULL")
                << ", PRIMARY KEY(id));").GetValueSync();
            UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
            auto client = kikimr.GetQueryClient();
            const auto inserted = client.ExecuteQuery(TStringBuilder()
                << "UPSERT INTO `/Root/parameter_keys` (id) VALUES (1),(2),(3)"
                << (nullableKey ? ",(NULL);" : ";"), NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
            for (const TString op : {"=", "<", "<=", ">", ">="})
            for (const bool disjunction : {false, true})
            for (const unsigned form : {0, 1, 2, 3})
            // Reuse each query with absent, present, then absent values. A
            // compiled range must test the parameter at execution, not prepare.
            for (const bool present : {false, true, false}) {
                const auto params = TParamsBuilder().AddParam("$bound")
                    .OptionalInt32(present ? std::optional<i32>{2} : std::nullopt).Build()
                    .AddParam("$choose").Bool(form != 3).Build().Build();
                const auto expression = form == 0 ? "$bound" : form == 1 ? "($bound + 0)"
                    : "IF($choose, $bound, CAST(3 AS Int32?))";
                const auto query = TStringBuilder() << "DECLARE $bound AS Int32?; DECLARE $choose AS Bool; "
                    << "SELECT id FROM `/Root/parameter_keys` WHERE id " << op << " " << expression
                    << (disjunction ? " OR id = 3" : "") << " ORDER BY id;";
                const auto result = client.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), query << ": " << result.GetIssues().ToString());
                UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
                TVector<i32> expected;
                const auto bound = form == 3 ? 3 : 2;
                for (const i32 value : {1, 2, 3}) {
                    const bool matches = (present || form == 3) && (op == "=" ? value == bound : op == "<" ? value < bound :
                        op == "<=" ? value <= bound : op == ">" ? value > bound : value >= bound);
                    if (matches || (disjunction && value == 3)) expected.push_back(value);
                }
                TResultSetParser rows(result.GetResultSet(0));
                TVector<i32> actual;
                while (rows.TryNextRow()) {
                    if (nullableKey) {
                        const auto value = rows.ColumnParser("id").GetOptionalInt32();
                        UNIT_ASSERT(value);
                        actual.push_back(*value);
                    } else actual.push_back(rows.ColumnParser("id").GetInt32());
                }
                UNIT_ASSERT_VALUES_EQUAL_C(actual, expected, query << ", present=" << present);
            }
        }
    }

    Y_UNIT_TEST(NativeTpchPerformanceFixtureUsesTableApiWithoutFallback) {
        NKikimrConfig::TAppConfig appConfig;
        auto* config = appConfig.MutableTableServiceConfig();
        config->SetEnableNewRBO(true);
        config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
        config->SetEnableFallbackToYqlOptimizer(false);
        config->SetEnableFallbackOnMultipleStatements(false);
        config->SetEnableInlineJoinFiltersAfterCBO(false);
        config->SetEnableNewRBOPhysicalStagePeephole(false);
        config->SetAllowOlapDataQuery(true);
        config->SetDefaultLangVer(NYql::GetMaxLangVersion());
        config->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto schema = TFileInput(SRC_("../join/data/schema/tpch.sql")).ReadAll();
        const auto columnSchema = std::regex_replace(std::string(schema.data(), schema.size()),
            std::regex(R"((CREATE TABLE [^(]+\([^;]*\))\s*;)"),
            "$1 WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 16);");
        const auto created = session.ExecuteSchemeQuery(TString(columnSchema)).GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
        const auto constants = NResource::Find("consts.yql");
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        const auto before = successes->Val(), failedBefore = failures->Val();
        // Exact IDs, preamble, table placeholders, schema and API from Pavel's
        // TPCH_YDB_PERF test. Only the frontend admission is made stricter.
        for (const ui32 id : {1,5,6,7,8,9,10,11,12,13,14,18,19}) {
            auto query = NResource::Find(TStringBuilder() << "resfs/file/tpch/queries/yql/q" << id << ".sql");
            SubstGlobal(query, "{path}", "/Root/");
            SubstGlobal(query, "{% include 'header.sql.jinja' %}", "PRAGMA YqlSelect = 'force';");
            const auto expanded = std::regex_replace(std::string(query.data(), query.size()),
                std::regex(R"(\{\{\s*([a-zA-Z0-9_]+)\s*\}\})"), "`/Root/$1`");
            const auto result = session.ExplainDataQuery(constants + "\n" + TString(expanded)).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), "TPC-H perf q" << id << ": " << result.GetIssues().ToString());
            UNIT_ASSERT(!result.GetPlan().empty());
            UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
        }
        UNIT_ASSERT_VALUES_EQUAL(successes->Val(), before + 13);
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failedBefore);
    }

    void RunManyJoinsWithDecimalRanges(bool native) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        appConfig.MutableTableServiceConfig()->SetEnableNewRBOPhysicalStagePeephole(false);
        if (native) appConfig.MutableTableServiceConfig()->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
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
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, result.GetIssues().ToString());
        UNIT_ASSERT(result.GetStats() && result.GetStats()->GetPlan());
        UNIT_ASSERT(!result.GetStats()->GetPlan()->empty());
        UNIT_ASSERT_VALUES_EQUAL(successes->Val(), successesBefore + 1);
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failuresBefore);
    }
    Y_UNIT_TEST(ManyJoinsWithDecimalRanges) { RunManyJoinsWithDecimalRanges(false); }
    Y_UNIT_TEST(NativeManyJoinsWithDecimalRanges) { RunManyJoinsWithDecimalRanges(true); }

}

} // namespace
} // namespace NKikimr::NKqp
