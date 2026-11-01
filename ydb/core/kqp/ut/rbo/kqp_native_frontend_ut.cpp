#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/cbo/solver/kqp_opt_predicate_selectivity.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_compatibility.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>
#include <yql/essentials/minikql/comp_nodes/ut/mkql_computation_node_ut.h>
#include <yql/essentials/minikql/mkql_type_ops.h>
#include <yql/essentials/providers/common/mkql/yql_provider_mkql.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/public/decimal/yql_decimal.h>

#include <cmath>
#include <bit>
#include <limits>
#include <type_traits>

namespace NKikimr::NKqp {
namespace {
TNativeTable NoTables(std::string_view, NFrontend::TSourcePosition pos) {
    throw NFrontend::TFrontendError(pos, "unexpected table lookup");
}

NFrontend::TExprId Projection(const NFrontend::TBoundProgram& program) {
    const auto column = program.Results.at(0).Columns.at(0).Column;
    for (const auto& relation : program.Relations) {
        for (const auto& definition : relation.Definitions) if (definition.Column == column) return definition.Expression;
    }
    ythrow yexception() << "projection not found";
}

// Compile and execute the terminal scalar graph directly: no YQL annotation,
// expression peepholes, SQL translation or compatibility transformer.
void CheckScalar(const NFrontend::TExpressionArena& arena, NFrontend::TExprId id, std::string_view sql) {
    const auto expected = NFrontend::Evaluate(arena, id);
    TExprContext ctx;
    const auto lambda = LowerNativeLambda(arena, id, ctx, {});
    UNIT_ASSERT_C(AreAllNodesTypeAnnotated(*lambda), sql);
    UNIT_ASSERT_C(!NeedsRboCompatibilityLowering(lambda), sql);
    NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
    auto registry = setup.FunctionRegistry->Clone();
    NMiniKQL::FillStaticModules(*registry);
    setup.FunctionRegistry = registry;
    setup.PgmBuilder.Reset(new NMiniKQL::TProgramBuilder(*setup.Env, *registry));
    NYql::NCommon::TMkqlCommonCallableCompiler compiler;
    NYql::NCommon::TMkqlBuildContext build(compiler, *setup.PgmBuilder, ctx);
    const auto runtime = NYql::NCommon::MkqlBuildLambda(*lambda, build, {setup.PgmBuilder->NewEmptyStruct()});
    auto graph = setup.BuildGraph(runtime);
    const auto value = graph->GetValue();
    const auto checkValue = [&](auto&& self, const NUdf::TUnboxedValuePod& value, const NFrontend::TValue& expected) -> void {
        if (expected.IsNull()) {
            UNIT_ASSERT_C(!value, sql);
            return;
        }
        UNIT_ASSERT_C(value, sql);
        using K = NFrontend::ETypeKind;
        switch (expected.Type.Kind) {
            case K::Bool: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<bool>(), std::get<bool>(expected.Data), sql); break;
            case K::Int8: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i8>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Int16: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i16>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Int32: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i32>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Int64: case K::Interval: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i64>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Uint8: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui8>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Uint16: case K::Date: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui16>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Uint32: case K::Datetime: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui32>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Uint64: case K::Timestamp: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui64>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Float: case K::Double: {
                const auto actual = expected.Type.Kind == K::Float ? double(value.Get<float>()) : value.Get<double>();
                const auto number = std::get<double>(expected.Data);
                UNIT_ASSERT_C((std::isnan(actual) && std::isnan(number)) || actual == number, sql);
                break;
            }
            case K::String: case K::Utf8: UNIT_ASSERT_VALUES_EQUAL_C(TString(value.AsStringRef()), TString(std::get<std::string>(expected.Data)), sql); break;
            case K::Decimal: UNIT_ASSERT_C(value.GetInt128() == std::get<NFrontend::TDecimal>(expected.Data).Coefficient, sql); break;
            case K::Null: UNIT_FAIL("unexpected non-null Null");
            case K::Tuple: {
                const auto& items = std::get<NFrontend::TTuple>(expected.Data)->Items;
                for (std::size_t i = 0; i < items.size(); ++i) self(self, value.GetElement(i), items[i]);
                break;
            }
            case K::Calendar: UNIT_FAIL("calendar values must be consumed by scalar date operations");
        }
    };
    checkValue(checkValue, value, expected);
}
void CheckScalar(std::string_view sql) {
    auto program = NFrontend::NDetail::Compile(sql, [](auto, auto pos) -> NFrontend::TCatalogTable {
        throw NFrontend::TFrontendError(pos, "unexpected table");
    });
    CheckScalar(program.Expressions, Projection(program), sql);
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeFrontend) {
    Y_UNIT_TEST(NativeSelectivityUsesKeysAndTypedStatisticsWithoutLowering) {
        using namespace NFrontend;
        using O = EOpcode;
        using K = ETypeKind;
        TExpressionArena arena;
        TOptimizerStatistics stats;
        stats.Nrows = 1000;
        stats.KeyColumns = MakeIntrusive<TOptimizerStatistics::TKeyColumns>(TVector<TString>{"10"});
        const auto key = arena.Column(10, {K::Int64});
        const auto value = arena.Column(20, {K::Int64, true});
        const auto one = arena.Literal({K::Int64}, std::int64_t{1});
        const auto parameter = arena.Parameter("p", {K::Int64});
        const auto estimate = [&](TExprId root, double expected) {
            const auto size = arena.Size();
            UNIT_ASSERT_DOUBLES_EQUAL(ComputeNativeSelectivity(arena, root, stats), expected, 1e-12);
            UNIT_ASSERT_VALUES_EQUAL(arena.Size(), size);
        };
        const auto keyEq = arena.Binary(O::Equal, key, one), valEq = arena.Binary(O::Equal, value, one);
        estimate(keyEq, 0.001);
        estimate(valEq, 0.1);
        estimate(arena.Binary(O::Equal, parameter, key), 0.001);
        estimate(arena.Binary(O::Less, key, parameter), 0.4);
        estimate(arena.Binary(O::Equal, key, value), 0.3);
        estimate(arena.Binary(O::And, keyEq, valEq), 0.0001);
        estimate(arena.Binary(O::NotEqual, key, one), 0.999);
        estimate(arena.Binary(O::Equal, arena.Cast(key, {K::Int32}), one), 0.5);
        estimate(arena.Unary(O::IsNull, key), 0);
        estimate(arena.Unary(O::IsNull, value), 0.1);
        estimate(arena.Unary(O::IsNotNull, value), 0.9);
        estimate(arena.Unary(O::IsNull, arena.Column(40, {K::Null})), 1);
        const auto nil = arena.Literal({K::Bool, true}, {});
        const auto yes = arena.Literal({K::Bool}, true), no = arena.Literal({K::Bool}, false);
        estimate(nil, 0);
        estimate(arena.Unary(O::Not, nil), 0);
        estimate(arena.Binary(O::And, nil, yes), 0);
        estimate(arena.Binary(O::Or, nil, yes), 1);
        estimate(arena.Unary(O::Not, arena.Binary(O::Or, nil, no)), 0);
        const TExprId coalesceTrue[]{nil, yes}, coalesceFalse[]{nil, no};
        estimate(arena.Coalesce(coalesceTrue), 1);
        estimate(arena.Coalesce(coalesceFalse), 0);
        estimate(arena.Binary(O::Equal, value, arena.Cast(arena.Null(), {K::Int64})), 0);
        const auto text = arena.Column(30, {K::String});
        const auto needle = arena.Literal({K::String}, std::string("abc"));
        estimate(arena.Binary(O::Contains, text, needle), 0.1);
        estimate(arena.Binary(O::StartsWith, text, needle), 0.5);

        // The common defaults match the old estimator; it is used only as a
        // test oracle after direct lowering, never by the native visitor.
        TExprContext context;
        for (const auto root : {keyEq, valEq, arena.Binary(O::Less, key, one), arena.Binary(O::Equal, key, value)}) {
            const auto lambda = LowerNativeLambda(arena, root, context, {});
            const auto legacy = TPredicateSelectivityComputer(std::make_shared<TOptimizerStatistics>(stats))
                .Compute(NNodes::TCoLambda(lambda).Body());
            UNIT_ASSERT_DOUBLES_EQUAL(ComputeNativeSelectivity(arena, root, stats), legacy, 1e-12);
        }
        // A deep DAG with repeated children must not become an exponential tree
        // or consume the native call stack. Unrelated arena roots are untouched.
        auto deep = valEq;
        for (size_t i = 0; i < 20000; ++i) deep = arena.Binary(O::And, deep, deep);
        estimate(deep, 0.1);
    }

    Y_UNIT_TEST(NativeSelectivityProbesExactStorageWidths) {
        using namespace NFrontend;
        using K = ETypeKind;
        const auto check = [&]<typename T>(TType type, TScalar value, T storageValue) {
            TExpressionArena arena;
            TOptimizerStatistics stats;
            stats.Nrows = 10;
            stats.ColumnStatistics = MakeIntrusive<TOptimizerStatistics::TColumnStatMap>();
            auto& columnStats = stats.ColumnStatistics->Data["7"];
            columnStats.Type = type.ToString();
            columnStats.CountMinSketch.reset(TCountMinSketch::Create());
            for (size_t i = 0; i < 3; ++i) {
                if constexpr (std::is_same_v<T, std::string_view>)
                    columnStats.CountMinSketch->Count(storageValue.data(), storageValue.size());
                else
                    columnStats.CountMinSketch->Count(reinterpret_cast<const char*>(&storageValue), sizeof(storageValue));
            }
            const auto column = arena.Column(7, type.Optional()), literal = arena.Literal(type, value);
            const auto equal = arena.Binary(EOpcode::Equal, column, literal);
            UNIT_ASSERT_DOUBLES_EQUAL_C(ComputeNativeSelectivity(arena, equal, stats), 0.3, 1e-12, type.ToString());
            const auto notEqual = arena.Binary(EOpcode::NotEqual, literal, column);
            UNIT_ASSERT_DOUBLES_EQUAL_C(ComputeNativeSelectivity(arena, notEqual, stats), 0.7, 1e-12, type.ToString());
            // Stale or different source types must not reinterpret storage bytes.
            columnStats.Type = "unsupported";
            UNIT_ASSERT_DOUBLES_EQUAL(ComputeNativeSelectivity(arena, equal, stats), 0.1, 1e-12);
        };
        check(TType{K::Bool}, true, ui8{1});
        check(TType{K::Int8}, std::int64_t{-7}, i8{-7});
        check(TType{K::Int16}, std::int64_t{-300}, i16{-300});
        check(TType{K::Int32}, std::int64_t{-70000}, i32{-70000});
        check(TType{K::Int64}, std::int64_t{9007199254740993LL}, i64{9007199254740993LL});
        check(TType{K::Uint8}, std::uint64_t{7}, ui8{7});
        check(TType{K::Uint16}, std::uint64_t{300}, ui16{300});
        check(TType{K::Uint32}, std::uint64_t{70000}, ui32{70000});
        check(TType{K::Uint64}, std::uint64_t{18446744073709551615ULL}, ui64{18446744073709551615ULL});
        check(TType{K::Float}, 1.5, 1.5f);
        check(TType{K::Double}, 1.5, 1.5);
        check(TType{K::Date}, std::uint64_t{100}, ui16{100});
        check(TType{K::Datetime}, std::uint64_t{70000}, ui32{70000});
        check(TType{K::Timestamp}, std::uint64_t{1234567890123456ULL}, ui64{1234567890123456ULL});
        check(TType{K::Interval}, std::int64_t{-1234567890123456LL}, i64{-1234567890123456LL});
        check(TType{K::String}, std::string("a\0b", 3), std::string_view("a\0b", 3));
        check(TType{K::Utf8}, std::string("héllo"), std::string_view("héllo"));
    }

    Y_UNIT_TEST(NativeSelectivityMergesTypedHistogramRanges) {
        using namespace NFrontend;
        using O = EOpcode;
        TExpressionArena arena;
        TOptimizerStatistics stats;
        stats.Nrows = 100;
        stats.ColumnStatistics = MakeIntrusive<TOptimizerStatistics::TColumnStatMap>();
        auto& source = stats.ColumnStatistics->Data["7"];
        source.Type = "Int64";
        auto histogram = std::make_shared<TEqWidthHistogram>(100, EHistogramValueType::Int64);
        histogram->InitializeBuckets(i64{0}, i64{100});
        for (i64 i = 0; i < 100; ++i) histogram->AddElement(i);
        source.EqWidthHistogramEstimator = std::make_shared<TEqWidthHistogramEstimator>(histogram);
        const auto column = arena.Column(7, {ETypeKind::Int64});
        const auto literal = [&](i64 value) { return arena.Literal({ETypeKind::Int64}, std::int64_t{value}); };
        const auto cmp = [&](O op, i64 value) { return arena.Binary(op, column, literal(value)); };
        const auto range = [&](i64 low, i64 high) { return arena.Binary(O::And, cmp(O::GreaterEqual, low), cmp(O::Less, high)); };
        const auto check = [&](TExprId root, double expected) {
            UNIT_ASSERT_DOUBLES_EQUAL(ComputeNativeSelectivity(arena, root, stats), expected, 1e-12);
        };
        check(cmp(O::Less, 30), 0.3);
        check(cmp(O::LessEqual, 30), 0.31);
        check(cmp(O::GreaterEqual, 70), 0.3);
        check(arena.Binary(O::Greater, literal(30), column), 0.3);
        check(arena.Unary(O::Not, cmp(O::GreaterEqual, 70)), 0.7);
        check(range(20, 40), 0.2);
        check(arena.Binary(O::And, cmp(O::Greater, 20), cmp(O::Less, 40)), 0.19);
        check(range(40, 20), 0);
        check(range(20, 20), 0);
        check(arena.Binary(O::And, range(20, 40), range(30, 50)), 0.1);
        check(arena.Binary(O::Or, range(20, 40), range(30, 50)), 0.3);
        check(arena.Binary(O::Or, range(10, 20), range(30, 40)), 0.2);
        check(arena.Binary(O::Or, range(40, 20), range(70, 80)), 0.1);
        check(arena.Binary(O::Or, range(10, 20), range(20, 30)), 0.2);
        check(arena.Binary(O::Or, cmp(O::Less, 20), cmp(O::Greater, 20)), 1.0);
        // No lossy signed/unsigned narrowing to obtain a sketch/histogram key.
        check(arena.Binary(O::Equal, column, arena.Literal({ETypeKind::Uint64}, std::uint64_t{18446744073709551615ULL})), 0.1);
        source.Type = "Int32";
        check(cmp(O::Less, 30), 0.4);
    }

    Y_UNIT_TEST(NativeStatisticsRequestsMatchConsumedPredicates) {
        using namespace NFrontend;
        using O = EOpcode;
        using K = ETypeKind;
        TExpressionArena arena;
        const auto a = arena.Column(1, {K::Int64, true}), b = arena.Column(2, {K::Int64, true});
        const auto one = arena.Literal({K::Int64}, std::int64_t{1});
        const auto keyEquality = arena.Binary(O::Equal, a, b);
        const auto equality = arena.Binary(O::Equal, a, one), inequality = arena.Binary(O::Less, b, one);
        auto usage = CollectNativePredicateStatistics(arena, arena.Binary(O::And, equality, inequality));
        UNIT_ASSERT(usage.CountMinColumns == TUnorderedIUs{1});
        UNIT_ASSERT(usage.HistogramColumns == TUnorderedIUs{2});
        UNIT_ASSERT(usage.JoinKeys.empty());
        usage = CollectNativePredicateStatistics(arena, arena.Binary(O::NotEqual, a, one));
        UNIT_ASSERT(usage.CountMinColumns == TUnorderedIUs{1});
        UNIT_ASSERT(usage.HistogramColumns.Empty());

        // Intersecting two inequalities into a point needs the equality sketch
        // as well as the individual range estimates, using the same visitor.
        const auto point = arena.Binary(O::And, arena.Binary(O::GreaterEqual, a, one), arena.Binary(O::LessEqual, a, one));
        usage = CollectNativePredicateStatistics(arena, point);
        UNIT_ASSERT(usage.CountMinColumns == TUnorderedIUs{1});
        UNIT_ASSERT(usage.HistogramColumns == TUnorderedIUs{1});
        for (const auto predicate : {
            arena.Binary(O::Equal, a, arena.Parameter("p", {K::Int64})),
            arena.Binary(O::Equal, arena.Cast(a, {K::Int32}), one),
            arena.Binary(O::Less, arena.Column(3, {K::String}), arena.Literal({K::String}, std::string("x")))}) {
            usage = CollectNativePredicateStatistics(arena, predicate);
            UNIT_ASSERT(usage.CountMinColumns.Empty());
            UNIT_ASSERT(usage.HistogramColumns.Empty());
            UNIT_ASSERT(usage.JoinKeys.empty());
        }

        const TExprId nullAsFalse[]{keyEquality, arena.Literal({K::Bool}, false)};
        for (const auto predicate : {keyEquality, arena.Binary(O::And, equality, keyEquality), arena.Coalesce(nullAsFalse)}) {
            usage = CollectNativePredicateStatistics(arena, predicate);
            UNIT_ASSERT_VALUES_EQUAL(usage.JoinKeys.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(usage.JoinKeys.front().first, 1);
            UNIT_ASSERT_VALUES_EQUAL(usage.JoinKeys.front().second, 2);
        }
        const TExprId nullAsTrue[]{keyEquality, arena.Literal({K::Bool}, true)};
        for (const auto predicate : {arena.Binary(O::Or, equality, keyEquality), arena.Unary(O::Not, keyEquality), arena.Coalesce(nullAsTrue)})
            UNIT_ASSERT(CollectNativePredicateStatistics(arena, predicate).JoinKeys.empty());
    }

    Y_UNIT_TEST(NativeFilterAndPushedReadUseTheSameStatistics) {
        using namespace NFrontend;
        NTests::TIdTestContext f;
        const auto id = f.Id("key");
        auto arena = std::make_shared<TExpressionArena>();
        const auto root = arena->Binary(EOpcode::Equal, arena->Column(id, {ETypeKind::Int64}),
            arena->Literal({ETypeKind::Int64}, std::int64_t{42}));
        const auto expression = TExpression::FromNative(arena, root, &f.ExprCtx, &f.Props);
        auto read = f.Read({id});
        read->Props.Metadata.emplace();
        read->Props.Metadata->ColumnsCount = 1;
        read->Props.Metadata->SourceStatsColumns.Add(id);
        read->Props.Metadata->KeyColumns.Append(id);
        const auto relation = f.Props.ColumnLineage.AddRelation("t", "/Root/table");
        f.Props.ColumnLineage.Add(id, {.SourceAlias = "t", .TableName = "/Root/table", .ColumnName = "key", .Relation = relation});
        auto metadata = MakeIntrusive<TKikimrTableMetadata>();
        metadata->DoesExist = true;
        metadata->RecordsCount = 100;
        metadata->DataSize = 800;
        f.Tables->GetOrAddTable("ut", "/Root", "/Root/table").Metadata = metadata;
        using TStatsMap = NYql::TOptimizerStatistics::TColumnStatMap;
        auto& global = f.TypeCtx.ColumnStatisticsByTableName["/Root/table"];
        global = MakeIntrusive<TStatsMap>();
        auto& source = global->Data["key"];
        source.Type = "Int64";
        source.CountMinSketch.reset(TCountMinSketch::Create());
        const i64 value = 42;
        for (int i = 0; i < 3; ++i) source.CountMinSketch->Count(reinterpret_cast<const char*>(&value), sizeof(value));
        read->ComputeStatistics(f.RboCtx, f.Props);
        auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, expression);
        filter->ComputeMetadata(f.RboCtx, f.Props);
        filter->ComputeStatistics(f.RboCtx, f.Props);
        UNIT_ASSERT_DOUBLES_EQUAL(filter->Props.Statistics->ERows, 3, 1e-12);
        UNIT_ASSERT_DOUBLES_EQUAL(filter->Props.Statistics->EBytes, 24, 1e-12);
        read->OriginalPredicate = expression;
        read->ComputeStatistics(f.RboCtx, f.Props);
        UNIT_ASSERT_DOUBLES_EQUAL(read->Props.Statistics->ERows, filter->Props.Statistics->ERows, 1e-12);
        UNIT_ASSERT_DOUBLES_EQUAL(read->Props.Statistics->Selectivity, filter->Props.Statistics->Selectivity, 1e-12);
        UNIT_ASSERT(!filter->GetFilterExpression().Node);
        UNIT_ASSERT(!read->OriginalPredicate->Node);
    }

    Y_UNIT_TEST(TupleStatesLowerDirectlyAndRoundTripThroughBackendTypes) {
        using namespace NFrontend;
        TExpressionArena arena, imported;
        const TType fields[]{{ETypeKind::Decimal, false, 35, 2}, {ETypeKind::Uint64}, {ETypeKind::Float, true}};
        const auto state = arena.TupleType(fields);
        const TTuple value = std::make_shared<const TTupleValue>(TTupleValue{{
            {fields[0], TDecimal{12345}}, {fields[1], std::uint64_t{3}}, {fields[2], 1.1}}});
        const auto literal = arena.Literal(state, value);
        CheckScalar(arena, literal, "native tuple state literal");
        CheckScalar(arena, arena.Convert(literal, state.Optional()), "optional tuple state");
        CheckScalar(arena, arena.Literal(state.Optional(), std::monostate{}), "absent tuple state");
        TExprContext ctx;
        for (const auto type : {state, state.Optional()}) {
            const auto* backend = ExportNativeType(type, ctx, &arena);
            const auto native = ImportNativeType(*backend, &imported);
            UNIT_ASSERT(native == imported.ImportType(arena, type));
            UNIT_ASSERT(IsSameAnnotation(*backend, *ExportNativeType(native, ctx, &imported)));
        }
        const auto* nestedTuple = ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{ExportNativeType(state.Optional(), ctx, &arena)});
        UNIT_ASSERT_EXCEPTION(ImportNativeType(*nestedTuple, &imported), TFrontendError);
        const auto* nestedOptional = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TOptionalExprType>(ctx.MakeType<TDataExprType>(EDataSlot::Uint64)));
        UNIT_ASSERT_EXCEPTION(ImportNativeType(*nestedOptional, &imported), TFrontendError);
    }

    Y_UNIT_TEST(UnicodeCaseLowersDirectlyToRuntime) {
        using namespace NFrontend;
        for (const auto& text : {std::string{}, std::string("AbC"), std::string("Привет École σςΣ ıIİi ſS ȿⱾ e\u0301"),
                std::string("\U00010428\U00010400😀"), std::string("a\0B", 3)}) {
            for (const bool nullable : {false, true}) {
                TExpressionArena arena;
                const TType type{ETypeKind::Utf8, nullable};
                const auto value = arena.Literal(type, text);
                for (const auto op : {EOpcode::UnicodeUpper, EOpcode::UnicodeLower}) {
                    CheckScalar(arena, arena.Unary(op, value), "Unicode case");
                    if (nullable) CheckScalar(arena, arena.Unary(op, arena.Literal(type, std::monostate{})), "Unicode NULL");
                }
            }
        }
        CheckScalar("SELECT Unicode::ToUpper('hello')");
        CheckScalar("SELECT Unicode::ToLower(NULL)");
    }

    Y_UNIT_TEST(GroupingSetsExpansionKeepsNativeExpressions) {
        for (const auto* clause : {"ROLLUP(k,v)", "CUBE(k,v)", "k, ROLLUP(v)", "GROUPING SETS ((k,v))", "GROUPING SETS ((k,v),())"}) {
            for (const auto* measures : {"SUM(k), SUM(v), AVG(k), COUNT(*)", "COUNT(DISTINCT k), SUM(v), SUM(DISTINCT k)", "1"}) {
                NTests::TIdTestContext f;
                const TString sql = TStringBuilder() << "SELECT k, v, GROUPING(k,v), " << measures
                    << " FROM (SELECT 1 AS k, CAST(2 AS Int32?) AS v) AS q GROUP BY " << clause;
                auto roots = CompileNativeToRbo(std::string_view(sql.data(), sql.size()), NoTables, f.ExprCtx);
                auto& root = *roots.front();
                for (const auto& item : root) UNIT_ASSERT(item.Current->Kind != EOperator::GroupingSets);
                auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
                    UNIT_FAIL("native grouping sets invoked YQL annotation");
                    return IGraphTransformer::TStatus::Error;
                });
                TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
                TGlobalPruningStage("native grouping sets pruning").RunStage(root, context);
                UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                const auto* expected = root.GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                TVector<std::unique_ptr<IRule>> rules;
                rules.emplace_back(std::make_unique<TExpandGroupingSetsRule>());
                rules.emplace_back(std::make_unique<TExpandDistinctAggregationRule>());
                TRuleBasedStage("native grouping sets expansion", std::move(rules)).RunStage(root, context);
                UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                TGlobalPruningStage("native grouping sets padding pruning").RunStage(root, context);
                TVector<std::unique_ptr<IRule>> copies;
                copies.emplace_back(std::make_unique<TExpandReplicateRule>());
                TRuleBasedStage("native grouping sets copies", std::move(copies)).RunStage(root, context);
                UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                const auto* actual = root.GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                for (const auto& [id, label] : root.GetColumns().Items()) {
                    const auto name = f.ExprCtx.GetIndexAsString(id);
                    UNIT_ASSERT_C(IsSameAnnotation(*expected->FindItemType(name), *actual->FindItemType(name)), sql);
                }
                NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
                for (const auto& item : root) {
                    UNIT_ASSERT(item.Current->Kind != EOperator::GroupingSets);
                    for (const auto& expression : item.Current->GetExpressions()) {
                        UNIT_ASSERT(expression.get().IsNative());
                        UNIT_ASSERT(!expression.get().Node);
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(SampleDeviationStatesStayTypedAcrossAggregationPhases) {
        for (const auto* kind : {"Int32", "Int64", "Uint64", "Float", "Double"}) {
            for (const bool nullable : {false, true}) {
                for (const auto* grouping : {"", " GROUP BY k", " GROUP BY ROLLUP(k)", " GROUP BY GROUPING SETS((k),(),())"}) {
                    NTests::TIdTestContext f;
                    const TString sql = TStringBuilder() << "SELECT " << (*grouping ? "k, " : "")
                        << "STDDEV_SAMPLE(v) AS s, AVG(v) AS a, AVG(Decimal('1.25',8,2)) AS d "
                        << "FROM (SELECT 1 AS k, CAST(" << kind << "('2') AS " << kind << (nullable ? "?" : "") << ") AS v) AS q" << grouping;
                    auto roots = CompileNativeToRbo(std::string_view(sql.data(), sql.size()), NoTables, f.ExprCtx);
                    auto& root = *roots.front();
                    auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
                        UNIT_FAIL("native aggregate phases invoked YQL annotation");
                        return IGraphTransformer::TStatus::Error;
                    });
                    TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
                    TGlobalPruningStage("native aggregate pruning").RunStage(root, context);
                    UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                    const auto* expected = root.GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                    TVector<std::pair<IOperator*, size_t>> edges;
                    for (const auto& item : root) if (item.Current->Kind == EOperator::Aggregate)
                        edges.emplace_back(item.Parent ? item.Parent : &root, item.Parent ? item.ChildIndex : 0);
                    UNIT_ASSERT(!edges.empty());
                    for (const auto& [parent, index] : edges) {
                        UNIT_ASSERT(TPropagateAggregateThroughStageRule().MatchAndApply(parent->MutableChild(index), context, root.PlanProps));
                    }
                    root.RecomputeOutputIUsSubtree();
                    root.ComputeParents();
                    UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                    // Forward actual partial states through native scalar maps,
                    // so their tuple type must survive ID substitution and binding.
                    size_t partials = 0;
                    for (const auto& [parent, index] : edges) {
                        auto final = CastOperator<TOpAggregate>(parent->MutableChild(index));
                        UNIT_ASSERT(final->GetAggregationPhase() == EOpPhase::Final);
                        auto partial = CastOperator<TOpAggregate>(final->GetInput().Get());
                        UNIT_ASSERT(partial->GetAggregationPhase() == EOpPhase::Intermediate);
                        TSubstitutions copies;
                        TMapIUs fields;
                        for (const auto& [output, traits] : partial->GetAggregationTraits().Items()) {
                            if (traits.AggFunction != "variance_1_1") continue;
                            ++partials;
                            const auto* type = partial->GetIUType(output, f.ExprCtx);
                            UNIT_ASSERT_VALUES_EQUAL(type->IsOptionalOrNull(), nullable);
                            const auto& required = RemoveOptionality(*type);
                            UNIT_ASSERT(required.GetKind() == ETypeAnnotationKind::Tuple);
                            const auto& items = required.Cast<TTupleExprType>()->GetItems();
                            UNIT_ASSERT_VALUES_EQUAL(items.size(), 3);
                            for (const auto* item : items) UNIT_ASSERT(item->GetKind() == ETypeAnnotationKind::Data &&
                                item->Cast<TDataExprType>()->GetSlot() == EDataSlot::Double);
                            const auto copy = root.PlanProps.InfoUnitRegistry.AddCopy(output);
                            fields.Add(copy, MakeColumnAccess(output, *partial, partial->Pos, &f.ExprCtx, &root.PlanProps));
                            copies.Add(output, copy);
                        }
                        auto map = MakeIntrusive<TOpMap>(final->GetInput(), final->Pos, std::move(fields));
                        final->SetChild(0, std::move(map));
                        final->RenameUsedIUs(copies);
                    }
                    UNIT_ASSERT_VALUES_EQUAL(partials, edges.size());
                    root.RecomputeOutputIUsSubtree();
                    root.ComputeParents();
                    UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                    TGlobalPruningStage("native state forwarding pruning").RunStage(root, context);
                    TVector<std::unique_ptr<IRule>> copies;
                    copies.emplace_back(std::make_unique<TExpandReplicateRule>());
                    TRuleBasedStage("native state forwarding copies", std::move(copies)).RunStage(root, context);
                    UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                    const auto* actual = root.GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                    for (const auto& [id, label] : root.GetColumns().Items()) {
                        const auto name = f.ExprCtx.GetIndexAsString(id);
                        UNIT_ASSERT_C(IsSameAnnotation(*expected->FindItemType(name), *actual->FindItemType(name)), sql);
                    }
                    NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
                    for (const auto& item : root) for (const auto& expression : item.Current->GetExpressions()) {
                        UNIT_ASSERT(expression.get().IsNative());
                        UNIT_ASSERT(!expression.get().Node);
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(MixedDistinctExpansionKeepsNativeExpressions) {
        for (const auto* projection : {
                "COUNT(DISTINCT v), COUNT(*), SUM(v), SUM(DISTINCT v), MIN(v), MAX(DISTINCT v)",
                "COUNT(DISTINCT v), SUM(d), SUM(DISTINCT d), MIN(d), MAX(DISTINCT d)",
                "COUNT(DISTINCT v), SOME(CAST(NULL AS Int32)), SOME(7)",
                "COUNT(DISTINCT v), AVG(v), AVG(DISTINCT v), AVG(k)",
                "COUNT(DISTINCT v), AVG(d), AVG(DISTINCT d)",
                "AVG(DISTINCT v), AVG(DISTINCT d), AVG(DISTINCT k)"}) {
            for (const bool grouped : {false, true}) {
                NTests::TIdTestContext f;
                const TString sql = TStringBuilder() << "SELECT " << projection
                    << " FROM (SELECT 1 AS k, CAST(2 AS Int32?) AS v, Decimal('1.25',8,2) AS d) AS q"
                    << (grouped ? " GROUP BY k" : "");
                auto roots = CompileNativeToRbo(std::string_view(sql.data(), sql.size()), NoTables, f.ExprCtx);
                auto& root = *roots.front();
                auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
                    UNIT_FAIL("native DISTINCT expansion invoked YQL annotation");
                    return IGraphTransformer::TStatus::Error;
                });
                TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
                TGlobalPruningStage("native distinct pruning").RunStage(root, context);
                UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                const auto* expected = root.GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                TVector<std::unique_ptr<IRule>> rules;
                rules.emplace_back(std::make_unique<TExpandDistinctAggregationRule>());
                TRuleBasedStage("native distinct expansion", std::move(rules)).RunStage(root, context);
                UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                TGlobalPruningStage("native distinct padding pruning").RunStage(root, context);
                TVector<std::unique_ptr<IRule>> copies;
                copies.emplace_back(std::make_unique<TExpandReplicateRule>());
                TRuleBasedStage("native distinct copies", std::move(copies)).RunStage(root, context);
                UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
                const auto* actual = root.GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                for (const auto& [id, label] : root.GetColumns().Items()) {
                    const auto name = f.ExprCtx.GetIndexAsString(id);
                    UNIT_ASSERT(IsSameAnnotation(*expected->FindItemType(name), *actual->FindItemType(name)));
                }
                NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
                for (const auto& item : root) {
                    if (item.Current->Kind == EOperator::Aggregate) {
                        const auto& aggregate = CastOperator<TOpAggregate>(*item.Current);
                        for (const auto& [output, traits] : aggregate.GetAggregationTraits().Items()) {
                            UNIT_ASSERT(!traits.Distinct);
                            if (traits.AggFunction != "avg") continue;
                            UNIT_ASSERT(traits.ResultType);
                            const auto* type = aggregate.GetIUType(output, f.ExprCtx);
                            const auto& required = RemoveOptionality(*type);
                            if (aggregate.GetAggregationPhase() == EOpPhase::Intermediate) {
                                UNIT_ASSERT(required.GetKind() == ETypeAnnotationKind::Tuple);
                                const auto& fields = required.Cast<TTupleExprType>()->GetItems();
                                UNIT_ASSERT_VALUES_EQUAL(fields.size(), 2);
                                UNIT_ASSERT(fields[1]->Cast<TDataExprType>()->GetSlot() == EDataSlot::Uint64);
                                const auto* input = aggregate.GetInput()->GetIUType(traits.Input, f.ExprCtx);
                                UNIT_ASSERT_VALUES_EQUAL(type->IsOptionalOrNull(), input->IsOptionalOrNull());
                                if (traits.ResultType->Kind == NFrontend::ETypeKind::Decimal) {
                                    const auto* decimal = fields[0]->Cast<TDataExprParamsType>();
                                    UNIT_ASSERT_VALUES_EQUAL(decimal->GetParamOne(), "35");
                                    UNIT_ASSERT_VALUES_EQUAL(decimal->GetParamTwo(), "2");
                                } else UNIT_ASSERT(fields[0]->Cast<TDataExprType>()->GetSlot() == EDataSlot::Double);
                            } else {
                                UNIT_ASSERT(required.GetKind() == ETypeAnnotationKind::Data);
                                UNIT_ASSERT(ImportNativeType(required) == traits.ResultType->Required());
                            }
                        }
                    }
                    for (const auto& expression : item.Current->GetExpressions()) {
                        UNIT_ASSERT(expression.get().IsNative());
                        UNIT_ASSERT(!expression.get().Node);
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(KeyRangesStayNativeThroughPushdownPruningAndCopy) {
        using namespace NFrontend;
        using K = ETypeKind;
        using O = EOpcode;
        NTests::TIdTestContext f;
        const auto key = f.Id("key"), suffix = f.Id("suffix"), payload = f.Id("payload"), unused = f.Id("unused");
        auto metadata = MakeIntrusive<TKikimrTableMetadata>();
        metadata->DoesExist = true;
        metadata->Name = "/Root/table";
        metadata->Kind = EKikimrTableKind::Datashard;
        metadata->KeyColumnNames = {"key", "suffix"};
        f.Tables->GetOrAddTable("ut", "/Root", metadata->Name).Metadata = metadata;
        auto arena = std::make_shared<TExpressionArena>();
        const auto a = arena->Column(key, {K::Int32}), b = arena->Column(suffix, {K::Int32, true});
        const auto equal = arena->Binary(O::Equal, a, arena->Literal({K::Uint64}, std::uint64_t{2}));
        const auto upper = arena->Binary(O::Greater, arena->Literal({K::Int64}, std::int64_t{5}), b);
        const auto residual = arena->Binary(O::Greater, arena->Column(payload, {K::Int32}), arena->Literal({K::Int32}, std::int64_t{0}));
        const auto rootId = arena->Binary(O::And, arena->Binary(O::And, equal, upper), residual);
        auto read = f.Read({key, suffix, payload, unused});
        f.SetType(*read, {suffix});
        const auto expression = TExpression::FromNative(arena, rootId, &f.ExprCtx, &f.Props);
        auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, expression);
        auto forbidden = CreateFunctorTransformer([](NYql::TExprNode::TPtr, NYql::TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native key ranges invoked YQL annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        TPushRangesRule rule(NYql::EStorageType::RowStorage);
        const auto pushed = rule.SimpleMatchAndApply(filter, context, f.Props);
        UNIT_ASSERT(pushed->Kind == EOperator::Filter);
        const auto& remaining = CastOperator<TOpFilter>(*pushed);
        UNIT_ASSERT_VALUES_EQUAL(remaining.GetFilterExpression().GetNativeRoot(), residual);
        auto& source = CastOperator<TOpRead>(*remaining.GetInput());
        UNIT_ASSERT(source.RangeInfo && source.RangeInfo->NativeRange);
        UNIT_ASSERT(!source.GetRanges() && !source.GetLiteralRange());
        UNIT_ASSERT_VALUES_EQUAL(source.RangeInfo->UsedPrefixLen, 2);
        UNIT_ASSERT_VALUES_EQUAL(source.RangeInfo->PointPrefixLen, 1);
        UNIT_ASSERT_VALUES_EQUAL(*source.RangeInfo->ExpectedMaxRanges, 1);
        const auto& bounds = *source.RangeInfo->NativeRange;
        UNIT_ASSERT_VALUES_EQUAL(bounds.Lower.Values.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(bounds.Upper.Values.size(), 2);
        UNIT_ASSERT(!bounds.Lower.Inclusive && !bounds.Upper.Inclusive);
        for (const auto* bound : {&bounds.Lower, &bounds.Upper}) {
            UNIT_ASSERT(bound->Values[0].GetNativeType() == TType{K::Int32});
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(*arena, bound->Values[0].GetNativeRoot()).Data), 2);
        }
        UNIT_ASSERT(Evaluate(*arena, bounds.Lower.Values[1].GetNativeRoot()).IsNull());
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(*arena, bounds.Upper.Values[1].GetNativeRoot()).Data), 5);
        UNIT_ASSERT_VALUES_EQUAL(source.OriginalPredicate->GetNativeRoot(), rootId);
        UNIT_ASSERT(rule.SimpleMatchAndApply(pushed, context, f.Props) == pushed);
        for (const auto& term : source.GetExpressions()) {
            UNIT_ASSERT(term.get().IsNative() && !term.get().Node);
            UNIT_ASSERT(term.get().GetRawInputIUs().Empty());
        }
        TSubstitutions renames;
        const auto copied = source.Copy(f.Props, renames);
        UNIT_ASSERT(copied);
        const auto& copy = CastOperator<TOpRead>(*copied);
        UNIT_ASSERT(copy.RangeInfo->NativeRange);
        UNIT_ASSERT_VALUES_EQUAL(copy.RangeInfo->NativeRange->Upper.Values[1].GetNativeRoot(), bounds.Upper.Values[1].GetNativeRoot());
        UNIT_ASSERT(copy.OriginalPredicate->GetRawInputIUs().Contains(*renames.Find(key)));
        UNIT_ASSERT(!copy.OriginalPredicate->GetRawInputIUs().Contains(key));
        auto root = f.Root(pushed, {{payload, "payload"}});
        TGlobalPruningStage("prune").RunStage(*root, context);
        UNIT_ASSERT(source.GetColumns() == (TUnorderedIUs{key, suffix, payload}));
        NTests::AssertIdInvariants(*root, root->PlanProps);
        const auto explain = source.ToJson(0, root->PlanProps.InfoUnitRegistry);
        UNIT_ASSERT_VALUES_EQUAL(explain["ReadRangesExpectedSize"].GetStringSafe(), "1");
        UNIT_ASSERT(!explain["ReadRangeLower"]["Inclusive"].GetBooleanSafe());
    }

    Y_UNIT_TEST(KeyRangesRequireExactBoundsAndPreserveCheckedExpressions) {
        using namespace NFrontend;
        using K = ETypeKind;
        using O = EOpcode;
        NTests::TIdTestContext f;
        const auto key = f.Id("key"), external = f.Id("external");
        auto metadata = MakeIntrusive<TKikimrTableMetadata>();
        metadata->DoesExist = true;
        metadata->Name = "/Root/table";
        metadata->Kind = EKikimrTableKind::Datashard;
        metadata->KeyColumnNames = {"key"};
        f.Tables->GetOrAddTable("ut", "/Root", metadata->Name).Metadata = metadata;
        auto arena = std::make_shared<TExpressionArena>();
        const auto value = arena->Column(key, {K::Uint64, true});
        auto forbidden = CreateFunctorTransformer([](NYql::TExprNode::TPtr, NYql::TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native key ranges invoked YQL annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        TPushRangesRule rule(NYql::EStorageType::RowStorage);
        const auto check = [&](TExprId predicate, bool pushed) {
            auto read = f.Read({key});
            auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, TExpression::FromNative(arena, predicate, &f.ExprCtx, &f.Props));
            const auto result = rule.SimpleMatchAndApply(filter, context, f.Props);
            UNIT_ASSERT_VALUES_EQUAL(result != filter, pushed);
            return result;
        };
        for (const ui64 number : {0ULL, 9007199254740993ULL, 9223372036854775808ULL, 18446744073709551615ULL}) {
            const auto literal = arena->Literal({K::Uint64}, std::uint64_t{number});
            const auto result = check(arena->Binary(O::Equal, value, literal), true);
            const auto& bound = CastOperator<TOpRead>(*result).RangeInfo->NativeRange->Lower.Values.front();
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(*arena, bound.GetNativeRoot()).Data), number);
        }
        const auto param = arena->Parameter("bound", {K::Uint64});
        check(arena->Binary(O::LessEqual, value, param), true);
        check(arena->Binary(O::Equal, value, arena->Parameter("nullable", {K::Uint64, true})), false);
        check(arena->Binary(O::Equal, value, arena->Parameter("signed", {K::Int64})), false);
        check(arena->Binary(O::Greater, value, arena->Literal({K::Int64}, std::int64_t{-1})), false);
        check(arena->Binary(O::Equal, value, arena->Null()), false);
        const auto equal = arena->Binary(O::Equal, value, param);
        check(arena->Binary(O::Or, equal, arena->Unary(O::IsNull, value)), false);
        check(arena->Binary(O::Equal, arena->Cast(value, {K::Int32}), arena->Literal({K::Int32}, std::int64_t{1})), false);
        const auto checked = arena->Binary(O::Equal, arena->Ensure(value, arena->Literal({K::Bool}, false), "checked"), param);
        check(arena->Binary(O::And, equal, checked), false);
        check(arena->Binary(O::And, checked, equal), false);
        check(arena->Binary(O::And, equal, arena->Binary(O::Equal, arena->Column(external, {K::Uint64}), param)), false);
        auto shared = equal;
        for (size_t i = 0; i < 20000; ++i) shared = arena->Binary(O::And, shared, shared);
        check(shared, true);
    }

    Y_UNIT_TEST(OlapPredicatesStayNativeThroughPushdownPruningAndCopy) {
        using namespace NFrontend;
        NTests::TIdTestContext f;
        const auto key = f.Id("key"), payload = f.Id("payload"), unused = f.Id("unused");
        auto read = f.Read({key, payload, unused});
        read->StorageType = NYql::EStorageType::ColumnStorage;
        f.SetType(*read, {key});
        auto arena = std::make_shared<TExpressionArena>();
        const auto value = arena->Column(key, {ETypeKind::Int32, true});
        const auto positive = arena->Binary(EOpcode::Greater, value, arena->Literal({ETypeKind::Int32}, std::int64_t{0}));
        auto expression = TExpression::FromNative(arena, positive, &f.ExprCtx, &f.Props);
        auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, expression);
        auto forbidden = CreateFunctorTransformer([](NYql::TExprNode::TPtr, NYql::TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native OLAP pushdown invoked YQL annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        TPushOlapFilterRule rule;
        const auto pushed = rule.SimpleMatchAndApply(filter, context, f.Props);
        UNIT_ASSERT(pushed->Kind == EOperator::Source);
        auto& source = CastOperator<TOpRead>(*pushed);
        UNIT_ASSERT(source.OlapProgram.IsNative());
        UNIT_ASSERT(!source.OlapProgram.Native().Node);
        UNIT_ASSERT(source.GetRequiredColumns(f.ExprCtx) == TUnorderedIUs{key});
        TSubstitutions renames;
        const auto copied = source.Copy(f.Props, renames);
        UNIT_ASSERT(copied);
        const auto& copy = CastOperator<TOpRead>(*copied);
        UNIT_ASSERT(copy.OlapProgram.IsNative());
        UNIT_ASSERT(copy.OlapProgram.Native().GetNativeArena() == arena);
        UNIT_ASSERT(copy.GetRequiredColumns(f.ExprCtx) == TUnorderedIUs{*renames.Find(key)});
        auto root = f.Root(pushed, {{payload, "payload"}});
        TGlobalPruningStage("prune").RunStage(*root, context);
        UNIT_ASSERT(source.GetColumns() == (TUnorderedIUs{key, payload}));
        UNIT_ASSERT(!source.OlapProgram.Native().Node);

        // Discarding NULL before a checked expression could suppress an error.
        const auto checked = arena->Ensure(value, arena->Literal({ETypeKind::Bool}, false), "checked");
        const auto failure = arena->Binary(EOpcode::Greater, checked, arena->Literal({ETypeKind::Int32}, std::int64_t{0}));
        const auto text = arena->Cast(value, {ETypeKind::String});
        const auto length = arena->Binary(EOpcode::Greater, arena->Unary(EOpcode::Length, text), arena->Literal({ETypeKind::Uint32}, std::uint64_t{1}));
        for (const bool canFail : {false, true}) for (const bool first : {false, true}) {
            const auto remaining = canFail ? failure : length;
            const auto predicate = arena->Binary(EOpcode::And, first ? positive : remaining, first ? remaining : positive);
            auto fresh = f.Read({key, payload}); fresh->StorageType = NYql::EStorageType::ColumnStorage;
            auto original = MakeIntrusive<TOpFilter>(fresh, f.Pos, TExpression::FromNative(arena, predicate, &f.ExprCtx, &f.Props));
            const auto result = rule.SimpleMatchAndApply(original, context, f.Props);
            UNIT_ASSERT(result->Kind == EOperator::Filter);
            const auto& input = CastOperator<TOpRead>(*CastOperator<TOpFilter>(*result).GetInput());
            UNIT_ASSERT_VALUES_EQUAL(bool(input.OlapProgram), first && !canFail);
        }
        const auto external = arena->Column(unused, {ETypeKind::Int32});
        auto fresh = f.Read({key, payload}); fresh->StorageType = NYql::EStorageType::ColumnStorage;
        const auto externalFilter = MakeIntrusive<TOpFilter>(fresh, f.Pos,
            TExpression::FromNative(arena, arena->Binary(EOpcode::Greater, value, external), &f.ExprCtx, &f.Props));
        UNIT_ASSERT(rule.SimpleMatchAndApply(externalFilter, context, f.Props) == externalFilter);
        const auto mixedExternal = MakeIntrusive<TOpFilter>(fresh, f.Pos,
            TExpression::FromNative(arena, arena->Binary(EOpcode::And, positive,
                externalFilter->GetFilterExpression().GetNativeRoot()), &f.ExprCtx, &f.Props));
        UNIT_ASSERT(rule.SimpleMatchAndApply(mixedExternal, context, f.Props) == mixedExternal);
        const auto parameter = arena->Parameter("flag", {ETypeKind::Bool, true});
        for (const bool first : {false, true}) {
            const auto logical = arena->Binary(EOpcode::Or, first ? parameter : positive, first ? positive : parameter);
            UNIT_ASSERT(!CanLowerNativeOlapPredicate(*arena, logical));
        }
        UNIT_ASSERT(!CanLowerNativeOlapPredicate(*arena, parameter));
    }

    Y_UNIT_TEST(IntegerOperationsLowerDirectlyForEveryWidth) {
        using namespace NFrontend;
        using K = ETypeKind;
        for (const auto a : {K::Int8, K::Uint8, K::Int16, K::Uint16, K::Int32, K::Uint32, K::Int64, K::Uint64})
            for (const auto b : {K::Int8, K::Uint8, K::Int16, K::Uint16, K::Int32, K::Uint32, K::Int64, K::Uint64})
                for (const int test : {0, 1, 2, 3, 4}) {
                    TExpressionArena arena;
                    auto input = [&](K kind, bool right) {
                        const unsigned width = kind == K::Int8 || kind == K::Uint8 ? 8 : kind == K::Int16 || kind == K::Uint16 ? 16 :
                            kind == K::Int32 || kind == K::Uint32 ? 32 : 64;
                        const TType type{kind, test == 2 || test == (right ? 4 : 3)};
                        if (test == (right ? 4 : 3)) return arena.Literal(type, std::monostate{});
                        TScalar value;
                        if (type.IsSigned()) {
                            const auto high = static_cast<std::int64_t>((std::uint64_t{1} << (width - 1)) - 1);
                            value = test == 0 ? std::int64_t{-1} : test == 1 ? high : test == 2 ? -high - 1 : std::int64_t{0};
                        } else {
                            const auto high = width == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << width) - 1;
                            value = test == 0 ? high : test == 1 ? high / 2 + 1 : std::uint64_t{0};
                        }
                        return arena.Literal(type, value);
                    };
                    const auto left = input(a, false), right = input(b, true);
                    for (const auto op : {EOpcode::Equal, EOpcode::NotEqual, EOpcode::Less, EOpcode::LessEqual, EOpcode::Greater, EOpcode::GreaterEqual,
                            EOpcode::Add, EOpcode::Subtract, EOpcode::Multiply, EOpcode::Divide, EOpcode::Modulo}) {
                        const auto root = arena.Binary(op, left, right);
                        CheckScalar(arena, root, "heterogeneous integer operation");
                    }
                    CheckScalar(arena, arena.If(arena.Literal({K::Bool}, false), left, right), "heterogeneous conditional");
                    const TExprId alternatives[]{left, right};
                    CheckScalar(arena, arena.Coalesce(alternatives), "heterogeneous coalesce");
                }
    }

    Y_UNIT_TEST(FloatingPromotionLowersWithoutAnnotation) {
        using namespace NFrontend;
        using K = ETypeKind;
        for (const bool nullable : {false, true}) {
            TExpressionArena arena;
            const auto integer = arena.Literal({K::Uint64, nullable}, std::uint64_t{9223372586610589697ULL});
            for (const auto kind : {K::Float, K::Double}) for (const double value : {0.0, -1.0,
                    std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
                const auto number = arena.Literal({kind}, value);
                CheckScalar(arena, arena.Binary(EOpcode::Add, integer, number), "integer/floating promotion");
                CheckScalar(arena, arena.Binary(EOpcode::Add, arena.Literal({K::Float, true}, value), arena.Literal({K::Double}, 1.0)),
                    "Float/Double promotion with optional lifting");
            }
        }
    }

    Y_UNIT_TEST(OptionalBooleanLiftsPreserveJoinKeys) {
        using namespace NFrontend;
        for (const auto& [condition, keys] : std::initializer_list<std::pair<const char*, std::size_t>>{
                {"a.id = b.id AND a.y = b.y", 2}, {"(a.id = b.id AND a.x = b.x) AND a.y = b.y", 3}}) {
            NTests::TIdTestContext f;
            const std::string sql = std::string("SELECT a.id, b.id AS right_id FROM ") +
                "(SELECT 1 AS id, 2 AS x, CAST(3 AS Int32?) AS y) AS a FULL JOIN " +
                "(SELECT 1 AS id, 2 AS x, CAST(3 AS Int32?) AS y) AS b ON " + condition;
            auto roots = CompileNativeToRbo(sql, NoTables, f.ExprCtx);
            auto& root = *roots.front();
            bool found = false;
            for (const auto& item : root) if (item.Current->Kind == EOperator::Join) {
                const auto& join = CastOperator<TOpJoin>(*item.Current);
                UNIT_ASSERT_VALUES_EQUAL(join.JoinKeys.Items().size(), keys);
                UNIT_ASSERT(join.JoinFilters.empty());
                found = true;
            }
            UNIT_ASSERT(found);
            auto arena = root.PlanProps.NativeExpressions;
            const auto equal = arena->Binary(EOpcode::Equal, arena->Column(0, {ETypeKind::Int32}), arena->Column(1, {ETypeKind::Int32}));
            const auto lifted = TExpression::FromNative(arena, arena->Convert(equal, {ETypeKind::Bool, true}), &f.ExprCtx, &root.PlanProps);
            UNIT_ASSERT(lifted.MaybeEquiJoinCondition());
            const TEquiJoinCondition equality(lifted);
            UNIT_ASSERT_VALUES_EQUAL(equality.GetLeftIU(), 0);
            UNIT_ASSERT_VALUES_EQUAL(equality.GetRightIU(), 1);
        }
    }

    Y_UNIT_TEST(CheckedUnwrapIsExecutableAndPreservesLazyFailures) {
        using namespace NFrontend;
        TExpressionArena arena;
        for (const auto value : {std::int64_t{-5}, std::int64_t{0}, std::int64_t{12}}) {
            const auto unwrapped = arena.Unary(EOpcode::Unwrap, arena.Literal({ETypeKind::Int64, true}, value));
            UNIT_ASSERT(arena[unwrapped].Type == (TType{ETypeKind::Int64}));
            CheckScalar(arena, unwrapped, "internal checked unwrap");
            const TExprId roots[]{unwrapped};
            const auto folded = arena.FoldConstants(roots).front();
            UNIT_ASSERT(arena[folded].Op == EOpcode::Literal);
            UNIT_ASSERT(arena[folded].Type == arena[unwrapped].Type);
        }
        const auto failed = arena.Unary(EOpcode::Unwrap, arena.Literal({ETypeKind::Int64, true}, std::monostate{}));
        UNIT_ASSERT_EXCEPTION(Evaluate(arena, failed), TFrontendError);
        const TExprId roots[]{failed};
        UNIT_ASSERT_VALUES_EQUAL(arena.FoldConstants(roots).front(), failed);
        const auto lazy = arena.If(arena.Literal({ETypeKind::Bool}, false), failed, arena.Literal({ETypeKind::Int64}, std::int64_t{7}));
        CheckScalar(arena, lazy, "unused failing unwrap");
        const auto failedMessage = arena.Unary(EOpcode::Unwrap, arena.Literal({ETypeKind::Utf8, true}, std::monostate{}));
        const TExprId args[]{arena.Literal({ETypeKind::Int64, true}, std::int64_t{8}), failedMessage};
        const auto withMessage = arena.Call("Unwrap", args);
        CheckScalar(arena, withMessage, "unused failing error message");
        const TExprId withMessageRoots[]{withMessage};
        CheckScalar(arena, arena.FoldConstants(withMessageRoots).front(), "folded unwrap with unused message");
    }

    Y_UNIT_TEST(WindowConversionKeepsTypedNativeExpressions) {
        for (const auto* sql : {
            "SELECT x, SUM(x) OVER() AS s, COUNT(*) OVER() AS n FROM (SELECT 1 AS x UNION ALL SELECT 2) AS t",
            "SELECT p, SUM(x) OVER(PARTITION BY p) AS s FROM (SELECT CAST(NULL AS Int32) AS p, 1 AS x UNION ALL SELECT CAST(NULL AS Int32), 2) AS t",
            "SELECT x, SUM(x) OVER(ORDER BY x ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING) AS s, Rank() OVER(ORDER BY x) AS r FROM (SELECT 1 AS x UNION ALL SELECT 2) AS t",
            "SELECT SUM(x) OVER() + 1 IN (SELECT CAST(4 AS Int64)) FROM (SELECT 1 AS x UNION ALL SELECT 2) AS t"}) {
            NTests::TIdTestContext f;
            f.Config->SetEnableWindowFunctionsV2(true);
            auto roots = CompileNativeToRbo(sql, NoTables, f.ExprCtx);
            auto& root = *roots.front();
            auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
                UNIT_FAIL("native window invoked YQL expression annotation");
                return IGraphTransformer::TStatus::Error;
            });
            TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
            UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
            TVector<std::unique_ptr<IRule>> windows;
            windows.emplace_back(std::make_unique<TExpandWholePartitionWindowRule>());
            TRuleBasedStage("native whole-partition windows", std::move(windows)).RunStage(root, context);
            UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
            TVector<std::unique_ptr<IRule>> copies;
            copies.emplace_back(std::make_unique<TExpandReplicateRule>());
            TRuleBasedStage("native window copies", std::move(copies)).RunStage(root, context);
            UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
            for (const auto& item : root) for (const auto& expression : item.Current->GetExpressions()) {
                UNIT_ASSERT(expression.get().IsNative());
                UNIT_ASSERT(!expression.get().Node);
            }
            NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
        }
    }

    Y_UNIT_TEST(CorrelatedRewritesKeepNativeExpressionsAndBindings) {
        for (const auto* sql : {
            "SELECT o.id FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o WHERE o.id IN (SELECT 1 UNION ALL SELECT CAST(NULL AS Int32))",
            "SELECT o.id FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o WHERE o.id NOT IN (SELECT 1 UNION ALL SELECT CAST(NULL AS Int32))",
            "SELECT o.id + 1 IN (SELECT CAST(2 AS Int64)) AS v FROM (SELECT 1 AS id) AS o",
            "SELECT (o.id IN (SELECT 1)) IN (SELECT true) AS v FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o",
            "SELECT NULL IN (SELECT NULL) AS v",
            "SELECT o.id IN (SELECT i.id FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS i WHERE i.id <= o.id) AS v FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o",
            "SELECT o.id FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o WHERE o.id NOT IN (SELECT i.id FROM (SELECT 1 AS id) AS i WHERE i.id < o.id)",
            "SELECT Decimal('1.25',8,2) IN (SELECT Decimal('1.250',9,3)) AS v",
            "PRAGMA DisableAnsiInForEmptyOrNullableItemsCollections; SELECT o.id IN (SELECT CAST(NULL AS Int32)) AS v FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o",
            "SELECT o.id FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o WHERE EXISTS(SELECT i.id FROM (SELECT 1 AS id) AS i WHERE i.id = o.id)",
            "SELECT o.id FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o WHERE NOT EXISTS(SELECT i.id FROM (SELECT 1 AS id) AS i WHERE i.id = o.id)",
            "SELECT o.id FROM (SELECT 1 AS id) AS o WHERE EXISTS(SELECT 1 WHERE false) OR EXISTS(SELECT 1 WHERE o.id = 1)",
            "SELECT EXISTS(SELECT i.id FROM (SELECT 1 AS id) AS i WHERE i.id = o.id) AS yes FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o",
            "SELECT EXISTS(SELECT 1 WHERE false) AS no, NOT EXISTS(SELECT 1) AS also_no",
            "SELECT EXISTS(SELECT 1 WHERE EXISTS(SELECT 1 WHERE o.id = 1)) AS yes FROM (SELECT 1 AS id) AS o",
            "SELECT (SELECT COUNT(*) FROM (SELECT 1 AS id UNION ALL SELECT 2 AS id) AS i WHERE i.id > o.id) AS n FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o",
            "SELECT (SELECT MAX(i.id) FROM (SELECT 1 AS id UNION ALL SELECT 2 AS id) AS i WHERE i.id = o.id) AS n FROM (SELECT 1 AS id UNION ALL SELECT CAST(NULL AS Int32) AS id) AS o",
            "SELECT (SELECT (SELECT o.id)) AS n FROM (SELECT 1 AS id) AS o",
            "SELECT (SELECT i.id FROM (SELECT 1 AS id UNION ALL SELECT 2 AS id) AS i WHERE i.id > o.id ORDER BY i.id DESC LIMIT 1) AS n FROM (SELECT 1 AS id) AS o"}) {
            NTests::TIdTestContext f;
            auto roots = CompileNativeToRbo(sql, NoTables, f.ExprCtx);
            auto& root = *roots.front();
            auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
                UNIT_FAIL("native correlation invoked YQL annotation");
                return IGraphTransformer::TStatus::Error;
            });
            TRBOContext context(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
            TGlobalPruningStage("native capture pruning").RunStage(root, context);
            UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
            TVector<std::unique_ptr<IRule>> inlineRules;
            inlineRules.emplace_back(std::make_unique<TInlineSimpleInExistsSubplanRule>());
            inlineRules.emplace_back(std::make_unique<TInlineGenericInExistsSubplanRule>());
            inlineRules.emplace_back(std::make_unique<TInlineScalarSubplanRule>());
            TRuleBasedStage("native scalar inlining", std::move(inlineRules)).RunStage(root, context);
            UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
            TConstantFoldingStage().RunStage(root, context);
            TVector<std::unique_ptr<IRule>> rules;
            rules.emplace_back(std::make_unique<TRewriteDependentJoinToCrossJoinNoFreeVarsRule>());
            rules.emplace_back(std::make_unique<TRewriteDependentJoinToCrossJoinRule>());
            rules.emplace_back(std::make_unique<TEliminateDependentJoinDomainRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughFilterRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughMapRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughAggregateRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughUnionAllRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughJoinRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughReplicateRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughSortRule>());
            rules.emplace_back(std::make_unique<TPushDependentJoinThroughLimitRule>());
            rules.emplace_back(std::make_unique<TDependentJoinNotSupportedRule>());
            TRuleBasedStage("native decorrelation", std::move(rules)).RunStage(root, context);
            TGlobalPruningStage("native correlation pruning").RunStage(root, context);
            TVector<std::unique_ptr<IRule>> copies;
            copies.emplace_back(std::make_unique<TExpandReplicateRule>());
            TRuleBasedStage("native replicate expansion", std::move(copies)).RunStage(root, context);
            UNIT_ASSERT(root.ComputeTypes(context) == IGraphTransformer::TStatus::Ok);
            UNIT_ASSERT(root.PlanProps.Subplans.Empty());
            NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
            for (const auto& item : root) {
                UNIT_ASSERT(item.Current->Kind != EOperator::DependentJoin);
                UNIT_ASSERT(item.Current->Kind != EOperator::AddDependencies);
                for (const auto& expression : item.Current->GetExpressions()) {
                    UNIT_ASSERT(expression.get().IsNative());
                    UNIT_ASSERT(!expression.get().Node);
                }
            }
        }
    }

    Y_UNIT_TEST(StableScalarKeysMatchRuntimeBytes) {
        using namespace NFrontend;
        using K = ETypeKind;
        auto check = [](TType type, const std::vector<TScalar>& values) {
            TExpressionArena arena;
            for (const auto& value : values) {
                for (const auto inputType : {type, type.Optional()}) {
                    const auto key = arena.Unary(EOpcode::StablePickle, arena.Literal(inputType, value));
                    CheckScalar(arena, key, type.ToString());
                    const TExprId roots[]{key};
                    const auto folded = arena.FoldConstants(roots).front();
                    UNIT_ASSERT(arena[folded].Op == EOpcode::Literal);
                    UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(arena[folded].Value),
                        std::get<std::string>(Evaluate(arena, key).Data));
                }
            }
            CheckScalar(arena, arena.Unary(EOpcode::StablePickle,
                arena.Literal(type.Optional(), std::monostate{})), type.ToString());
        };
        check({K::Bool}, {false, true});
        for (const auto [kind, width] : {std::pair{K::Int8, 8u}, {K::Int16, 16u}, {K::Int32, 32u}, {K::Int64, 64u}}) {
            const auto max = static_cast<std::int64_t>((std::uint64_t{1} << (width - 1)) - 1);
            std::vector<TScalar> values{std::int64_t{0}, std::int64_t{-1}, max, -max - 1};
            for (unsigned bit = 6; bit < width - 1; bit += 7) {
                const auto boundary = std::int64_t{1} << bit;
                for (const auto value : {boundary - 1, boundary, -boundary, -boundary - 1}) values.push_back(value);
            }
            check({kind}, values);
        }
        for (const auto [kind, width] : {std::pair{K::Uint8, 8u}, {K::Uint16, 16u}, {K::Uint32, 32u}, {K::Uint64, 64u}}) {
            std::vector<TScalar> values{std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max() >> (64 - width)};
            for (unsigned bit = 7; bit < width; bit += 7) {
                const auto boundary = std::uint64_t{1} << bit;
                values.push_back(boundary - 1);
                values.push_back(boundary);
            }
            check({kind}, values);
        }
        for (const auto kind : {K::Float, K::Double}) {
            check({kind}, {0.0, -0.0, 1.25, -42.5, std::numeric_limits<double>::denorm_min(),
                std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN(), std::bit_cast<double>(0xfff123456789abcdULL)});
        }
        check({K::Date}, {std::uint64_t{0}, std::uint64_t{127}, std::uint64_t{128}, std::uint64_t{49672}});
        check({K::Datetime}, {std::uint64_t{0}, std::uint64_t{16384}, std::uint64_t{4000000000}});
        check({K::Timestamp}, {std::uint64_t{0}, std::uint64_t{2097152}, std::uint64_t{4000000000000000}});
        check({K::Interval}, {std::int64_t{0}, std::int64_t{-16384}, std::int64_t{4000000000000000}});
        for (const auto kind : {K::String, K::Utf8}) {
            check({kind}, {std::string{}, std::string("a\0b", 3), std::string(7, 'x'), std::string(8, 'x'), std::string(257, 'x')});
        }
        std::vector<TScalar> decimals;
        for (const __int128 value : {__int128{0}, __int128{-1}, __int128{1}, TDecimal::Inf() - 1,
                -TDecimal::Inf() + 1, TDecimal::Inf(), -TDecimal::Inf(), TDecimal::NaN()}) decimals.push_back(TDecimal{value});
        for (unsigned bit = 8; bit < 112; bit += 8) {
            const auto boundary = __int128{1} << bit;
            for (const auto value : {boundary - 1, boundary, -boundary, -boundary - 1}) decimals.push_back(TDecimal{value});
        }
        check({K::Decimal, false, 35, 2}, decimals);
        TExpressionArena arena;
        CheckScalar(arena, arena.Unary(EOpcode::StablePickle, arena.Null()), "Null key");
    }

    Y_UNIT_TEST(ScalarSubplansRemainNativeThroughTyping) {
        NTests::TIdTestContext f;
        auto roots = CompileNativeToRbo("SELECT (SELECT (SELECT 1) + 2) AS x, (SELECT 7) AS y", NoTables, f.ExprCtx);
        auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native scalar subquery invoked YQL annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext nativeContext(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        auto& root = *roots.front();
        UNIT_ASSERT(!root.PlanProps.Subplans.Empty());
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
        for (const auto& item : IterateSubtreeWithSubplans(&root, root.PlanProps)) {
            for (const auto& expression : item.Current->GetExpressions()) {
                UNIT_ASSERT(expression.get().IsNative());
                UNIT_ASSERT(!expression.get().Node);
                UNIT_ASSERT(expression.get().PlanProps == &root.PlanProps);
            }
        }
    }

    Y_UNIT_TEST(NativeAssertionsExecuteWithoutCompatibilityLowering) {
        using namespace NFrontend;
        TExpressionArena arena;
        const auto value = arena.Literal({ETypeKind::Int32}, std::int64_t{42});
        const auto no = arena.Literal({ETypeKind::Bool}, false);
        const auto yes = arena.Literal({ETypeKind::Bool}, true);
        const auto failed = arena.Ensure(value, no, "Scalar subquery returned more than one row");
        CheckScalar(arena, arena.Ensure(value, yes, "unused"), "successful assertion");
        CheckScalar(arena, arena.If(no, failed, value), "assertion in an unselected branch");
        TExprContext ctx;
        const auto lambda = LowerNativeLambda(arena, failed, ctx, {});
        UNIT_ASSERT(AreAllNodesTypeAnnotated(*lambda));
        UNIT_ASSERT(!NeedsRboCompatibilityLowering(lambda));
        NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
        NYql::NCommon::TMkqlCommonCallableCompiler compiler;
        NYql::NCommon::TMkqlBuildContext build(compiler, *setup.PgmBuilder, ctx);
        auto graph = setup.BuildGraph(NYql::NCommon::MkqlBuildLambda(*lambda, build, {setup.PgmBuilder->NewEmptyStruct()}));
        UNIT_ASSERT_EXCEPTION_CONTAINS(graph->GetValue(), std::exception, "Scalar subquery returned more than one row");
    }

    Y_UNIT_TEST(ConstantFoldingRemainsNative) {
        NTests::TIdTestContext f;
        auto roots = CompileNativeToRbo("SELECT 1 + 2 AS answer WHERE 2 + 3 = 5 LIMIT 1ul + 1ul", NoTables, f.ExprCtx);
        auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native constant folding invoked YQL annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext nativeContext(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        auto& root = *roots.front();
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
        TConstantFoldingStage().RunStage(root, nativeContext);
        bool folded = false, filterFolded = false, limitFolded = false;
        for (const auto& item : root) for (const auto& expression : item.Current->GetExpressions()) {
            UNIT_ASSERT(expression.get().IsNative());
            UNIT_ASSERT(!expression.get().Node);
            const auto& node = (*expression.get().GetNativeArena())[expression.get().GetNativeRoot()];
            folded |= node.Op == NFrontend::EOpcode::Literal && NFrontend::FormatValue({node.Type, node.Value}) == "3";
            if (item.Current->Kind == EOperator::Filter) {
                UNIT_ASSERT(node.Op == NFrontend::EOpcode::Literal && std::get<bool>(node.Value));
                filterFolded = true;
            }
            if (item.Current->Kind == EOperator::Limit) {
                UNIT_ASSERT(node.Op == NFrontend::EOpcode::Literal && std::get<std::uint64_t>(node.Value) == 2);
                limitFolded = true;
            }
        }
        UNIT_ASSERT(folded && filterFolded && limitFolded);
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
    }

    Y_UNIT_TEST(DecimalIntegerOperationsMatchRuntimeArithmetic) {
        namespace D = NYql::NDecimal;
        using namespace NFrontend;
        auto check = []<class T>(ETypeKind kind) {
            for (const auto [precision, scale] : {std::pair{3, 2}, {8, 2}, {35, 2}, {35, 35}}) {
                const TType type{ETypeKind::Decimal, false, static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale)};
                const auto bound = D::GetDivider(precision);
                const __int128 inputs[]{0, 1, -1, 2, -2, bound - 1, 1 - bound, D::Inf(), -D::Inf(), D::Nan()};
                for (const auto a : inputs) for (const T b : {T{0}, T{1}, T{2}, T{3}, T(-1), T(-2), T(-3), std::numeric_limits<T>::lowest(), std::numeric_limits<T>::max()}) {
                    TExpressionArena arena;
                    const auto left = arena.Literal(type, TDecimal{a});
                    const TScalar factor = [&]() -> TScalar {
                        if constexpr (std::is_signed_v<T>) return std::int64_t{b};
                        else return std::uint64_t{b};
                    }();
                    const auto right = arena.Literal({kind}, factor);
                    const auto product = D::TDecimalMultiplicator<T>(precision).Do(a, b);
                    const std::pair<TExprId, __int128> operations[]{
                        {arena.Binary(EOpcode::Multiply, left, right), product}, {arena.Binary(EOpcode::Multiply, right, left), product},
                        {arena.Binary(EOpcode::Divide, left, right), D::TDecimalDivisor<T>().Do(a, b)}};
                    for (const auto& [result, expected] : operations) {
                        UNIT_ASSERT(arena[result].Type == type);
                        const auto value = Evaluate(arena, result);
                        UNIT_ASSERT_C(std::get<TDecimal>(value.Data).Coefficient == expected,
                            TStringBuilder() << type.ToString() << " op " << int(arena[result].Op) << " " << TType{kind}.ToString() << ": "
                                << FormatValue({type, TDecimal{a}}) << ", " << FormatValue({{kind}, factor}) << " -> " << FormatValue(value));
                        const TExprId roots[]{result};
                        const auto folded = arena.FoldConstants(roots).front();
                        UNIT_ASSERT(arena[folded].Type == type);
                        UNIT_ASSERT(std::get<TDecimal>(Evaluate(arena, folded).Data).Coefficient == expected);
                    }
                }
            }
        };
        check.operator()<i8>(ETypeKind::Int8); check.operator()<ui8>(ETypeKind::Uint8);
        check.operator()<i16>(ETypeKind::Int16); check.operator()<ui16>(ETypeKind::Uint16);
        check.operator()<i32>(ETypeKind::Int32); check.operator()<ui32>(ETypeKind::Uint32);
        check.operator()<i64>(ETypeKind::Int64); check.operator()<ui64>(ETypeKind::Uint64);
        for (const auto* type : {"Int8", "Int16", "Int32", "Int64", "Uint8", "Uint16", "Uint32", "Uint64"}) {
            for (const auto* value : {"0", "0.01", "-0.01", "9.99", "inf", "nan"}) {
                const auto sql = TStringBuilder() << "SELECT Decimal('" << value << "',3,2) * " << type << "('127')";
                CheckScalar(std::string_view{sql.data(), sql.size()});
                for (const auto* divisor : {"0", "3", std::string_view(type).starts_with("Int") ? "-3" : "255"}) {
                    const auto division = TStringBuilder() << "SELECT Decimal('" << value << "',3,2) / " << type << "('" << divisor << "')";
                    CheckScalar(std::string_view{division.data(), division.size()});
                }
            }
        }
        CheckScalar("SELECT Decimal('0',3,2) * Uint64('18446744073709551615')");
        CheckScalar("SELECT IF(false, Int64('1')) * Decimal('1.25',3,2)");
        CheckScalar("SELECT Decimal('0',35,35) / Uint64('18446744073709551615')");
        CheckScalar("SELECT Decimal('1.25',3,2) / Int64('-9223372036854775808')");
        CheckScalar("SELECT Decimal('1.25',3,2) / IF(false, Int64('1'))");
        CheckScalar("SELECT IF(false, Decimal('1.25',3,2)) / 2");
    }

    Y_UNIT_TEST(DecimalValuesMatchRuntimeArithmetic) {
        namespace D = NYql::NDecimal;
        using namespace NFrontend;
        using O = EOpcode;
        for (const auto [precision, scale] : {std::pair{2, 0}, {8, 2}, {35, 2}, {35, 35}}) {
            const TType type{ETypeKind::Decimal, false, static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale)};
            __int128 bound = 1;
            for (int i = 0; i < precision; ++i) bound *= 10;
            const __int128 inputs[]{0, 1, -1, 2, -2, bound - 1, 1 - bound, D::Inf(), -D::Inf(), D::Nan()};
            for (const auto a : inputs) for (const auto b : inputs) {
                TExpressionArena arena;
                const auto left = arena.Literal(type, TDecimal{a}), right = arena.Literal(type, TDecimal{b});
                const std::pair<O, __int128> arithmetic[]{
                    {O::Add, D::Add(a, b, precision)}, {O::Subtract, D::Sub(a, b, precision)},
                    {O::Multiply, D::TDecimalMultiplicator<D::TInt128>(precision, scale).Do(a, b)},
                    {O::Divide, D::TDecimalDivisor<D::TInt128>(precision, scale).Do(a, b)}, {O::Modulo, D::Mod(a, b)}};
                for (const auto [op, expected] : arithmetic) {
                    const auto result = arena.Binary(op, left, right);
                    UNIT_ASSERT(arena[result].Type == type);
                    const auto value = Evaluate(arena, result);
                    UNIT_ASSERT_C(std::get<TDecimal>(value.Data).Coefficient == expected,
                        TStringBuilder() << type.ToString() << " op " << int(op) << " ("
                            << FormatValue({type, TDecimal{a}}) << ", " << FormatValue({type, TDecimal{b}}) << ") -> " << FormatValue(value));
                }
                const std::pair<O, bool> comparisons[]{
                    {O::Equal, D::IsEqual(a, b, 0)}, {O::NotEqual, !D::IsEqual(a, b, 0)},
                    {O::Less, D::IsLess(a, b, 0)}, {O::LessEqual, D::IsLessOrEqual(a, b, 0)},
                    {O::Greater, D::IsGreater(a, b, 0)}, {O::GreaterEqual, D::IsGreaterOrEqual(a, b, 0)}};
                for (const auto [op, expected] : comparisons)
                    UNIT_ASSERT_VALUES_EQUAL(std::get<bool>(Evaluate(arena, arena.Binary(op, left, right)).Data), expected);
            }
            for (const std::string text : {"0", "+0", "-0", "1", "1.25", "-.5", "0.005", "0.015", "999.995", "inf", "-INF", "+NaN",
                "99999999999999999999999999999999999", "100000000000000000000000000000000001", "0000000000000000000000001.2300"}) {
                TExpressionArena arena;
                const auto input = arena.Literal({ETypeKind::String}, text);
                const auto value = Evaluate(arena, arena.Cast(input, type));
                UNIT_ASSERT_C(std::get<TDecimal>(value.Data).Coefficient == D::FromString(TString(text), precision, scale),
                    TStringBuilder() << text << " as " << type.ToString());
            }
        }
    }

    Y_UNIT_TEST(DateTextMatchesRuntimeLexicalContract) {
        using namespace NFrontend;
        TExpressionArena arena;
        const auto parsed = arena.Cast(arena.Parameter("text", {ETypeKind::String, true}), {ETypeKind::Date});
        auto check = [&](std::string text, bool lower = false) {
            TEvaluationContext context;
            context.Parameter = [&](std::string_view) { return TValue{{ETypeKind::String, true}, text}; };
            const auto actual = Evaluate(arena, parsed, context);
            const auto expected = NMiniKQL::ValueFromString(NUdf::EDataSlot::Date, NUdf::TStringRef(text.data(), text.size()));
            UNIT_ASSERT_VALUES_EQUAL_C(actual.IsNull(), !expected, text);
            if (expected) UNIT_ASSERT_VALUES_EQUAL_C(std::get<std::uint64_t>(actual.Data), expected.Get<ui16>(), text);
            if (lower) {
                TExpressionArena direct;
                // Keep parsing as a runtime cast; literal casts fold in binding.
                const auto input = direct.Binary(EOpcode::Concat, direct.Literal({ETypeKind::String}, text),
                    direct.Literal({ETypeKind::String}, std::string{}));
                CheckScalar(direct, direct.Cast(input, {ETypeKind::Date}), text);
            }
        };
        for (int year = 1969; year <= 2106; ++year) for (int month = 0; month <= 13; ++month) for (int day = 0; day <= 32; ++day) {
            check(std::to_string(year) + '-' + std::to_string(month) + '-' + std::to_string(day));
            check(std::to_string(year) + '-' + (month < 10 ? "0" : "") + std::to_string(month) + '-' + (day < 10 ? "0" : "") + std::to_string(day));
        }
        for (const std::string text : {"", " ", "1970-01-01", "1969-12-31", "2105-12-31", "2106-01-01", "2000-02-29", "2100-02-29",
                "1998-3-18", "1998-03-8", "01998-03-18", "+1998-03-18", "1998--03-18", "1998-003-18", "1998-03-018",
                " 1998-03-18", "1998-03-18 ", "1998-03-18\n", "1998-03-18Z", "1998-03-18T00:00:00Z", "1998/03/18", "１９９８-03-18"}) check(text, true);
        check(std::string{"1998-03-18\0", 11}, true);
        check(std::string{"1998-03-18\xff", 11}, true);
        TEvaluationContext null;
        null.Parameter = [](std::string_view) { return TValue{{ETypeKind::String, true}, {}}; };
        UNIT_ASSERT(Evaluate(arena, parsed, null).IsNull());
    }

    Y_UNIT_TEST(TextCastsMatchRuntimeLexicalContract) {
        using namespace NFrontend;
        for (const auto kind : {ETypeKind::Bool, ETypeKind::Float, ETypeKind::Double}) {
            const auto slot = kind == ETypeKind::Bool ? NUdf::EDataSlot::Bool :
                kind == ETypeKind::Float ? NUdf::EDataSlot::Float : NUdf::EDataSlot::Double;
            for (const std::string text : {"", " ", " 1", "1 ", "\t1", "1\n", "0", "-0", "+0", "1", "-1", "+1",
                ".5", "1.", "1e2", "-1.25E-2", "1e9999", "-1e9999", "1e-9999", "-1e-9999",
                "0x10", "-0x10", "0x1p2", "nan", "+NaN", "-NaN", "NaN(1)", "inf", "+Inf", "-Inf",
                "infinity", " inf", " true", "true", "TRUE", "false", "FALSE", "yes", "no", "bad", "1junk",
                "3.4028234663852886e38", "1.7976931348623157e308", "2.2250738585072014e-308", "4.9406564584124654e-324"}) {
                TExpressionArena arena;
                const auto input = arena.Literal({ETypeKind::String}, text);
                const auto result = Evaluate(arena, arena.Cast(input, {kind}));
                const auto expected = NMiniKQL::ValueFromString(slot, NUdf::TStringRef(text.data(), text.size()));
                const auto context = TStringBuilder() << TType{kind}.ToString() << ": '" << text << "'";
                UNIT_ASSERT_VALUES_EQUAL_C(result.IsNull(), !expected, context);
                if (!expected) continue;
                if (kind == ETypeKind::Bool) {
                    UNIT_ASSERT_VALUES_EQUAL_C(std::get<bool>(result.Data), expected.Get<bool>(), context);
                } else {
                    const auto value = std::get<double>(result.Data);
                    const auto number = kind == ETypeKind::Float ? double(expected.Get<float>()) : expected.Get<double>();
                    UNIT_ASSERT_C((std::isnan(value) && std::isnan(number)) || value == number, context);
                    if (value == 0) UNIT_ASSERT_VALUES_EQUAL_C(std::signbit(value), std::signbit(number), context);
                }
            }
        }
    }

    Y_UNIT_TEST(DirectRboAndNoYqlAnnotation) {
        NTests::TIdTestContext f;
        auto roots = CompileNativeToRbo(R"(
            SELECT a.id, b.id AS right_id, a.id + 2 AS next
            FROM (SELECT 1 AS id) AS a
            LEFT JOIN (SELECT CAST(1L AS Int32) AS id) AS b ON a.id == b.id
            WHERE a.id > 0 ORDER BY next LIMIT 5
        )", NoTables, f.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(roots.size(), 1);
        auto& root = *roots.front();
        UNIT_ASSERT(root.PlanProps.NativeExpressions);
        bool joinFound = false;
        for (const auto& item : root) {
            for (const auto& expression : item.Current->GetExpressions()) {
                UNIT_ASSERT(expression.get().IsNative());
                UNIT_ASSERT(!expression.get().Node);
            }
            if (item.Current->Kind == EOperator::Join) {
                const auto& join = CastOperator<TOpJoin>(*item.Current);
                UNIT_ASSERT_VALUES_EQUAL(join.JoinKeys.Items().size(), 1);
                UNIT_ASSERT(join.JoinFilters.empty());
                joinFound = true;
            }
        }
        UNIT_ASSERT(joinFound);
        auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native expression invoked YQL type annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext nativeContext(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
        NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
        TGlobalInliningStage inlining("native inlining");
        inlining.RunStage(root, nativeContext);
        TGlobalPruningStage pruning("native pruning");
        pruning.RunStage(root, nativeContext);
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
        for (const auto& item : root) for (const auto& expression : item.Current->GetExpressions()) {
            UNIT_ASSERT(expression.get().IsNative());
            UNIT_ASSERT(!expression.get().Node);
        }
    }

    Y_UNIT_TEST(AggregationAndUnionHaveNativeInputs) {
        NTests::TIdTestContext f;
        auto roots = CompileNativeToRbo(R"(
            SELECT SUM(x) AS total, COUNT(*) AS n
            FROM (SELECT 1 AS x UNION ALL SELECT 2L AS x) AS q
            HAVING SUM(x) > 0
        )", NoTables, f.ExprCtx);
        UNIT_ASSERT(roots[0]->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
        NTests::AssertIdInvariants(*roots[0]->GetInput(), roots[0]->PlanProps);
    }

    Y_UNIT_TEST(RuntimeColumnsParametersAndNumericCasts) {
        using namespace NFrontend;
        TExpressionArena arena;
        const auto column = arena.Column(0, {ETypeKind::Int32, true});
        const auto converted = arena.Cast(column, {ETypeKind::Int64});
        const auto fallback = arena.Literal({ETypeKind::Int64}, std::int64_t{42});
        const TExprId alternatives[]{converted, fallback};
        const auto parameter = arena.Parameter("p", {ETypeKind::Uint64});
        const auto result = arena.Binary(EOpcode::Add, arena.Coalesce(alternatives), arena.Cast(parameter, {ETypeKind::Int64}));
        for (const bool present : {false, true}) for (const ui64 parameterValue : {ui64{5}, std::numeric_limits<ui64>::max()}) {
            TEvaluationContext native;
            native.Column = [&](TColumnId) { return TValue{{ETypeKind::Int32, true}, present ? TScalar{std::int64_t{3}} : TScalar{std::monostate{}}}; };
            native.Parameter = [&](std::string_view) { return TValue{{ETypeKind::Uint64}, parameterValue}; };
            const auto expected = Evaluate(arena, result, native);
            TExprContext ctx;
            const auto lambda = LowerNativeLambda(arena, result, ctx, {});
            UNIT_ASSERT(AreAllNodesTypeAnnotated(*lambda));
            UNIT_ASSERT(!NeedsRboCompatibilityLowering(lambda));
            NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
            auto& pb = *setup.PgmBuilder;
            auto value = present ? pb.NewOptional(pb.NewDataLiteral<i32>(3)) :
                pb.NewEmptyOptional(pb.NewOptionalType(pb.NewDataType(NUdf::TDataType<i32>::Id)));
            auto row = pb.NewStruct({{"0", value}});
            NYql::NCommon::TMkqlCommonCallableCompiler compiler;
            NYql::NCommon::TMkqlBuildContext build(compiler, pb, ctx);
            build.Parameters = pb.NewStruct({{"$p", pb.NewDataLiteral<ui64>(parameterValue)}});
            auto graph = setup.BuildGraph(NYql::NCommon::MkqlBuildLambda(*lambda, build, {row}));
            auto actual = graph->GetValue();
            if (expected.IsNull()) UNIT_ASSERT(!actual);
            else UNIT_ASSERT_VALUES_EQUAL(actual.Get<i64>(), std::get<std::int64_t>(expected.Data));
        }
    }

    Y_UNIT_TEST(TerminalScalarExecution) {
        TString failures;
        for (auto sql : {
            "SELECT 1 + 2 * 3", "SELECT 1 / 0", "SELECT 7 / 2", "SELECT 9 % 4",
            "SELECT 1.0 / 0.0", "SELECT 0.0 / 0.0", "SELECT -1.0 / 0.0", "SELECT 7.0 / 2.0", "SELECT -3 + 2L",
            "SELECT CAST('123' AS Int64)", "SELECT CAST('bad' AS Int32)",
            "SELECT CAST(1 AS Int64)", "SELECT CAST(1 AS Uint8)",
            "SELECT COALESCE(NULL, CAST('bad' AS Int32), 7)",
            "SELECT CASE WHEN true THEN 7 ELSE 1/0 END",
            "SELECT 2 IN (1, NULL)", "SELECT NULL IS NULL", "SELECT NULL == NULL", "SELECT NULL AND NULL", "SELECT 4 IS NOT NULL",
            "SELECT Length('abcd')", "SELECT Contains('abcd', 'bc')",
            "SELECT StartsWith('abcd', 'ab')", "SELECT EndsWith('abcd', 'cd')", "SELECT 'a' || 'b'",
            "SELECT ABS(-3)", "SELECT NOT false", "SELECT true AND NULL", "SELECT true OR NULL",
            "SELECT Date('2020-01-02')", "SELECT Timestamp('2020-01-02T03:04:05Z')",
            "SELECT CAST('12.34' AS Decimal(10,2))",
            "SELECT CAST('1.23' AS Decimal(10,2)) / CAST('2' AS Decimal(10,2))",
            "SELECT CAST('1.23' AS Decimal(10,2)) * CAST('2' AS Decimal(10,2))",
            "SELECT CAST('999' AS Decimal(2,0))",
            "SELECT CAST(CAST('9.9' AS Decimal(2,1)) AS Int32)",
            "SELECT CAST(CAST('0.9' AS Decimal(2,1)) AS Int32)",
            "SELECT CAST('inf' AS Decimal(8,2)) - CAST('inf' AS Decimal(8,2))",
            "SELECT CAST('inf' AS Decimal(8,2)) * CAST('0' AS Decimal(8,2))",
            "SELECT CAST('1' AS Decimal(8,2)) / CAST('0' AS Decimal(8,2))",
            "SELECT CAST('0' AS Decimal(8,2)) / CAST('0' AS Decimal(8,2))",
            "SELECT CAST('1' AS Decimal(8,2)) % CAST('0' AS Decimal(8,2))",
            "SELECT -CAST('nan' AS Decimal(8,2))",
            "SELECT CAST('nan' AS Decimal(8,2)) != CAST('nan' AS Decimal(8,2))",
            "SELECT CAST(CAST('inf' AS Decimal(8,2)) AS Double)",
            "SELECT CAST(CAST('nan' AS Decimal(8,2)) AS Float)",
            "SELECT CAST(CAST('inf' AS Decimal(8,2)) AS String)",
            "SELECT Date('2020-01-01') + Interval('P1D')",
            "SELECT Date('2020-01-01') + DateTime::IntervalFromDays(60)",
            "SELECT DateTime::IntervalFromDays(-49672)",
            "SELECT DateTime::IntervalFromDays(49673)",
            "SELECT DateTime::IntervalFromDays(213503982)",
            "SELECT DateTime::IntervalFromHours(1)",
            "SELECT DateTime2::IntervalFromMinutes(IF(false, 1))",
            "SELECT CAST(Date('2020-01-02') AS Timestamp)",
            "SELECT DateTime::GetYear(Timestamp('2000-02-29T12:34:56.123456Z'))",
            "SELECT DateTime::MakeDate(DateTime::ShiftMonths(Date('2020-01-31'), 1))",
            "SELECT DateTime::MakeDatetime(DateTime::ShiftYears(Timestamp('2000-02-29T12:34:56.123456Z'), 100))",
            "SELECT DateTime::MakeTimestamp(DateTime::ShiftMonths(Timestamp('2020-03-31T12:34:56.123456Z'), -1))",
            "SELECT DateTime::MakeTimestamp(DateTime::ShiftYears(Timestamp('2000-02-29T12:34:56.123456Z'), 4096))",
            "SELECT DateTime::MakeDate(DateTime::ShiftYears(Date('1970-01-01'), -1))",
            "SELECT DateTime::MakeDate(DateTime::ShiftMonths(Date('2105-12-31'), 1))",
            "SELECT DateTime::GetYear(DateTime::ShiftMonths(IF(false, Date('2000-02-29')), 1))",
            "SELECT DateTime::MakeTimestamp(DateTime::Split(IF(false, Timestamp('2020-01-01T00:00:00Z'))))",
            "SELECT Lower('AbC')", "SELECT Upper('AbC')", "SELECT Lower(CAST(NULL AS String))",
            "SELECT Substring('abcdef', 2, 3)", "SELECT Substring('abcdef', NULL, 3)",
            "SELECT Substring('abcdef', 4294967295u, 3)", "SELECT Substring('abcdef', 2, NULL)",
            "SELECT Substring('abcdef', IF(false, 2u), IF(false, 3u))",
            "SELECT CAST(12 AS Double)", "SELECT CAST(12 AS Bool)", "SELECT CAST(true AS Int32)",
            "SELECT CAST(9223372586610589697ul AS Float)", "SELECT CAST(4611686293305294849L AS Float)",
            "SELECT CAST(Double('-4.9406564584124654e-324') AS Uint64)",
            "SELECT CAST(Float('-1e-40') AS Uint8)",
            "SELECT CAST(12 AS String)", "SELECT CAST(Utf8('hello') AS String)",
            "SELECT 'abc' LIKE '%b%'", "SELECT 'ab' LIKE '%a%b%'",
            "SELECT 'ab' LIKE 'a%b'", "SELECT 'abc' LIKE 'a%b'", "SELECT 'abb' LIKE 'a%b'",
            "SELECT 'ab' LIKE 'ab%b'", "SELECT 'aba' LIKE '%ab%ba%'",
            "SELECT 'abba' LIKE 'a%%b%a'", "SELECT Utf8('abc') LIKE '%a%c%'",
            "SELECT IF(false, 'abc') LIKE '%a%b%'", "SELECT NULL LIKE '%a%b%'"
        }) {
            try {
                CheckScalar(sql);
            } catch (const std::exception& error) {
                failures += TStringBuilder() << sql << ": " << error.what() << "\n";
            }
        }
        UNIT_ASSERT_C(failures.empty(), failures);
    }
}
} // namespace NKikimr::NKqp
