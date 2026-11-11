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
    Y_UNIT_TEST(RegexAdmissionPreservesExistingConfiguration) {
        using namespace NFrontend;
        using namespace NYql;
        for (const bool nullable : {false, true})
        for (const auto op : {EOpcode::RegexMatch, EOpcode::RegexGrep, EOpcode::Like, EOpcode::ILike})
        for (const bool parameter : {false, true}) for (const bool negate : {false, true}) {
            NTests::TIdTestContext f;
            auto& arena = *f.Props.NativeExpressions;
            auto& ctx = f.ExprCtx;
            const auto id = f.Id("s");
            const TType string{ETypeKind::String, nullable};
            const auto pattern = parameter ? arena.Parameter("pattern", {ETypeKind::String})
                : arena.Literal({ETypeKind::String}, std::string{"a.*"});
            const bool fullMatch = op != EOpcode::RegexGrep;
            auto root = op == EOpcode::Like || op == EOpcode::ILike
                ? arena.Like(arena.Column(id, string), pattern, {}, {}, op == EOpcode::Like)
                : arena.Regex(arena.Column(id, string), pattern, fullMatch);
            if (negate) root = arena.Unary(EOpcode::Not, root);

            // Independently build the typed legacy runtime call. The policy
            // oracle consumes its public UDF signature and configuration,
            // never the native emitter's result or eligibility decision.
            const auto atom = [&](TStringBuf text) {
                auto node = ctx.NewAtom(f.Pos, text); node->SetTypeAnn(ctx.MakeType<TUnitExprType>()); return node;
            };
            const auto typed = [&](TStringBuf name, NYql::TExprNode::TListType args, const TTypeAnnotationNode* type) {
                auto node = ctx.NewCallable(f.Pos, name, std::move(args)); node->SetTypeAnn(type); return node;
            };
            const auto syntax = [&](const TTypeAnnotationNode* type) { return LowerExecutionType(type, ctx, f.Pos); };
            const auto* textType = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::String);
            const auto* boolType = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Bool);
            const auto* optionalText = ctx.MakeType<TOptionalExprType>(textType);
            const auto* fieldType = nullable ? static_cast<const TTypeAnnotationNode*>(optionalText) : textType;
            const auto* rowType = ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{ctx.MakeType<TItemExprType>("s", fieldType)});
            auto row = ctx.NewArgument(f.Pos, "row"); row->SetTypeAnn(rowType);
            auto member = typed("Member", {row, atom("s")}, fieldType);
            if (!nullable) member = typed("Just", {member}, optionalText);
            auto legacyPattern = parameter ? typed("Parameter", {atom("pattern"), syntax(textType)}, textType)
                : typed("String", {atom("a.*")}, textType);
            TVector<const TItemExprType*> fields;
            for (const TStringBuf name : {"CaseSensitive", "DotNl", "Literal", "LogErrors", "LongestMatch", "MaxMem",
                    "NeverCapture", "NeverNl", "OneLine", "PerlClasses", "PosixSyntax", "Utf8", "WordBoundary"})
                fields.push_back(ctx.MakeType<TItemExprType>(name, name == "MaxMem"
                    ? ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64) : boolType));
            const auto* optionsType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TStructExprType>(fields));
            auto config = ctx.NewList(f.Pos, {legacyPattern, typed("Nothing", {syntax(optionsType)}, optionsType)});
            config->SetTypeAnn(ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{textType, optionsType}));
            const auto* signature = ctx.MakeType<TCallableExprType>(boolType,
                TVector<TCallableExprType::TArgumentInfo>{{optionalText, {}, 0}}, 0, TStringBuf{});
            auto settings = ctx.NewList(f.Pos, {});
            settings->SetTypeAnn(ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{}));
            const auto udf = typed("Udf", {atom(fullMatch ? "Re2.Match" : "Re2.Grep"), config,
                syntax(ctx.MakeType<TVoidExprType>()), atom(""), syntax(signature), syntax(config->GetTypeAnn()), atom(""), settings}, signature);
            auto legacy = typed("Apply", {udf, member}, boolType);
            if (negate) legacy = typed("Not", {legacy}, boolType);
            for (const bool apply : {false, true}) for (const bool regexp : {false, true}) {
                NOpt::TOLAPPredicateNode collected;
                NOpt::CollectPredicates(NNodes::TExprBase(legacy), collected, row.Get(), rowType, {apply, false, false, regexp});
                const bool expected = apply ? collected.CanBePushedApply : collected.CanBePushed;
                UNIT_ASSERT_VALUES_EQUAL(expected, apply && regexp);
                UNIT_ASSERT_VALUES_EQUAL(CanPushNativeOlapPredicate(arena, root, apply, regexp), expected);
                f.Config->SetEnableOlapScalarApply(apply);
                f.Config->SetEnableOlapPushdownRegexp(regexp);
                auto read = f.Read({id}); read->StorageType = NYql::EStorageType::ColumnStorage;
                const auto filter = MakeIntrusive<TOpFilter>(read, f.Pos, TExpression::FromNative(f.Props.NativeExpressions, root, &f.Props));
                const auto result = TPushOlapFilterRule{}.SimpleMatchAndApply(filter, f.RboCtx, f.Props);
                UNIT_ASSERT_VALUES_EQUAL(result->Kind == EOperator::Source && bool(CastOperator<TOpRead>(*result).OlapProgram), expected);
                if (expected) {
                    const auto emitted = LowerNativeOlapPredicate(arena, root, ctx, f.Pos);
                    UNIT_ASSERT_VALUES_EQUAL(FindNodes(emitted, [](const auto& node) { return node->IsCallable("KqpOlapApply"); }).size(), 1);
                }
            }
        }
    }

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
