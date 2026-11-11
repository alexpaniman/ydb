#include "kqp_rbo_test_helpers.h"
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/rbo/frontend/predicate.h>
#include <ydb/core/kqp/opt/kqp_opt.h>
#include <yql/essentials/core/yql_expr_constraint.h>
#include <yql/essentials/providers/common/provider/yql_data_provider_impl.h>
#include <yql/essentials/providers/common/transform/yql_visit.h>
#include <ydb/core/kqp/opt/physical/predicate_collector.h>
#include <yql/essentials/core/peephole_opt/yql_opt_peephole_physical.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

namespace {

// Supply the real KQP closure annotator and the provider's default constraint
// transformer, without catalog I/O or a query executor. Common peepholes need
// both provider hooks even though this fixture contains only scalar expressions.
class TLegacyPredicateProvider final : public NYql::TDataProviderBase {
public:
    explicit TLegacyPredicateProvider(NTests::TIdTestContext& f)
        : Annotation(NOpt::CreateKqpTypeAnnotationTransformer("ut", f.Tables, f.Config))
        , Constraints(NYql::CreateDefCallableConstraintTransformer()) {}

    TStringBuf GetName() const override { return "kikimr"; }
    bool CanParse(const NYql::TExprNode& node) override {
        return NYql::NNodes::TKqpPredicateClosure::Match(&node);
    }
    NYql::IGraphTransformer& GetTypeAnnotationTransformer(bool) override { return *Annotation; }
    NYql::IGraphTransformer& GetConstraintTransformer(bool, bool) override { return *Constraints; }

private:
    THolder<NYql::TVisitorTransformerBase> Annotation;
    TAutoPtr<NYql::IGraphTransformer> Constraints;
};

// The pre-native rule coalesced filter truth and ran these peepholes before
// collection. This is an independent test oracle, never a production adapter.
NYql::NNodes::TCoLambda NormalizeLegacyPredicate(const NYql::TExprNode::TPtr& predicate,
    const NYql::TExprNode::TPtr& row, NTests::TIdTestContext& f, NYql::TTypeAnnotationContext& types) {
    using namespace NYql;
    using namespace NYql::NNodes;
    auto& ctx = f.ExprCtx;
    const auto pos = predicate->Pos();
    auto argument = ctx.NewArgument(pos, "normalized_row");
    argument->SetTypeAnn(row->GetTypeAnn());
    argument->SetState(TExprNode::EState::ConstrComplete);
    auto body = ctx.NewCallable(pos, "Coalesce", {ctx.ReplaceNode(TExprNode::TPtr(predicate), *row, argument),
        ctx.NewCallable(pos, "Bool", {ctx.NewAtom(pos, "false")})});
    auto closure = ctx.NewCallable(pos, TKqpPredicateClosure::CallableName(), {
        ctx.NewLambda(pos, ctx.NewArguments(pos, {argument}), std::move(body)),
        ExpandType(pos, *ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{row->GetTypeAnn()}), ctx)});
    TExprNode::TPtr normalized;
    bool nonDeterministic = false;
    UNIT_ASSERT_VALUES_EQUAL_C(PeepHoleOptimizeNode(closure, normalized, ctx, types, nullptr, nonDeterministic),
        IGraphTransformer::TStatus::Ok, ctx.IssueManager.GetIssues().ToString());
    return TKqpPredicateClosure(normalized).Lambda();
}

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeOlapPolicy) {
    Y_UNIT_TEST(PredicateFormsMatchExistingNormalizationAndCollector) {
        using namespace NFrontend;
        using namespace NYql;
        using K = ETypeKind;
        using O = EOpcode;
        struct TPair { TExprId Native; NYql::TExprNode::TPtr Legacy; };
        TString mismatches;
        unsigned tested = 0;
        for (const bool nullable : {false, true}) {
            NTests::TIdTestContext f;
            auto& arena = *f.Props.NativeExpressions;
            auto& ctx = f.ExprCtx;
            const auto types = MakeIntrusive<TTypeAnnotationContext>();
            types->AddDataSink("kikimr", MakeIntrusive<TLegacyPredicateProvider>(f));
            const TPositionHandle pos;
            const TType integer{K::Int32, nullable}, boolean{K::Bool, nullable}, string{K::String, nullable};
            const auto* rowType = ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{
                ctx.MakeType<TItemExprType>("x", ExportNativeType(integer, ctx)),
                ctx.MakeType<TItemExprType>("y", ExportNativeType(integer, ctx)),
                ctx.MakeType<TItemExprType>("b", ExportNativeType(boolean, ctx)),
                ctx.MakeType<TItemExprType>("s", ExportNativeType(string, ctx))});
            const auto row = ctx.NewArgument(pos, "row"); row->SetTypeAnn(rowType);
            const auto pair = [&](TExprId id, TStringBuf callable, NYql::TExprNode::TListType args) {
                auto legacy = ctx.NewCallable(pos, callable, std::move(args));
                return TPair{id, std::move(legacy)};
            };
            const auto column = [&](unsigned id, TType type, TStringBuf name) {
                return pair(arena.Column(id, type), "Member", {row, ctx.NewAtom(pos, name)});
            };
            const auto xId = f.Id("x"), yId = f.Id("y"), bId = f.Id("b"), sId = f.Id("s");
            const auto x = column(xId, integer, "x"), y = column(yId, integer, "y");
            const auto b = column(bId, boolean, "b"), s = column(sId, string, "s");
            const auto one = pair(arena.Literal({K::Int32}, std::int64_t{1}), "Int32", {ctx.NewAtom(pos, "1")});
            const auto yes = pair(arena.Literal({K::Bool}, true), "Bool", {ctx.NewAtom(pos, "true")});
            const auto no = pair(arena.Literal({K::Bool}, false), "Bool", {ctx.NewAtom(pos, "false")});
            const auto text = pair(arena.Literal({K::String}, std::string{"a"}), "String", {ctx.NewAtom(pos, "a")});
            const auto binary = [&](O op, TStringBuf name, const TPair& left, const TPair& right) {
                return pair(arena.Binary(op, left.Native, right.Native), name, {left.Legacy, right.Legacy});
            };
            const auto unary = [&](O op, TStringBuf name, const TPair& value) {
                return pair(arena.Unary(op, value.Native), name, {value.Legacy});
            };
            const auto coalesce = [&](const TPair& left, const TPair& right) {
                const TExprId args[]{left.Native, right.Native};
                return pair(arena.Coalesce(args), "Coalesce", {left.Legacy, right.Legacy});
            };
            const auto condition = [&](const TPair& test, const TPair& left, const TPair& right) {
                const auto truth = arena[test.Native].Type.OptionalDepth
                    ? ctx.NewCallable(pos, "Coalesce", {test.Legacy, no.Legacy}) : test.Legacy;
                return pair(arena.If(test.Native, left.Native, right.Native), "If", {truth, left.Legacy, right.Legacy});
            };
            const auto compare = binary(O::Less, "<", x, y);
            const auto sum = binary(O::Add, "+", x, y);
            const auto contains = binary(O::Contains, "StringContains", s, text);
            const auto exists = unary(O::IsNotNull, "Exists", x);
            struct TCase { TStringBuf Name; TPair Value; bool NullableStringValue = false; };
            const TVector<TCase> cases{
                {"boolean column", b}, {"comparison", compare},
                {"not column", unary(O::Not, "Not", b)},
                {"not comparison", unary(O::Not, "Not", compare)},
                {"column and comparison", binary(O::And, "And", b, compare)},
                {"column or comparison", binary(O::Or, "Or", b, compare)},
                {"comparison and exists", binary(O::And, "And", compare, exists)},
                {"comparison or exists", binary(O::Or, "Or", compare, exists)},
                {"comparison and true", binary(O::And, "And", compare, yes)},
                {"comparison or false", binary(O::Or, "Or", compare, no)},
                {"coalesce column", coalesce(b, no)},
                {"coalesce comparison", coalesce(compare, no)},
                {"if column", condition(b, yes, no)},
                {"if comparison", condition(compare, yes, no)},
                {"if predicates", condition(compare, exists, unary(O::Not, "Not", compare))},
                {"just column", unary(O::OptionalWrap, "Just", b)},
                {"just comparison", unary(O::OptionalWrap, "Just", compare)},
                {"column exists", exists},
                {"computed exists", unary(O::IsNotNull, "Exists", sum)},
                {"computed null", unary(O::Not, "Not", unary(O::IsNotNull, "Exists", sum))},
                {"addition comparison", binary(O::Less, "<", sum, one)},
                {"same-column addition", binary(O::Less, "<", binary(O::Add, "+", x, x), one)},
                {"negation comparison", binary(O::Less, "<", unary(O::Negate, "Minus", x), one)},
                {"contains", contains}, {"not contains", unary(O::Not, "Not", contains), true},
                {"coalesce contains", coalesce(contains, yes), true},
            };
            for (const auto& [name, value, nullableStringValue] : cases) for (const bool apply : {false, true}) {
                // Pre-PR RBO rejects Bool?? filters before the OLAP rule.
                if (arena[value.Native].Type.OptionalDepth > 1) continue;
                const auto normalized = NormalizeLegacyPredicate(value.Legacy, row, f, *types);
                NOpt::TOLAPPredicateNode collected;
                NOpt::CollectPredicates(normalized.Body(), collected, normalized.Args().Arg(0).Raw(), rowType, {apply, false});
                const bool admitted = apply ? collected.CanBePushedApply : collected.CanBePushed;
                // Nullable string kernels erase NULL in value-sensitive contexts;
                // the existing native runtime parity tests require scalar apply.
                const bool expected = admitted && !(nullable && nullableStringValue && !apply);
                f.Config->SetEnableOlapScalarApply(apply);
                auto read = f.Read({xId, yId, bId, sId});
                read->StorageType = NYql::EStorageType::ColumnStorage;
                const TExprId roots[]{PrepareFilterPredicate(arena, value.Native)};
                const auto root = arena.FoldConstants(roots).front();
                const auto filter = MakeIntrusive<TOpFilter>(read, f.Pos,
                    TExpression::FromNative(f.Props.NativeExpressions, root, &f.Props));
                TPushOlapFilterRule rule;
                const auto result = rule.SimpleMatchAndApply(filter, f.RboCtx, f.Props);
                ++tested;
                const bool actual = result->Kind == EOperator::Source && CastOperator<TOpRead>(*result).OlapProgram;
                if (actual != expected) mismatches += TStringBuilder() << name << " nullable=" << nullable
                    << " scalarApply=" << apply << " native=" << actual << " legacy=" << expected
                    << " normalized=" << KqpExprToPrettyString(normalized.Body(), ctx) << '\n';
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(tested, 100);
        UNIT_ASSERT_C(mismatches.empty(), mismatches);
    }

}
} // namespace NKikimr::NKqp
