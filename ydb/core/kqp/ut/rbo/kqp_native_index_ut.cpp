#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

namespace NKikimr::NKqp {
namespace {
TKikimrSettings Settings(bool native) {
    NKikimrConfig::TAppConfig app;
    auto* config = app.MutableTableServiceConfig();
    config->SetEnableNewRBO(native);
    config->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                                        : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    config->SetEnableFallbackToYqlOptimizer(false);
    config->SetEnableIndexStreamWrite(true);
    auto settings = TKikimrSettings(app).SetWithSampleTables(false);
    settings.LogSettings = TTestLogSettings();
    settings.LogSettings->DefaultLogPriority = NActors::NLog::PRI_CRIT;
    return settings;
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeIndexViews) {
    Y_UNIT_TEST(HandoffUsesOnlyNativeReadsAndLookups) {
        using namespace NFrontend;
        struct TCase { const char* Sql; size_t Reads; size_t Lookups; bool IndexFilter = false; };
        for (const auto& test : std::vector<TCase>{
                {"SELECT id, g FROM t VIEW by_g", 1, 0},
                {"SELECT COUNT(*) FROM t VIEW by_g", 1, 0},
                {"SELECT id, payload FROM t VIEW by_g", 1, 1},
                {"SELECT payload FROM t VIEW by_g WHERE g = 1", 1, 1, true},
                {"SELECT id FROM t VIEW by_g WHERE payload = 'x'", 1, 1},
                {"SELECT (SELECT MAX(payload) FROM t VIEW by_g)", 1, 1},
                {"SELECT a.id, b.payload FROM t VIEW by_g a JOIN t VIEW by_g b ON a.id = b.id", 2, 1},
                {"SELECT a.id FROM t VIEW by_g a WHERE EXISTS (SELECT b.id FROM t VIEW by_g b "
                 "WHERE b.g = a.g AND b.payload = 'x')", 2, 1}}) {
            NTests::TIdTestContext f;
            const auto nodes = f.ExprCtx.NodeAllocationCounter;
            const auto types = f.ExprCtx.TypeNodes.size();
            size_t resolutions = 0;
            auto query = CompileNativeToRbo(test.Sql, [&](std::string_view name, TSourcePosition) {
                UNIT_ASSERT_VALUES_EQUAL(name, "t");
                TNativeTable table{{"t", {{"id", {ETypeKind::Int64, true}}, {"g", {ETypeKind::Int32, true}},
                    {"payload", {ETypeKind::String, true}}}}, TRboTable{"/Root/t", "1:2", "", "3"}};
                table.ResolveIndex = [&](std::string_view index, TSourcePosition) {
                    ++resolutions;
                    UNIT_ASSERT_VALUES_EQUAL(index, "by_g");
                    TRboTable impl{"/Root/t/by_g/indexImplTable", "1:3", "", "3"};
                    impl.NativeColumns = std::make_shared<TRboTable::TColumns>(TRboTable::TColumns{
                        {"id", {ETypeKind::Int64, true}}, {"g", {ETypeKind::Int32, true}}});
                    return TNativeIndex{std::move(impl), {"id"}};
                };
                return table;
            }, f.ExprCtx);
            UNIT_ASSERT_VALUES_EQUAL_C(resolutions, 1, test.Sql);
            UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations[0].GetTable(), "/Root/t");
            size_t reads = 0, lookups = 0;
            for (const auto& root : query.Roots) {
                UNIT_ASSERT_C(root->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok, test.Sql);
                for (const auto& item : IterateSubtreeWithSubplans(root.Get(), root->PlanProps)) {
                    if (item.Current->Kind == EOperator::Source) {
                        ++reads;
                        const auto& read = CastOperator<TOpRead>(*item.Current);
                        UNIT_ASSERT_VALUES_EQUAL(read.GetTable().Path, "/Root/t/by_g/indexImplTable");
                    } else if (item.Current->Kind == EOperator::TableLookup) {
                        ++lookups;
                        const auto& lookup = CastOperator<TOpTableLookup>(*item.Current);
                        UNIT_ASSERT_VALUES_EQUAL(lookup.Table.Path, "/Root/t");
                        UNIT_ASSERT(lookup.AllowNullKeys);
                        UNIT_ASSERT_VALUES_EQUAL_C(lookup.GetInput()->Kind == EOperator::Filter, test.IndexFilter, test.Sql);
                        UNIT_ASSERT(lookup.LookupKeys.Unordered().IsSubsetOf(lookup.GetInput()->GetOutputIUs()));
                        UNIT_ASSERT(!lookup.GetColumns().Contains(lookup.LookupKeys.Items().front().first));
                    }
                }
            }
            UNIT_ASSERT_VALUES_EQUAL_C(reads, test.Reads, test.Sql);
            UNIT_ASSERT_VALUES_EQUAL_C(lookups, test.Lookups, test.Sql);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
        }
    }

