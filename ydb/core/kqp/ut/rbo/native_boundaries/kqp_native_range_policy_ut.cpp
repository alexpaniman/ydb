#include "../kqp_rbo_test_helpers.h"
#include "kqp_native_scalar_test_helpers.h"
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_range_bounds.h>
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
    void CheckScalarKeyEligibility(bool computedBound, bool mixedNumericTypes = false) {
        using namespace NFrontend;
        using namespace NYql;
        TString mismatches;
        TVector<std::pair<const char*, const char*>> types;
        if (mixedNumericTypes) {
            for (const auto key : {"Int32", "Uint32", "Int64", "Uint64"})
                for (const auto bound : {"Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64", "Float", "Double"})
                    types.emplace_back(key, bound);
        } else {
            for (const auto name : {"Bool", "Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32",
                    "Int64", "Uint64", "Float", "Double", "String", "Utf8", "Uuid", "Decimal(12,2)",
                    "Date", "Datetime", "Timestamp", "Interval", "Date32", "Datetime64", "Timestamp64", "Interval64"})
                types.emplace_back(name, name);
        }
        for (const auto& [name, boundName] : types)
        for (const bool nullable : {false, true})
        for (const bool nullableBound : {false, true})
        for (const auto& [op, callable] : {std::pair{EOpcode::Equal, "=="}, std::pair{EOpcode::Less, "<"},
                std::pair{EOpcode::LessEqual, "<="}, std::pair{EOpcode::Greater, ">"}, std::pair{EOpcode::GreaterEqual, ">="}}) {
            NTests::TIdTestContext f;
            const auto scalarType = ParseType(name);
            const auto keyType = nullable ? scalarType.Optional() : scalarType;
            const auto boundTypeNative = ParseType(boundName);
            const auto parameterType = nullableBound ? boundTypeNative.Optional() : boundTypeNative;
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
            // These promotions merge distinct integer keys (2^24 + 1 for
            // Float, 2^53 + 1 for Double). The legacy equality extractor loses
            // matching rows; retain the scalar predicate for those cases.
            const bool lossyEquality = op == EOpcode::Equal && scalarType.IsIntegral() &&
                ((boundTypeNative.Kind == ETypeKind::Float &&
                  (scalarType.Kind == ETypeKind::Int32 || scalarType.Kind == ETypeKind::Uint32 ||
                   scalarType.Kind == ETypeKind::Int64 || scalarType.Kind == ETypeKind::Uint64)) ||
                 (boundTypeNative.Kind == ETypeKind::Double &&
                  (scalarType.Kind == ETypeKind::Int64 || scalarType.Kind == ETypeKind::Uint64)));
            if (lossyEquality) {
                UNIT_ASSERT_VALUES_EQUAL(expected.UsedPrefixLen, 1);
                UNIT_ASSERT_VALUES_EQUAL(expected.PointPrefixLen, 1);
                UNIT_ASSERT_VALUES_EQUAL(used, 0);
                UNIT_ASSERT_VALUES_EQUAL(points, 0);
                UNIT_ASSERT(result == filter);
                continue;
            }
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

    Y_UNIT_TEST(MixedNumericBoundsMatchLegacyRangeExtraction) {
        CheckScalarKeyEligibility(false, true);
    }

    Y_UNIT_TEST(ClosedMixedNumericBoundsMatchLegacyRangeExtraction) {
        CheckScalarKeyEligibility(true, true);
    }

    Y_UNIT_TEST(MixedIntegerBoundsExecuteAtDomainEdges) {
        using namespace NFrontend;
        using O = EOpcode;
        const auto integer = [](const TScalar& value) -> __int128 {
            if (const auto* signedValue = std::get_if<std::int64_t>(&value)) return *signedValue;
            return std::get<std::uint64_t>(value);
        };
        struct TDomain { const char* Name; __int128 Minimum; __int128 Maximum; };
        const TDomain domains[]{
            {"Int8", -128, 127}, {"Uint8", 0, 255}, {"Int16", -32768, 32767}, {"Uint16", 0, 65535},
            {"Int32", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()},
            {"Uint32", 0, std::numeric_limits<std::uint32_t>::max()},
            {"Int64", std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()},
            {"Uint64", 0, std::numeric_limits<std::uint64_t>::max()},
        };
        for (const auto keyName : {"Int32", "Uint32", "Int64", "Uint64"})
        for (const auto& domain : domains)
        for (const bool nullableKey : {false, true})
        for (const bool nullableBound : {false, true})
        for (const bool computed : {false, true})
        for (const auto op : {O::Equal, O::Less, O::LessEqual, O::Greater, O::GreaterEqual}) {
            const auto boundName = domain.Name;
            const auto keyScalar = ParseType(keyName), boundScalar = ParseType(boundName);
            const auto keyType = nullableKey ? keyScalar.Optional() : keyScalar;
            const auto boundType = nullableBound ? boundScalar.Optional() : boundScalar;
            auto arena = std::make_shared<TExpressionArena>();
            auto input = arena->Parameter("bound", boundType);
            if (computed) input = arena->If(arena->Parameter("choose", {ETypeKind::Bool}),
                input, arena->Parameter("other", boundType));
            const auto bound = BuildNativeRangeBound(TExpression::FromNative(arena, input), keyType, op);
            UNIT_ASSERT(bound);
            const auto rangeOp = op == O::Equal ? op : op == O::Less || op == O::LessEqual
                ? (bound->Inclusive ? O::LessEqual : O::Less) : (bound->Inclusive ? O::GreaterEqual : O::Greater);
            std::vector<TScalar> keys;
            if (keyScalar.IsSigned()) {
                const auto minimum = keyScalar.Kind == ETypeKind::Int32 ? std::int64_t{std::numeric_limits<std::int32_t>::min()} : std::numeric_limits<std::int64_t>::min();
                const auto maximum = keyScalar.Kind == ETypeKind::Int32 ? std::int64_t{std::numeric_limits<std::int32_t>::max()} : std::numeric_limits<std::int64_t>::max();
                keys = {minimum, minimum + 1, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, maximum - 1, maximum};
            } else {
                const auto maximum = keyScalar.Kind == ETypeKind::Uint32 ? std::uint64_t{std::numeric_limits<std::uint32_t>::max()} : std::numeric_limits<std::uint64_t>::max();
                keys = {std::uint64_t{0}, std::uint64_t{1}, maximum - 1, maximum};
            }
            if (nullableKey) keys.emplace_back();
            std::vector<TExprId> decisions;
            for (const auto& key : keys) {
                auto decision = arena->Binary(rangeOp, arena->Literal(keyType, key), bound->Value);
                if (bound->Validity) decision = arena->Binary(O::And, *bound->Validity, decision);
                const TExprId alternatives[]{decision, arena->Literal({ETypeKind::Bool}, false)};
                decisions.push_back(arena->Coalesce(alternatives));
            }
            const auto root = arena->Collection(decisions, true);
            std::vector<__int128> candidates{domain.Minimum, domain.Minimum + 1, domain.Maximum - 1, domain.Maximum,
                -2147483649LL, -2147483648LL, -1, 0, 1, 2147483647LL, 2147483648LL, 4294967295LL,
                4294967296LL, __int128{9223372036854775807ULL}, __int128{9223372036854775808ULL}};
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            std::vector<TScalar> values;
            for (const auto value : candidates) if (value >= domain.Minimum && value <= domain.Maximum) {
                values.push_back(boundScalar.IsSigned() ? TScalar{static_cast<std::int64_t>(value)}
                    : TScalar{static_cast<std::uint64_t>(value)});
            }
            if (nullableBound) values.emplace_back();
            for (std::size_t index = 0; index < values.size(); ++index) {
                const bool choose = index % 2 == 0;
                const TScalar zero = boundScalar.IsSigned() ? TScalar{std::int64_t{0}} : TScalar{std::uint64_t{0}};
                TEvaluationContext evaluation;
                evaluation.Parameter = [&](std::string_view name) -> TValue {
                    if (name == "choose") return {{ETypeKind::Bool}, choose};
                    const bool selected = !computed || (name == "bound" ? choose : !choose);
                    return {boundType, selected ? values[index] : zero};
                };
                const auto actual = std::get<TTuple>(Evaluate(*arena, root, evaluation).Data);
                for (std::size_t key = 0; key < keys.size(); ++key) {
                    bool expected = false;
                    if (!std::holds_alternative<std::monostate>(keys[key]) && !std::holds_alternative<std::monostate>(values[index])) {
                        const auto a = integer(keys[key]), b = integer(values[index]);
                        expected = op == O::Equal ? a == b : op == O::Less ? a < b : op == O::LessEqual ? a <= b : op == O::Greater ? a > b : a >= b;
                    }
                    UNIT_ASSERT_VALUES_EQUAL_C(std::get<bool>(actual->Items[key].Data), expected,
                        keyName << " op=" << unsigned(op) << " " << boundName << " value=" << index << " key=" << key);
                }
                NTests::CheckScalar(*arena, root, "mixed integer range domain edges", evaluation,
                    [&](NYql::NCommon::TMkqlBuildContext& build, NMiniKQL::TProgramBuilder& pb) {
                        const auto parameter = [&](std::string_view name) {
                            const auto value = evaluation.Parameter(name);
                            const auto literal = arena->Literal(value.Type, value.Data);
                            const auto emitted = LowerNativeLambda(*arena, literal, build.ExprCtx, {})->TailPtr();
                            return NYql::NCommon::MkqlBuildExpr(*emitted, build);
                        };
                        build.Parameters = pb.NewStruct({{"$bound", parameter("bound")}, {"$other", parameter("other")}, {"$choose", parameter("choose")}});
                    });
            }
        }
    }

    Y_UNIT_TEST(FloatingEqualityPreservesAllMatchingIntegerKeys) {
        using namespace NFrontend;
        using O = EOpcode;
        struct TDomain { const char* Name; __int128 Minimum; __int128 Maximum; unsigned Bits; };
        const TDomain domains[]{
            {"Int8", -128, 127, 7}, {"Uint8", 0, 255, 8},
            {"Int16", -32768, 32767, 15}, {"Uint16", 0, 65535, 16},
            {"Int32", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), 31},
            {"Uint32", 0, std::numeric_limits<std::uint32_t>::max(), 32},
            {"Int64", std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), 63},
            {"Uint64", 0, std::numeric_limits<std::uint64_t>::max(), 64},
        };
        for (const auto& domain : domains)
        for (const auto floatingKind : {ETypeKind::Float, ETypeKind::Double})
        for (const bool nullableKey : {false, true})
        for (const bool nullableBound : {false, true})
        for (const bool computed : {false, true}) {
            const auto keyScalar = ParseType(domain.Name);
            const auto keyType = nullableKey ? keyScalar.Optional() : keyScalar;
            const TType boundType{floatingKind, nullableBound};
            const bool exactPromotion = domain.Bits <= (floatingKind == ETypeKind::Float ? 24 : 53);
            const auto floating = [&](double value) { return floatingKind == ETypeKind::Float ? double(float(value)) : value; };
            auto arena = std::make_shared<TExpressionArena>();
            auto input = arena->Parameter("bound", boundType);
            if (computed) input = arena->If(arena->Parameter("choose", {ETypeKind::Bool}),
                input, arena->Parameter("other", boundType));
            const auto bound = BuildNativeRangeBound(TExpression::FromNative(arena, input), keyType, O::Equal);
            UNIT_ASSERT_VALUES_EQUAL(bool(bound), exactPromotion);
            std::vector<__int128> candidates{domain.Minimum, domain.Minimum + 1, domain.Maximum - 1, domain.Maximum,
                -16777217, -16777216, -1, 0, 1, 16777215, 16777216, 16777217, 16777218,
                __int128{9007199254740991LL}, __int128{9007199254740992LL}, __int128{9007199254740993LL}};
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            std::vector<TScalar> keys;
            for (const auto value : candidates) if (value >= domain.Minimum && value <= domain.Maximum)
                keys.push_back(keyScalar.IsSigned() ? TScalar{static_cast<std::int64_t>(value)} : TScalar{static_cast<std::uint64_t>(value)});
            if (nullableKey) keys.emplace_back();
            std::vector<TExprId> decisions;
            for (const auto& key : keys) {
                auto decision = arena->Binary(O::Equal, arena->Literal(keyType, key), bound ? bound->Value : input);
                if (bound && bound->Validity)
                    decision = arena->If(*bound->Validity, decision, arena->Literal({ETypeKind::Bool}, false));
                const TExprId alternatives[]{decision, arena->Literal({ETypeKind::Bool}, false)};
                decisions.push_back(arena->Coalesce(alternatives));
            }
            const auto root = arena->Collection(decisions, true);
            std::vector<TScalar> values;
            for (const double value : {double(domain.Minimum), double(domain.Minimum) - 1, double(domain.Maximum),
                    double(domain.Maximum) + 1, -16777216., -1., -0.5, -0., 0., 0.5, 1., 16777216.,
                    9007199254740992., std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
                values.push_back(floating(value));
            if (nullableBound) values.emplace_back();
            for (std::size_t index = 0; index < values.size(); ++index) {
                const bool choose = index % 2 == 0;
                TEvaluationContext evaluation;
                evaluation.Parameter = [&](std::string_view name) -> TValue {
                    if (name == "choose") return {{ETypeKind::Bool}, choose};
                    const bool selected = !computed || (name == "bound" ? choose : !choose);
                    return {boundType, selected ? values[index] : TScalar{13.0}};
                };
                const auto actual = std::get<TTuple>(Evaluate(*arena, root, evaluation).Data);
                for (std::size_t key = 0; key < keys.size(); ++key) {
                    bool expected = false;
                    if (!std::holds_alternative<std::monostate>(keys[key]) && !std::holds_alternative<std::monostate>(values[index])) {
                        const double number = keyScalar.IsSigned() ? double(std::get<std::int64_t>(keys[key])) : double(std::get<std::uint64_t>(keys[key]));
                        expected = floating(number) == std::get<double>(values[index]);
                    }
                    UNIT_ASSERT_VALUES_EQUAL_C(std::get<bool>(actual->Items[key].Data), expected,
                        domain.Name << " == " << boundType.ToString() << " bound=" << index << " key=" << key);
                }
                NTests::CheckScalar(*arena, root, "floating equality preserves integer keys", evaluation,
                    [&](NYql::NCommon::TMkqlBuildContext& build, NMiniKQL::TProgramBuilder& pb) {
                        const auto parameter = [&](std::string_view name) {
                            const auto value = evaluation.Parameter(name);
                            const auto emitted = LowerNativeLambda(*arena, arena->Literal(value.Type, value.Data), build.ExprCtx, {})->TailPtr();
                            return NYql::NCommon::MkqlBuildExpr(*emitted, build);
                        };
                        build.Parameters = pb.NewStruct({{"$bound", parameter("bound")}, {"$other", parameter("other")}, {"$choose", parameter("choose")}});
                    });
            }
        }
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
                column.TypeInfo = NScheme::TTypeInfo(NScheme::NTypeIds::Int32);
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
