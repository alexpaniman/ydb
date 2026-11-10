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
        execute("UPDATE " + table + " SET value = 99 WHERE id = 99ul RETURNING id, value", empty);
        execute("UPDATE " + table + " ON (id, value) VALUES (99ul, 99) RETURNING id, value", empty);
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
    execute("SELECT id, value FROM `/Root/Indexed/by_value/indexImplTable` ORDER BY id", empty);
    if (native) {
        const auto rejected = session.ExplainDataQuery(
            "UPSERT INTO `/Root/Indexed/by_value/indexImplTable` (value, id) VALUES (99, 99ul)").GetValueSync();
        UNIT_ASSERT(!rejected.IsSuccess());
        UNIT_ASSERT_STRING_CONTAINS(rejected.GetIssues().ToString(), "table does not support native writes");
        UNIT_ASSERT_C(rejected.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos,
            rejected.GetIssues().ToString());
    }
    execute(R"(
        SELECT COUNT(*) AS before_count FROM Target;
        UPSERT INTO Target (id, value) VALUES (6ul, 60) RETURNING value, id;
        SELECT COUNT(*) AS after_count FROM Target;
        UPSERT INTO Target (id, value) VALUES (7ul, 70);
    )", empty);
    execute("SELECT id, value, name FROM Target ORDER BY id", empty);
    execute(R"(
        UPSERT INTO Target (id, value) VALUES (8ul, 80);
        UPDATE Target SET value = value + 1 WHERE id = 8ul RETURNING id, value;
        SELECT id, value FROM Target WHERE id = 8ul;
        DELETE FROM Target WHERE id = 8ul RETURNING id, value;
        SELECT COUNT(*) AS after_delete FROM Target WHERE id = 8ul;
    )", empty);
    execute(R"(
        UPSERT INTO Target (id, value) VALUES (9ul, 90);
        $q = SELECT id, value FROM Target WHERE id = 9ul;
        SELECT id, value FROM $q;
        SELECT id, value FROM $q;
    )", empty);
    return snapshots;
}
}