    Y_UNIT_TEST_TWIN(CoveringAndFetchedRowsMatchLegacy, Native) {
        using namespace NYdb;
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto scheme = session.ExecuteSchemeQuery(R"(
            CREATE TABLE t (id Int64, g Int32, payload String, PRIMARY KEY(id), INDEX ByG GLOBAL ON(g));
        )").GetValueSync();
        UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
        const auto seed = session.ExecuteDataQuery(R"(
            UPSERT INTO t (id, g, payload) VALUES
                (NULL, 0, 'nullpk'), (1, 1, 'one'), (2, 1, 'two'), (3, 2, 'three');
        )", NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(seed.IsSuccess(), seed.GetIssues().ToString());
        struct TCase { TString Sql; TString Expected; bool Correlated = false; };
        for (const auto& [sql, expected, correlated] : TVector<TCase>{
                {"SELECT id, g FROM t VIEW ByG ORDER BY id", R"([[#;[0]];[[1];[1]];[[2];[1]];[[3];[2]]])"},
                {"SELECT COUNT(*) FROM t VIEW ByG", "[[4u]]"},
                {"SELECT id, payload FROM t VIEW ByG ORDER BY id",
                    R"([[#;["nullpk"]];[[1];["one"]];[[2];["two"]];[[3];["three"]]])"},
                {"SELECT id, payload FROM t VIEW ByG WHERE g = 1 ORDER BY id", R"([[[1];["one"]];[[2];["two"]]])"},
                {"SELECT payload FROM t VIEW ByG WHERE id IS NULL", R"([[["nullpk"]]])"},
                {"SELECT id FROM t VIEW ByG WHERE payload = 'two'", R"([[[2]]])"},
                {"SELECT (SELECT MAX(payload) FROM t VIEW ByG) AS p", R"([[["two"]]])"},
                {"SELECT b.payload FROM t a JOIN t VIEW ByG b ON a.id = b.id WHERE a.id = 2", R"([[["two"]]])"},
                {"$q = SELECT id, payload FROM t VIEW ByG; "
                 "SELECT a.payload, b.payload FROM $q a JOIN $q b ON a.id = b.id ORDER BY a.payload",
                    R"([[["one"];["one"]];[["three"];["three"]];[["two"];["two"]]])"},
                {"SELECT a.id FROM t VIEW ByG a WHERE EXISTS (SELECT b.id FROM t VIEW ByG b "
                 "WHERE b.g = a.g AND b.payload = 'two') ORDER BY a.id", R"([[[1]];[[2]]])", true}}) {
            const auto result = session.ExecuteDataQuery(sql, NYdb::NTable::TTxControl::BeginTx().CommitTx(),
                NYdb::NTable::TExecDataQuerySettings().CollectQueryStats(NYdb::NTable::ECollectQueryStatsMode::Full)).GetValueSync();
            if (!Native && correlated) {
                // The legacy SQL compiler cannot capture this outer source.
                // The native arm still checks its actual rows independently.
                UNIT_ASSERT(!result.IsSuccess());
                UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "Member not found: a");
                continue;
            }
            UNIT_ASSERT_C(result.IsSuccess(), TStringBuilder() << sql << ": " << result.GetIssues().ToString());
            UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
            if constexpr (Native) {
                UNIT_ASSERT(result.GetStats());
                UNIT_ASSERT(result.GetStats()->GetAst());
                UNIT_ASSERT_STRING_CONTAINS(TString(*result.GetStats()->GetAst()), "native frontend:");
            }
            CompareYson(expected, FormatResultSetYson(result.GetResultSet(0)));
            if (sql == "SELECT id, payload FROM t VIEW ByG WHERE g = 1 ORDER BY id") {
                THashMap<TString, ui64> rows;
                for (const auto& phase : TProtoAccessor::GetProto(*result.GetStats()).query_phases())
                    for (const auto& access : phase.table_access()) rows[TString(access.name())] += access.reads().rows();
                UNIT_ASSERT_VALUES_EQUAL(rows.at("/Root/t/ByG/indexImplTable"), 2);
                UNIT_ASSERT_VALUES_EQUAL(rows.at("/Root/t"), 2);
            }
        }
    }

