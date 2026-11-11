#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;
using namespace NYdb::NTable;

TVector<TString> JoinResults(bool native, bool blocks, bool nullableKeys) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(native);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetUseBlockHashJoin(blocks);
    service->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED :
        NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    const TString nullability = nullableKeys ? "" : " NOT NULL";
    const auto schema = session.ExecuteSchemeQuery(
        "CREATE TABLE L (k Int32" + nullability + ", s Int32" + nullability + ", lv String, PRIMARY KEY(k,s));"
        "CREATE TABLE R (k Int64" + nullability + ", s Int32" + nullability + ", rv String, PRIMARY KEY(k,s));").GetValueSync();
    UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
    const auto seed = session.ExecuteDataQuery(R"(
        UPSERT INTO L(k,s,lv) VALUES (1,1,'l1'),(2,1,'l2'),(2,2,'l22');
        UPSERT INTO R(k,s,rv) VALUES (2,1,'r2'),(3,1,'r3'),(2,3,'r23');
    )", TTxControl::BeginTx().CommitTx()).GetValueSync();
    UNIT_ASSERT_C(seed.IsSuccess(), seed.GetIssues().ToString());
    if (nullableKeys) {
        const auto nulls = session.ExecuteDataQuery(R"(
            UPSERT INTO L(k,s,lv) VALUES (NULL,0,'ln');
            UPSERT INTO R(k,s,rv) VALUES (NULL,0,'rn');
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(nulls.IsSuccess(), nulls.GetIssues().ToString());
    }
    TVector<TString> snapshots;
    const auto execute = [&](const TString& nativeSql, const TString& legacySql = {}) {
        const auto& sql = native || legacySql.empty() ? nativeSql : legacySql;
        const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, sql);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        TString columns;
        for (const auto& column : result.GetResultSet(0).GetColumnsMeta()) columns += column.Name + ":" + column.Type.ToString() + ";";
        snapshots.push_back(nativeSql);
        snapshots.push_back(columns);
        snapshots.push_back(FormatResultSetYson(result.GetResultSet(0)));
    };
    for (const TString& keys : {TString("k"), TString("k,s")}) {
        const TString condition = keys == "k" ? "l.k = r.k" : "l.k = r.k AND l.s = r.s";
        for (const TString& kind : {TString("INNER"), TString("LEFT"), TString("RIGHT"), TString("FULL"), TString("EXCLUSION")}) {
            // Legacy SQL accepts USING keys in stars but rejects the merged
            // unqualified reference. Its explicit ON/COALESCE form provides
            // the independent value/type oracle for the native merged key.
            execute("SELECT k, l.k AS lk, r.k AS rk, l.lv AS lv, r.rv AS rv FROM L AS l " + kind +
                " JOIN R AS r USING(" + keys + ") ORDER BY k, lk, rk, lv, rv",
                "SELECT COALESCE(l.k,r.k) AS k, l.k AS lk, r.k AS rk, l.lv AS lv, r.rv AS rv FROM L AS l " + kind +
                " JOIN R AS r ON " + condition + " ORDER BY k, lk, rk, lv, rv");
        }
        for (const TString& kind : {TString("LEFT SEMI"), TString("LEFT ONLY"), TString("RIGHT SEMI"), TString("RIGHT ONLY")}) {
            const TString side = kind.StartsWith("LEFT") ? "l" : "r";
            execute("SELECT k, s FROM L AS l " + kind + " JOIN R AS r USING(" + keys + ") ORDER BY k, s",
                "SELECT " + side + ".k AS k, " + side + ".s AS s FROM L AS l " + kind +
                " JOIN R AS r ON " + condition + " ORDER BY k, s");
        }
    }
    execute("SELECT k, s, lv, rv FROM (SELECT * FROM L AS l FULL JOIN R AS r USING(k,s)) AS d ORDER BY k, s, lv, rv");
    execute("SELECT k, s, lv FROM (SELECT l.* FROM L AS l FULL JOIN R AS r USING(k,s)) AS d ORDER BY k, s, lv");
    execute("SELECT k, s, rv FROM (SELECT r.* FROM L AS l FULL JOIN R AS r USING(k,s)) AS d ORDER BY k, s, rv");
    execute("SELECT k, COUNT(*) AS n FROM L AS l FULL JOIN R AS r USING(k) GROUP BY k ORDER BY k",
        "SELECT k, COUNT(*) AS n FROM (SELECT COALESCE(l.k,r.k) AS k FROM L AS l FULL JOIN R AS r ON l.k = r.k) AS d GROUP BY k ORDER BY k");
    execute("SELECT k FROM L AS l FULL JOIN R AS r USING(k) WHERE k > 1 ORDER BY k",
        "SELECT k FROM (SELECT COALESCE(l.k,r.k) AS k FROM L AS l FULL JOIN R AS r ON l.k = r.k) AS d WHERE k > 1 ORDER BY k");
    return snapshots;
}
}

Y_UNIT_TEST_SUITE(KqpNativeJoinUsingRuntime) {
    Y_UNIT_TEST_QUAD(ValuesTypesAndStarsMatchLegacyOnAndCoalesce, Blocks, NullableKeys) {
        const auto expected = JoinResults(false, Blocks, NullableKeys);
        const auto actual = JoinResults(true, Blocks, NullableKeys);
        UNIT_ASSERT_VALUES_EQUAL(actual.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) UNIT_ASSERT_VALUES_EQUAL_C(actual[i], expected[i], i);
    }

    Y_UNIT_TEST(HandoffUsesOrdinaryNativeJoinsAndMaps) {
        using namespace NFrontend;
        NTests::TIdTestContext fixture;
        const auto nodes = fixture.ExprCtx.NodeAllocationCounter;
        const auto types = fixture.ExprCtx.TypeNodes.size();
        const auto query = CompileNativeToRbo(
            "SELECT k, l.k AS lk, r.k AS rk FROM L AS l FULL JOIN R AS r USING(k)",
            [](std::string_view name, TSourcePosition) {
                const auto path = TString(name);
                return TNativeTable{NFrontend::TCatalogTable{std::string(name), {{"k", {name == "L" ? ETypeKind::Int32 : ETypeKind::Int64, true}}}},
                    TRboTable{path, "1:2", "", "3"}, NYql::EStorageType::RowStorage};
            }, fixture.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 1);
        const auto& root = query.Roots[0];
        UNIT_ASSERT(root->ComputeTypes(fixture.RboCtx) == IGraphTransformer::TStatus::Ok);
        std::size_t joins = 0;
        for (const auto& item : *root) {
            joins += item.Current->Kind == EOperator::Join;
            UNIT_ASSERT(item.Current->NativeSchema);
            for (const auto& expression : item.Current->GetExpressions()) UNIT_ASSERT(expression.get().IsNative());
        }
        UNIT_ASSERT_VALUES_EQUAL(joins, 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.TypeNodes.size(), types);
    }
}
}
