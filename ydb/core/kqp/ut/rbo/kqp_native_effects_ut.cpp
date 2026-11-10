#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;
using namespace NYdb::NTable;

TVector<TString> WriteResults(bool native, bool peephole) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(native);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    service->SetEnableNewRBOPhysicalStagePeephole(peephole);
    service->SetEnableIndexStreamWrite(true);
    TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    const auto schema = session.ExecuteSchemeQuery(R"(
        CREATE TABLE Target (id Uint64, value Int32, name Utf8, PRIMARY KEY(id));
        CREATE TABLE Indexed (id Uint64, value Int32, name Utf8, PRIMARY KEY(id), INDEX by_value GLOBAL ON (value));
    )").GetValueSync();
    UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
    TVector<TString> snapshots;
    const auto execute = [&](const TString& sql, const TParams& params) {
        const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx(), params).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, sql);
        snapshots.push_back(ToString(result.GetResultSets().size()));
        for (const auto& rows : result.GetResultSets()) {
            TString columns;
            for (const auto& column : rows.GetColumnsMeta()) columns += column.Name + ":" + column.Type.ToString() + ";";
            snapshots.push_back(columns);
            snapshots.push_back(FormatResultSetYson(rows));
        }
    };
    const auto empty = TParamsBuilder().Build();
    for (const TString& table : {TString("Target"), TString("Indexed")}) {
        execute("INSERT INTO " + table + " (id, value, name) VALUES (1ul, 10, 'one'u) RETURNING name, id, value", empty);
        execute("UPSERT INTO " + table + " (id, value, name) VALUES (2ul, 20, 'two'u), (3ul, 30, 'three'u)", empty);
        execute("REPLACE INTO " + table + " (id, value) VALUES (2ul, 21) RETURNING id, name, value", empty);
        execute("UPDATE " + table + " SET value = value + 1 WHERE id = 1ul RETURNING value, id, name", empty);
        execute("UPDATE " + table + " ON (id, value) VALUES (3ul, 31) RETURNING id, value", empty);
        execute("SELECT id, value, name FROM " + table + " ORDER BY id", empty);
        execute("DELETE FROM " + table + " WHERE id = 2ul RETURNING name, id, value", empty);
        execute("DELETE FROM " + table + " ON (id) VALUES (3ul) RETURNING id, value", empty);
        execute("SELECT id, value, name FROM " + table + " ORDER BY id", empty);
    }
    auto rows = TParamsBuilder().AddParam("$rows").BeginList()
        .AddListItem().BeginStruct().AddMember("id").Uint64(4).AddMember("value").Int32(40).EndStruct()
        .AddListItem().BeginStruct().AddMember("id").Uint64(5).AddMember("value").Int32(50).EndStruct()
        .EndList().Build().Build();
    execute("DECLARE $rows AS List<Struct<id:Uint64,value:Int32>>; UPSERT INTO Target SELECT * FROM AS_TABLE($rows)", rows);
    execute("INSERT INTO Indexed SELECT id, value, name FROM Target WHERE id > 1ul", empty);
    execute("SELECT id, value, name FROM Indexed ORDER BY id", empty);
    execute(R"(
        SELECT COUNT(*) AS before_count FROM Target;
        UPSERT INTO Target (id, value) VALUES (6ul, 60) RETURNING value, id;
        SELECT COUNT(*) AS same_snapshot_count FROM Target;
        UPSERT INTO Target (id, value) VALUES (7ul, 70);
    )", empty);
    execute("SELECT id, value, name FROM Target ORDER BY id", empty);
    return snapshots;
}
}

Y_UNIT_TEST_SUITE(KqpNativeEffects) {
    Y_UNIT_TEST_TWIN(WritesAndReturningMatchLegacyStorageSemantics, Peephole) {
        const auto expected = WriteResults(false, Peephole);
        const auto actual = WriteResults(true, Peephole);
        UNIT_ASSERT_VALUES_EQUAL(actual.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) UNIT_ASSERT_VALUES_EQUAL_C(actual[i], expected[i], i);
    }

    Y_UNIT_TEST(HandoffPreservesEffectsAndResultOrderWithoutYqlExpressions) {
        using namespace NFrontend;
        NTests::TIdTestContext fixture;
        const auto nodes = fixture.ExprCtx.NodeAllocationCounter;
        const auto types = fixture.ExprCtx.TypeNodes.size();
        auto query = CompileNativeToRbo(R"(
            UPSERT INTO t (id, value) VALUES (1ul, 2) RETURNING value, id;
            SELECT id FROM t;
            UPDATE t SET value = value + 1 WHERE id = 2ul;
            DELETE FROM t ON (id) VALUES (3ul) RETURNING id;
        )", [](std::string_view, TSourcePosition) {
            return TNativeTable{{"/Root/t", {{"id", {ETypeKind::Uint64, true}}, {"value", {ETypeKind::Int32, true}}},
                TWriteSchema{{"id"}, {}, true}}, TRboTable{"/Root/t", "1:2", "", "3"}, NYql::EStorageType::RowStorage};
        }, fixture.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 4);
        UNIT_ASSERT(!query.Roots[0]->PlanProps.WithEffects);
        UNIT_ASSERT_VALUES_EQUAL(*query.Roots[0]->PlanProps.ResultIndex, 1);
        UNIT_ASSERT_VALUES_EQUAL(*query.Roots[1]->PlanProps.ResultIndex, 0);
        UNIT_ASSERT(!query.Roots[2]->PlanProps.ResultIndex);
        UNIT_ASSERT_VALUES_EQUAL(*query.Roots[3]->PlanProps.ResultIndex, 2);
        UNIT_ASSERT(query.Execution.DiscardResults == (TVector<bool>{false, false, false}));
        UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations.size(), 4);
        for (const auto& root : query.Roots) {
            UNIT_ASSERT(root->ComputeTypes(fixture.RboCtx) == IGraphTransformer::TStatus::Ok);
            for (const auto& item : *root) {
                UNIT_ASSERT(item.Current->NativeSchema);
                for (const auto& expression : item.Current->GetExpressions()) UNIT_ASSERT(expression.get().IsNative());
            }
        }
        const auto& effect = CastOperator<TOpTableEffect>(*query.Roots[1]->GetInput());
        UNIT_ASSERT(effect.EffectType == EEffectType::UpsertRowsIndex);
        UNIT_ASSERT_VALUES_EQUAL(effect.GetColumns().Items().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(effect.GetReturningColumns().Items()[0].second, "value");
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.TypeNodes.size(), types);
    }
}
}
