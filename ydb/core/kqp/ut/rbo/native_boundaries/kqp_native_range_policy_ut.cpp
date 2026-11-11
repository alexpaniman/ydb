#include "../kqp_rbo_test_helpers.h"
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <yql/essentials/core/extract_predicate/extract_predicate.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_compatibility.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_source_builder.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_lookup_join_builder.h>
#include <yql/essentials/minikql/comp_nodes/ut/mkql_computation_node_ut.h>
#include <yql/essentials/providers/common/mkql/yql_provider_mkql.h>

#include <cmath>
#include <limits>

namespace NKikimr::NKqp {

namespace {
    void CheckScalarKeyEligibility(bool computedBound) {
        using namespace NFrontend;
        using namespace NYql;
        TString mismatches;
        for (const auto name : {"Bool", "Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32",
                "Int64", "Uint64", "Float", "Double", "String", "Utf8", "Uuid", "Decimal(12,2)",
                "Date", "Datetime", "Timestamp", "Interval", "Date32", "Datetime64", "Timestamp64", "Interval64"})
        for (const bool nullable : {false, true})
        for (const bool nullableBound : {false, true})
        for (const auto& [op, callable] : {std::pair{EOpcode::Equal, "=="}, std::pair{EOpcode::Less, "<"},
                std::pair{EOpcode::LessEqual, "<="}, std::pair{EOpcode::Greater, ">"}, std::pair{EOpcode::GreaterEqual, ">="}}) {
            NTests::TIdTestContext f;
            const auto scalarType = ParseType(name);
            const auto keyType = nullable ? scalarType.Optional() : scalarType;
            const auto parameterType = nullableBound ? scalarType.Optional() : scalarType;
            const auto key = f.Id("x");
            const auto arena = f.Props.NativeExpressions;
            auto bound = arena->Parameter("bound", parameterType);
            if (computedBound) bound = arena->If(arena->Parameter("choose", {ETypeKind::Bool}),
                bound, arena->Parameter("other", parameterType));
            const auto predicate = arena->Binary(op, arena->Column(key, keyType), bound);

            // Independently construct the old range-extractor boundary. The
            // legacy optimizer is used only as a test oracle, never to form
            // the native predicate or its range representation.
            auto& ctx = f.ExprCtx;
            const auto* fieldType = ExportNativeType(keyType, ctx);
            const auto* boundType = ExportNativeType(parameterType, ctx);
            const auto* rowType = ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{
                ctx.MakeType<TItemExprType>("x", fieldType)});
            const auto row = ctx.NewArgument(f.Pos, "row"); row->SetTypeAnn(rowType);
            row->AddConstraint(ctx.MakeConstraint<TEmptyConstraintNode>());
            const auto member = ctx.NewCallable(f.Pos, "Member", {row, ctx.NewAtom(f.Pos, "x")});
            auto parameter = ctx.NewCallable(f.Pos, "Parameter", {
                ctx.NewAtom(f.Pos, "bound"), ExpandType(f.Pos, *boundType, ctx)});
            if (computedBound) {
                const auto condition = ctx.NewCallable(f.Pos, "Parameter", {ctx.NewAtom(f.Pos, "choose"),
                    ExpandType(f.Pos, *ctx.MakeType<TDataExprType>(EDataSlot::Bool), ctx)});
                const auto other = ctx.NewCallable(f.Pos, "Parameter", {ctx.NewAtom(f.Pos, "other"), ExpandType(f.Pos, *boundType, ctx)});
                parameter = ctx.NewCallable(f.Pos, "If", {condition, parameter, other});
            }
            auto legacy = ctx.NewCallable(f.Pos, callable, {member, parameter});
            if (nullable || nullableBound) {
                const auto otherwise = ctx.NewCallable(f.Pos, "Bool", {ctx.NewAtom(f.Pos, "false")});
                legacy = ctx.NewCallable(f.Pos, "Coalesce", {legacy, otherwise});
            }
            const auto lambda = ctx.NewLambda(f.Pos, ctx.NewArguments(f.Pos, {row}), std::move(legacy));
            TPredicateExtractorSettings settings;
            settings.HaveNextValueCallable = true;
            settings.BuildLiteralRange = true;
            auto legacyTypes = MakeIntrusive<TTypeAnnotationContext>();
            auto extractor = MakePredicateRangeExtractor(settings);
            THashSet<TString> possibleKeys;
            UNIT_ASSERT(extractor->Prepare(lambda, *rowType, possibleKeys, ctx, *legacyTypes));
            const auto expected = extractor->BuildComputeNode({"x"}, ctx, *legacyTypes);

            auto table = MakeIntrusive<TKikimrTableMetadata>();
            table->Name = "/Root/table"; table->DoesExist = true;
            table->Kind = EKikimrTableKind::Datashard; table->KeyColumnNames = {"x"};
            table->Columns["x"].Type = name; table->Columns["x"].NotNull = !nullable;
            f.Tables->GetOrAddTable("ut", "/Root", table->Name).Metadata = table;
            auto read = f.Read({key});
            read->TableRef.NativeCatalog.emplace(TCatalogTable{std::string(table->Name), {{"x", keyType}}});
            f.SetType(*read); read->NativeSchema->Row[key] = keyType;
            auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, TExpression::FromNative(arena, predicate, &f.Props));
            const auto nodes = ctx.NodeAllocationCounter, types = ctx.TypeNodes.size();
            const auto result = TPushRangesRule(NYql::EStorageType::RowStorage).SimpleMatchAndApply(filter, f.RboCtx, f.Props);
            UNIT_ASSERT_VALUES_EQUAL(ctx.NodeAllocationCounter, nodes);
            UNIT_ASSERT_VALUES_EQUAL(ctx.TypeNodes.size(), types);
            const auto source = result->Kind == EOperator::Filter ? result->GetChildren().front() : result;
            const auto& actual = CastOperator<TOpRead>(*source).RangeInfo;
            const auto used = actual ? actual->UsedPrefixLen : 0;
            const auto points = actual ? actual->PointPrefixLen : 0;
            if (used != expected.UsedPrefixLen || points != expected.PointPrefixLen)
                mismatches += TStringBuilder() << keyType.ToString() << " " << callable << " " << parameterType.ToString() << ": used=" << used
                    << '/' << expected.UsedPrefixLen << " points=" << points << '/' << expected.PointPrefixLen << '\n';
        }
        UNIT_ASSERT_C(mismatches.empty(), mismatches);
    }

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeRangePolicy) {
    Y_UNIT_TEST(ScalarKeyEligibilityMatchesLegacyRangeExtraction) {
        CheckScalarKeyEligibility(false);
    }

    Y_UNIT_TEST(ClosedScalarBoundsMatchLegacyRangeExtraction) {
        CheckScalarKeyEligibility(true);
    }

    Y_UNIT_TEST(ComputedBoundsRequireRepeatableRowIndependentValues) {
        using namespace NFrontend;
        using O = EOpcode;
        NTests::TIdTestContext f;
        const auto key = f.Id("x");
        const auto arena = f.Props.NativeExpressions;
        auto table = MakeIntrusive<TKikimrTableMetadata>();
        table->Name = "/Root/table"; table->DoesExist = true;
        table->Kind = EKikimrTableKind::Datashard; table->KeyColumnNames = {"x"};
        f.Tables->GetOrAddTable("ut", "/Root", table->Name).Metadata = table;
        const auto value = arena->Column(key, {ETypeKind::Uint64});
        const auto one = arena->Literal({ETypeKind::Uint64}, std::uint64_t{1});
        const auto constant = arena->Binary(O::Add, arena->Parameter("bound", {ETypeKind::Uint64}), one);
        for (const auto bound : {constant, arena->Binary(O::Add, value, one), arena->Call("RandomNumber", {})}) {
            auto read = f.Read({key});
            auto filter = MakeIntrusive<TOpFilter>(read, f.Pos,
                TExpression::FromNative(arena, arena->Binary(O::Equal, value, bound), &f.Props));
            const auto nodes = f.ExprCtx.NodeAllocationCounter, types = f.ExprCtx.TypeNodes.size();
            const auto result = TPushRangesRule(NYql::EStorageType::RowStorage).SimpleMatchAndApply(filter, f.RboCtx, f.Props);
            UNIT_ASSERT_VALUES_EQUAL(result != filter, bound == constant);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
        }
    }

    Y_UNIT_TEST(UnorderedBoundsAndPointsExecuteWithoutSemanticLowering) {
        using namespace NFrontend;
        using namespace NYql;
        using O = EOpcode;
        for (const auto name : {"Float", "Double", "Decimal(12,2)"})
        for (const bool nullable : {false, true})
        for (const bool nullableBound : {false, true})
        for (const auto op : {O::Equal, O::Less, O::LessEqual, O::Greater, O::GreaterEqual})
        for (const unsigned boundKind : {0, 1, 2})
        for (const bool multiple : {false, true})
        for (const unsigned form : {0, 1, 2})
        for (const auto storage : {NYql::EStorageType::RowStorage, NYql::EStorageType::ColumnStorage}) {
            if ((multiple && op != O::Equal) || (boundKind == 2 && !nullableBound)) continue;
            const bool nan = boundKind == 1, absent = boundKind == 2, invalid = nan || absent;
            NTests::TIdTestContext f;
            const auto scalar = ParseType(name), keyType = nullable ? scalar.Optional() : scalar;
            const auto key = f.Id("x");
            const auto arena = f.Props.NativeExpressions;
            const auto parameterType = nullableBound ? scalar.Optional() : scalar;
            auto parameter = arena->Parameter("bound", parameterType);
            if (form) parameter = arena->If(arena->Parameter("choose", {ETypeKind::Bool}),
                parameter, arena->Parameter("other", parameterType));
            auto predicate = arena->Binary(op, arena->Column(key, keyType), parameter);
            if (multiple) {
                const TScalar other = scalar.Kind == ETypeKind::Decimal ? TScalar{TDecimal{700}} : TScalar{7.0};
                predicate = arena->Binary(O::Or, predicate,
                    arena->Binary(O::Equal, arena->Column(key, keyType), arena->Literal(scalar, other)));
            }
            auto table = MakeIntrusive<TKikimrTableMetadata>();
            table->Name = "/Root/table"; table->DoesExist = true;
            table->Kind = storage == NYql::EStorageType::RowStorage ? EKikimrTableKind::Datashard : EKikimrTableKind::Olap;
            table->KeyColumnNames = {"x", "suffix"};
            table->Columns["x"].Type = name; table->Columns["x"].NotNull = !nullable;
            table->Columns["suffix"].Type = "Int32"; table->Columns["suffix"].NotNull = true;
            f.Tables->GetOrAddTable("ut", "/Root", table->Name).Metadata = table;
            auto read = f.Read({key}); read->StorageType = storage;
            read->TableRef.NativeCatalog.emplace(TCatalogTable{std::string(table->Name),
                {{"x", keyType}, {"suffix", {ETypeKind::Int32}}}});
            f.SetType(*read); read->NativeSchema->Row[key] = keyType;
            auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, TExpression::FromNative(arena, predicate, &f.Props));
            const auto nodes = f.ExprCtx.NodeAllocationCounter, types = f.ExprCtx.TypeNodes.size();
            const auto result = TPushRangesRule(storage).SimpleMatchAndApply(filter, f.RboCtx, f.Props);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
            UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
            UNIT_ASSERT(result->Kind == EOperator::Source);
            auto& source = CastOperator<TOpRead>(*result);
            UNIT_ASSERT(source.RangeInfo && source.RangeInfo->NativeRanges);
            UNIT_ASSERT(source.RangeInfo->NativeRanges->front().Condition);
            UNIT_ASSERT(source.ToJson(0, f.Props.InfoUnitRegistry)["ReadRanges"][0].Has("Condition"));
            TSubstitutions renames;
            auto copied = source.Copy(f.Props, renames);
            auto& copy = CastOperator<TOpRead>(*copied);
            size_t visitedConditions = 0;
            copied->RewriteExpressions([&](const TExpression& expression) {
                if (expression.GetNativeType() == TType{ETypeKind::Bool} && expression.GetRawInputIUs().Empty())
                    ++visitedConditions;
                return expression;
            });
            UNIT_ASSERT(visitedConditions >= 1);
            const TScalar bound = absent ? TScalar{} : scalar.Kind == ETypeKind::Decimal
                ? TScalar{TDecimal{nan ? TDecimal::NaN() : 300}}
                : TScalar{nan ? std::numeric_limits<double>::quiet_NaN() : 3.0};
            auto& ctx = f.ExprCtx;
            const auto* schema = ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{
                ctx.MakeType<TItemExprType>("x", ExportNativeType(keyType, ctx)),
                ctx.MakeType<TItemExprType>("suffix", ExportNativeType({ETypeKind::Int32}, ctx))});
            const TPhysicalNames names(f.Props.InfoUnitRegistry);
            const auto physical = TPhysicalSourceBuilder(copy, ctx, f.Pos, names,
                f.Props.InfoUnitRegistry, "stage", false, {}, schema).BuildPhysicalOp();
            const auto ranges = FindNode(physical, [](const NYql::TExprNode::TPtr& node) { return node->IsCallable("RangeFinalize"); });
            UNIT_ASSERT(ranges);
            const auto execute = [&](const NYql::TExprNode::TPtr& expression, const auto& check) {
                UNIT_ASSERT(AreAllNodesTypeAnnotated(*expression));
                UNIT_ASSERT(!NeedsRboCompatibilityLowering(expression));
                NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
                NCommon::TMkqlCommonCallableCompiler compiler;
                NCommon::TMkqlBuildContext build(compiler, *setup.PgmBuilder, ctx);
                const auto literal = LowerNativeLambda(*arena, arena->Literal(parameterType, bound), ctx, f.Pos)->TailPtr();
                const TScalar decoy = scalar.Kind == ETypeKind::Decimal ? TScalar{TDecimal{1700}} : TScalar{17.0};
                const auto other = LowerNativeLambda(*arena, arena->Literal(parameterType, decoy), ctx, f.Pos)->TailPtr();
                const auto selected = NCommon::MkqlBuildExpr(*literal, build), unselected = NCommon::MkqlBuildExpr(*other, build);
                build.Parameters = setup.PgmBuilder->NewStruct({
                    {"$bound", form == 2 ? unselected : selected}, {"$other", form == 2 ? selected : unselected},
                    {"$choose", setup.PgmBuilder->NewDataLiteral<bool>(form != 2)}});
                auto graph = setup.BuildGraph(NCommon::MkqlBuildExpr(*expression, build));
                check(graph->GetValue());
            };
            execute(ranges, [&](const NUdf::TUnboxedValue& values) {
                UNIT_ASSERT_VALUES_EQUAL_C(values.GetListLength(), (invalid ? 0 : 1) + (multiple ? 1 : 0), name << " op=" << unsigned(op));
                if (invalid) return;
                auto iterator = values.GetListIterator();
                NUdf::TUnboxedValue interval;
                UNIT_ASSERT(iterator.Next(interval));
                const auto finite = interval.GetElement(op == O::Less || op == O::LessEqual ? 1 : 0).GetElement(0);
                const NUdf::TUnboxedValuePod finiteValue = nullable ? finite.GetOptionalValue() : static_cast<NUdf::TUnboxedValuePod>(finite);
                if (scalar.Kind == ETypeKind::Decimal) UNIT_ASSERT(finiteValue.GetInt128() == 300);
                else if (scalar.Kind == ETypeKind::Float) UNIT_ASSERT_VALUES_EQUAL(finiteValue.Get<float>(), 3.0f);
                else UNIT_ASSERT_VALUES_EQUAL(finiteValue.Get<double>(), 3.0);
                if (op == O::Greater || op == O::GreaterEqual) {
                    const auto upper = interval.GetElement(1);
                    const auto item = upper.GetElement(0);
                    const NUdf::TUnboxedValuePod value = nullable ? item.GetOptionalValue() : static_cast<NUdf::TUnboxedValuePod>(item);
                    if (scalar.Kind == ETypeKind::Decimal) UNIT_ASSERT(value.GetInt128() == TDecimal::NaN());
                    else if (scalar.Kind == ETypeKind::Float) UNIT_ASSERT(std::isnan(value.Get<float>()));
                    else UNIT_ASSERT(std::isnan(value.Get<double>()));
                    UNIT_ASSERT_VALUES_EQUAL(upper.GetElement(2).Get<i32>(), 0);
                }
                if (nullable && (op == O::Less || op == O::LessEqual)) {
                    const auto lower = interval.GetElement(0);
                    UNIT_ASSERT(lower.GetElement(0));
                    UNIT_ASSERT(!lower.GetElement(0).GetOptionalValue());
                    UNIT_ASSERT_VALUES_EQUAL(lower.GetElement(2).Get<i32>(), 0);
                }
            });
            if (op == O::Equal && storage == NYql::EStorageType::RowStorage) {
                UNIT_ASSERT_VALUES_EQUAL(copy.RangeInfo->PointPrefixes.size(), 1);
                auto prefix = copy.RangeInfo->PointPrefixes.front();
                UNIT_ASSERT(prefix.NativePoints->Rows.front().Condition);
                // A finite NULL point remains valid alongside guarded parameters.
                if (nullable) prefix.NativePoints->Rows.push_back({{TExpression::FromNative(arena,
                    arena->Literal(keyType, {}), &f.Props)}, {}});
                const auto points = NLookupJoinBuilder::BuildLookupPoints(prefix, ctx, f.Pos);
                execute(points, [&](const NUdf::TUnboxedValue& values) {
                    UNIT_ASSERT_VALUES_EQUAL(values.GetListLength(), (invalid ? 0 : 1) + (nullable ? 1 : 0) + (multiple ? 1 : 0));
                });
            }
        }
    }

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