Y_UNIT_TEST_SUITE(KqpNativeEffects) {
    Y_UNIT_TEST(PredicateUpdateMembershipRangesStayNative) {
        using namespace NFrontend;
        NTests::TIdTestContext fixture;
        auto metadata = MakeIntrusive<TKikimrTableMetadata>();
        metadata->DoesExist = true;
        metadata->Name = "/Root/t";
        metadata->Kind = EKikimrTableKind::Datashard;
        metadata->KeyColumnNames = {"id"};
        fixture.Tables->GetOrAddTable("ut", "/Root", metadata->Name).Metadata = metadata;
        const auto nodes = fixture.ExprCtx.NodeAllocationCounter;
        const auto types = fixture.ExprCtx.TypeNodes.size();
        auto query = CompileNativeToRbo(R"(
            DECLARE $a AS Uint64; DECLARE $b AS Uint64;
            UPDATE t SET value = value + 1 WHERE id IN [$a, $b];
        )", [](std::string_view, TSourcePosition) {
            return TNativeTable{{"/Root/t", {{"id", {ETypeKind::Uint64, true}}, {"value", {ETypeKind::Int32, true}}},
                TWriteSchema{{"id"}, {}, false}}, TRboTable{"/Root/t", "1:2", "", "3"}};
        }, fixture.ExprCtx);
        auto& root = *query.Roots.front();
        UNIT_ASSERT(root.ComputeTypes(fixture.RboCtx) == IGraphTransformer::TStatus::Ok);
        TIntrusivePtr<IOperator> filter;
        for (const auto& item : root) if (item.Current->Kind == EOperator::Filter) filter = item.Current;
        UNIT_ASSERT(filter);
        const auto pushed = TPushRangesRule(NYql::EStorageType::RowStorage).SimpleMatchAndApply(filter, fixture.RboCtx, root.PlanProps);
        UNIT_ASSERT(pushed->Kind == EOperator::Source || pushed->Kind == EOperator::Filter);
        const auto read = pushed->Kind == EOperator::Source ? pushed : CastOperator<TOpFilter>(*pushed).GetInput();
        UNIT_ASSERT(read->Kind == EOperator::Source);
        const auto& ranges = CastOperator<TOpRead>(*read).RangeInfo;
        UNIT_ASSERT(ranges && ranges->NativeRanges);
        UNIT_ASSERT_VALUES_EQUAL(ranges->NativeRanges->size(), 2);
        for (const auto& range : *ranges->NativeRanges) {
            UNIT_ASSERT_VALUES_EQUAL(range.Lower.Values.size(), 1);
            UNIT_ASSERT(range.Lower.Values.front().IsNative());
            const auto& value = range.Lower.Values.front();
            UNIT_ASSERT((*value.GetNativeArena())[value.GetNativeRoot()].Op == EOpcode::Parameter);
        }
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.TypeNodes.size(), types);
    }

    Y_UNIT_TEST_TWIN(PredicateUpdateHandoffPreservesExistingRowKnowledge, StreamWrite) {
        using namespace NFrontend;
        for (const bool indexed : {false, true}) {
            NTests::TIdTestContext fixture;
            fixture.Config->SetEnableIndexStreamWrite(StreamWrite);
            const auto nodes = fixture.ExprCtx.NodeAllocationCounter;
            const auto types = fixture.ExprCtx.TypeNodes.size();
            auto query = CompileNativeToRbo(R"(
                UPDATE t SET value = value + 1 WHERE id = 1ul RETURNING value, id;
                UPDATE t ON (id, value) VALUES (2ul, 20) RETURNING id, value;
            )", [indexed](std::string_view, TSourcePosition) {
                return TNativeTable{{"/Root/t", {{"id", {ETypeKind::Uint64, true}}, {"value", {ETypeKind::Int32, true}}},
                    TWriteSchema{{"id"}, {}, indexed}}, TRboTable{"/Root/t", "1:2", "", "3"}};
            }, fixture.ExprCtx, {}, {}, fixture.Config.Get());
            UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 2);
            const auto& predicate = CastOperator<TOpTableEffect>(*query.Roots[0]->GetInput());
            const auto& on = CastOperator<TOpTableEffect>(*query.Roots[1]->GetInput());
            UNIT_ASSERT(predicate.EffectType == (indexed ? EEffectType::UpdateRowsIndex : EEffectType::UpsertRows));
            UNIT_ASSERT(on.EffectType == (indexed ? EEffectType::UpdateRowsIndex : EEffectType::UpdateRows));
            UNIT_ASSERT(!on.Options.Settings);
            if (indexed) {
                UNIT_ASSERT(!predicate.Options.Settings);
            } else {
                const TVector<TVector<TString>> expected{{"Mode", StreamWrite ? "update_conditional" : "upsert"},
                    {"IsUpdate"}, {"IsConditionalUpdate"}};
                UNIT_ASSERT(predicate.Options.Settings && *predicate.Options.Settings == expected);
            }
            UNIT_ASSERT_VALUES_EQUAL(predicate.GetReturningColumns().Items()[0].second, "value");
            UNIT_ASSERT_VALUES_EQUAL(on.GetReturningColumns().Items()[0].second, "id");
            UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations[0].GetOperation(), static_cast<ui32>(NYql::TYdbOperation::Select));
            UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations[1].GetOperation(), static_cast<ui32>(NYql::TYdbOperation::Update));
            UNIT_ASSERT_VALUES_EQUAL(query.Execution.TableOperations[2].GetOperation(), static_cast<ui32>(NYql::TYdbOperation::UpdateOn));
            for (const auto& root : query.Roots) {
                UNIT_ASSERT(root->ComputeTypes(fixture.RboCtx) == IGraphTransformer::TStatus::Ok);
                for (const auto& item : *root) {
                    UNIT_ASSERT(item.Current->NativeSchema);
                    for (const auto& expression : item.Current->GetExpressions()) UNIT_ASSERT(expression.get().IsNative());
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.NodeAllocationCounter, nodes);
            UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.TypeNodes.size(), types);
        }
    }

    Y_UNIT_TEST_TWIN(InsertRevertNoOpPreservesReturningAndOtherStatements, Peephole) {
        NKikimrConfig::TAppConfig config;
        auto* service = config.MutableTableServiceConfig();
        service->SetEnableNewRBO(true);
        service->SetEnableFallbackToYqlOptimizer(false);
        service->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
        service->SetEnableNewRBOPhysicalStagePeephole(Peephole);
        service->SetEnableIndexStreamWrite(true);
        TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto created = session.ExecuteSchemeQuery(R"(
            CREATE TABLE Target (id Uint64, value String, PRIMARY KEY(id), INDEX by_value GLOBAL UNIQUE ON(value));
            CREATE TABLE Audit (id Uint64, PRIMARY KEY(id));
        )").GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
        auto execute = [&](const TString& sql) {
            auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
            UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos,
                result.GetIssues().ToString());
            return result;
        };
        auto inserted = execute("INSERT OR REVERT INTO Target (id, value) VALUES (1ul, 'old') RETURNING value, id;");
        CompareYson(R"([[["old"];[1u]]])", FormatResultSetYson(inserted.GetResultSet(0)));
        for (const TString& rows : {TString("(2ul, 'a'), (2ul, 'b'), (3ul, 'c')"), TString("(4ul, 'old'), (5ul, 'new')")}) {
            auto reverted = execute("INSERT OR REVERT INTO Target (id, value) VALUES " + rows + " RETURNING value, id;");
            CompareYson("[]", FormatResultSetYson(reverted.GetResultSet(0)));
        }
        auto mixed = execute(R"(
            UPSERT INTO Audit (id) VALUES (1ul);
            INSERT OR REVERT INTO Target (id, value) VALUES (1ul, 'conflict'), (9ul, 'unused');
            SELECT COUNT(*) FROM Audit;
            SELECT id, value FROM Target;
        )");
        CompareYson("[[1u]]", FormatResultSetYson(mixed.GetResultSet(0)));
        CompareYson(R"([[[1u];["old"]]])", FormatResultSetYson(mixed.GetResultSet(1)));
    }

    Y_UNIT_TEST_TWIN(InsertRevertChecksAreNativeBeforeOptimization, UniqueIndex) {
        using namespace NFrontend;
        NTests::TIdTestContext fixture;
        const auto nodes = fixture.ExprCtx.NodeAllocationCounter;
        const auto types = fixture.ExprCtx.TypeNodes.size();
        auto query = CompileNativeToRbo(R"(
            INSERT OR REVERT INTO t (id, value) VALUES (NULL, NULL), (1ul, 'same') RETURNING value, id;
        )", [](std::string_view, TSourcePosition) {
            TNativeTable table{{"/Root/t", {{"id", {ETypeKind::Uint64, true}}, {"value", {ETypeKind::String, true}}},
                TWriteSchema{{"id"}, {}, UniqueIndex}}, TRboTable{"/Root/t", "1:2", "", "3"}};
            if constexpr (UniqueIndex) {
                TRboTable index{"/Root/t/by_value/indexImplTable", "1:3", "", "3"};
                auto columns = std::make_shared<TRboTable::TColumns>();
                columns->emplace("value", TType{ETypeKind::String, true});
                index.NativeColumns = std::move(columns);
                table.UniqueConstraints.push_back({std::move(index), {"value"}});
            }
            return table;
        }, fixture.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 1);
        auto& root = *query.Roots.front();
        UNIT_ASSERT(root.ComputeTypes(fixture.RboCtx) == IGraphTransformer::TStatus::Ok);
        const auto& effect = CastOperator<TOpTableEffect>(*root.GetInput());
        UNIT_ASSERT_VALUES_EQUAL(*effect.Options.OnConflict, "abort");
        UNIT_ASSERT(effect.GetInput()->Kind == EOperator::Join);
        UNIT_ASSERT_VALUES_EQUAL(effect.GetReturningColumns().Items()[0].second, "value");
        size_t lookups = 0;
        for (const auto& item : root) {
            UNIT_ASSERT(item.Current->NativeSchema);
            for (const auto& expression : item.Current->GetExpressions()) UNIT_ASSERT(expression.get().IsNative());
            if (item.Current->Kind == EOperator::TableLookup) {
                ++lookups;
                const auto& lookup = CastOperator<TOpTableLookup>(*item.Current);
                UNIT_ASSERT(lookup.AllowNullKeys);
                UNIT_ASSERT_VALUES_EQUAL(lookup.LookupKeys.Items().size(), 1);
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(lookups, UniqueIndex ? 2 : 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.TypeNodes.size(), types);
    }

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
        UNIT_ASSERT(query.Roots[0]->PlanProps.WithEffects);
        UNIT_ASSERT_VALUES_EQUAL(*query.Roots[0]->PlanProps.ResultIndex, 0);
        UNIT_ASSERT_VALUES_EQUAL(*query.Roots[1]->PlanProps.ResultIndex, 1);
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
        const auto& effect = CastOperator<TOpTableEffect>(*query.Roots[0]->GetInput());
        UNIT_ASSERT(effect.EffectType == EEffectType::UpsertRowsIndex);
        UNIT_ASSERT_VALUES_EQUAL(effect.GetColumns().Items().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(effect.GetReturningColumns().Items()[0].second, "value");
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(fixture.ExprCtx.TypeNodes.size(), types);
    }
}
}