    Y_UNIT_TEST_TWIN(AuthorizationUsesTheMainTable, Native) {
        using namespace NYdb;
        TKikimrRunner kikimr(Settings(Native));
        auto admin = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto scheme = admin.ExecuteSchemeQuery(R"(
            CREATE TABLE t (id Int64, value Int32, payload String, PRIMARY KEY(id), INDEX ByValue GLOBAL ON(value));
        )").GetValueSync();
        UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
        auto permissions = kikimr.GetSchemeClient().ModifyPermissions("/Root/t", NYdb::NScheme::TModifyPermissionsSettings()
            .AddGrantPermissions(NYdb::NScheme::TPermissions("reader@builtin", {"ydb.deprecated.describe_schema", "ydb.deprecated.select_row"})))
            .GetValueSync();
        UNIT_ASSERT_C(permissions.IsSuccess(), permissions.GetIssues().ToString());
        TDriver driver(TDriverConfig().SetEndpoint(kikimr.GetEndpoint()).SetDatabase("/Root").SetAuthToken("reader@builtin"));
        NYdb::NTable::TTableClient client(driver);
        for (const bool allowed : {true, false}) {
            if (!allowed) {
                permissions = kikimr.GetSchemeClient().ModifyPermissions("/Root/t", NYdb::NScheme::TModifyPermissionsSettings()
                    .AddRevokePermissions(NYdb::NScheme::TPermissions("reader@builtin", {"ydb.deprecated.select_row"}))).GetValueSync();
                UNIT_ASSERT_C(permissions.IsSuccess(), permissions.GetIssues().ToString());
            }
            auto session = client.CreateSession().GetValueSync().GetSession();
            for (const auto* sql : {"SELECT id FROM t VIEW ByValue", "SELECT payload FROM t VIEW ByValue"}) {
                const auto result = session.ExecuteDataQuery(sql, NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
                if (allowed) UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
                else {
                    // Revoking access may invalidate a cached plan during table
                    // resolution (ABORTED), or be rejected before execution.
                    UNIT_ASSERT(result.GetStatus() == EStatus::ABORTED || result.GetStatus() == EStatus::UNAUTHORIZED);
                    UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "AccessDenied");
                }
                UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
            }
        }
    }

    Y_UNIT_TEST_TWIN(MissingIndexesAreErrors, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto scheme = session.ExecuteSchemeQuery(R"(
            CREATE TABLE t (id Int64, value Int32, PRIMARY KEY(id), INDEX ByValue GLOBAL ON(value));
            CREATE TABLE plain (id Int64, PRIMARY KEY(id));
        )").GetValueSync();
        UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
        for (const auto& [sql, legacyIssue] : TVector<std::pair<TString, TString>>{
                {"SELECT id FROM t VIEW Missing", "Required global index not found"},
                {"SELECT id FROM plain VIEW Missing", "No global indexes for table"}}) {
            const auto result = session.ExplainDataQuery(sql).GetValueSync();
            UNIT_ASSERT_C(!result.IsSuccess(), sql);
            UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
            UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), Native ? TString("Index not found") : legacyIssue);
        }
    }
}
} // namespace NKikimr::NKqp
