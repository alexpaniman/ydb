#include "kqp_rbo_test_helpers.h"

namespace NKikimr::NKqp {

Y_UNIT_TEST_SUITE(KqpNativeRangePolicy) {
    Y_UNIT_TEST(AutomaticIndexEligibilityPreservesExistingOptimizerPolicy) {
        using K = NFrontend::ETypeKind;
        using O = NFrontend::EOpcode;
        using I = TIndexDescription::EType;
        using S = TIndexDescription::EIndexState;
        // The pre-migration IsSelectableIndex policy (d11af71c6d0) is broader
        // than explicit native VIEW admission. Synthetic row metadata isolates
        // that policy from the storage-specific implementation of each index.
        const std::pair<I, bool> cases[]{
            {I::GlobalSync, true}, {I::GlobalAsync, false}, {I::GlobalSyncUnique, true},
            {I::GlobalSyncVectorKMeansTree, true}, {I::GlobalFulltextPlain, true},
            {I::GlobalFulltextRelevance, true}, {I::GlobalFulltextCompact, true},
            {I::GlobalFulltextCompactRelevance, true}, {I::GlobalJson, false},
            {I::GlobalJsonCompact, false}, {I::LocalMinMax, false},
            {I::LocalBloomFilter, false}, {I::LocalBloomNgramFilter, false},
        };
        for (const auto [kind, eligible] : cases) for (const auto state : {S::Invalid, S::Ready, S::NotReady, S::WriteOnly})
        for (const bool disabled : {false, true}) {
            NTests::TIdTestContext f;
            f.Config->OptDisableAutoIndexSelection = disabled;
            const auto pk = f.Id("id"), key = f.Id("g");
            auto table = MakeIntrusive<TKikimrTableMetadata>();
            table->Name = "/Root/table";
            table->DoesExist = true;
            table->Kind = EKikimrTableKind::Datashard;
            table->KeyColumnNames = {"id"};
            for (const auto* name : {"id", "g"}) {
                auto& column = table->Columns[name]; column.Type = "Int32"; column.NotNull = true;
            }
            auto index = MakeIntrusive<TKikimrTableMetadata>();
            index->Name = "/Root/table/by_g/indexImplTable";
            index->DoesExist = true;
            index->Kind = EKikimrTableKind::Datashard;
            index->KeyColumnNames = {"g", "id"};
            index->Columns = table->Columns;
            table->Indexes.emplace_back("by_g", TVector<TString>{"g"}, TVector<TString>{},
                kind, state, 1, 1, 1, std::monostate{});
            table->ImplTables.push_back(index);
            f.Tables->GetOrAddTable("ut", "/Root", table->Name).Metadata = table;
            f.Tables->GetOrAddTable("ut", "/Root", index->Name).Metadata = index;
            auto read = f.Read({pk, key});
            f.SetType(*read);
            const auto arena = f.Props.NativeExpressions;
            const auto predicate = arena->Binary(O::Equal, arena->Column(key, {K::Int32}),
                arena->Literal({K::Int32}, std::int64_t{2}));
            auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, TExpression::FromNative(arena, predicate, &f.Props));
            const auto nodes = f.ExprCtx.NodeAllocationCounter, types = f.ExprCtx.TypeNodes.size();
            const auto result = TPushRangesRule(NYql::EStorageType::RowStorage).SimpleMatchAndApply(filter, f.RboCtx, f.Props);
            const bool expected = eligible && state == S::Ready && !disabled;
            UNIT_ASSERT_VALUES_EQUAL_C(result != filter, expected,
                "kind=" << unsigned(kind) << " state=" << unsigned(state) << " disabled=" << disabled);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
            if (!expected) continue;
            UNIT_ASSERT(result->Kind == EOperator::Source);
            const auto& selected = CastOperator<TOpRead>(*result);
            UNIT_ASSERT_VALUES_EQUAL(selected.TableRef.Path, index->Name);
            UNIT_ASSERT(selected.RangeInfo && selected.RangeInfo->NativeRanges);
            UNIT_ASSERT_VALUES_EQUAL(selected.RangeInfo->NativeRanges->size(), 1);
            const auto& range = selected.RangeInfo->NativeRanges->front();
            UNIT_ASSERT_VALUES_EQUAL(range.Lower.Values.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(range.Upper.Values.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*arena,
                range.Lower.Values.front().GetNativeRoot())), "2");
        }
    }
}
} // namespace NKikimr::NKqp
