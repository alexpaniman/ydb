#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
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
    }
}
void CheckScalar(std::string_view sql) {
    auto program = NFrontend::NDetail::Compile(sql, [](auto, auto pos) -> NFrontend::TCatalogTable {
        throw NFrontend::TFrontendError(pos, "unexpected table");
    });
    CheckScalar(program.Expressions, Projection(program), sql);
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeFrontend) {
    Y_UNIT_TEST(CorrelatedRewritesKeepNativeExpressionsAndBindings) {
        for (const auto* sql : {
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
            "SELECT CAST(Date('2020-01-02') AS Timestamp)",
            "SELECT Lower('AbC')", "SELECT Upper('AbC')", "SELECT Lower(CAST(NULL AS String))",
            "SELECT Substring('abcdef', 2, 3)", "SELECT Substring('abcdef', NULL, 3)",
            "SELECT Substring('abcdef', 4294967295u, 3)", "SELECT Substring('abcdef', 2, NULL)",
            "SELECT Substring('abcdef', IF(false, 2u), IF(false, 3u))",
            "SELECT CAST(12 AS Double)", "SELECT CAST(12 AS Bool)", "SELECT CAST(true AS Int32)",
            "SELECT CAST(9223372586610589697ul AS Float)", "SELECT CAST(4611686293305294849L AS Float)",
            "SELECT CAST(Double('-4.9406564584124654e-324') AS Uint64)",
            "SELECT CAST(Float('-1e-40') AS Uint8)",
            "SELECT CAST(12 AS String)", "SELECT CAST(Utf8('hello') AS String)",
            "SELECT 'abc' LIKE '%b%'"
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
