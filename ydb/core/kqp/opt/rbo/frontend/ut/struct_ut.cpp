#include <ydb/core/kqp/opt/rbo/frontend/bindings.h>

#include <library/cpp/testing/unittest/registar.h>

#include <array>

using namespace NKikimr::NKqp::NFrontend;

namespace {
using K = ETypeKind;
using O = EOpcode;
using TField = std::pair<std::string, TType>;
using TItem = std::pair<std::string, TExprId>;
}

Y_UNIT_TEST_SUITE(KqpNativeStructs) {
    Y_UNIT_TEST(NamesAndTypesDefineCanonicalShapes) {
        TExpressionArena arena;
        const TField fields[]{{"z", {K::String}}, {"some`field", {K::Int32}}};
        const TField reversed[]{fields[1], fields[0]};
        const auto type = arena.StructType(fields);
        UNIT_ASSERT(type == arena.StructType(reversed));
        UNIT_ASSERT_VALUES_EQUAL(arena.StructNames(type)[0], "some`field");
        UNIT_ASSERT(arena.StructItems(type)[0] == TType{K::Int32});
        const TField renamed[]{{"different", {K::Int32}}, fields[0]};
        UNIT_ASSERT(type != arena.StructType(renamed));
        UNIT_ASSERT(type != arena.TupleType(arena.StructItems(type)));
        UNIT_ASSERT(arena.StructType({}).Shape != arena.TupleType({}).Shape);
        auto invalid = type; invalid.Kind = K::Tuple;
        UNIT_ASSERT_EXCEPTION(arena.ValidateType(invalid), TFrontendError);
        const TField duplicate[]{fields[0], fields[0]};
        UNIT_ASSERT_EXCEPTION(arena.StructType(duplicate), TFrontendError);
        UNIT_ASSERT_EXCEPTION(arena.TupleItems(type), TFrontendError);
        UNIT_ASSERT_EXCEPTION(arena.Literal(type, TTuple(std::make_shared<TTupleValue>())), TFrontendError);
    }

    Y_UNIT_TEST(ConstructionAccessAndFoldingPreserveNamesAndValues) {
        TExpressionArena arena;
        const TItem fields[]{{"z", arena.Literal({K::String}, std::string{"last"})},
            {"some`field", arena.Literal({K::Int32}, std::int64_t{42})}};
        const auto record = arena.Struct(fields);
        const auto member = arena.Member(record, "some`field");
        UNIT_ASSERT(arena[member].Type == TType{K::Int32});
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(arena, member).Data), 42);
        const TExprId roots[]{record, member};
        const auto folded = arena.FoldConstants(roots);
        UNIT_ASSERT(arena[folded[0]].Op == O::Literal);
        UNIT_ASSERT(arena[folded[1]].Op == O::Literal);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, record)), FormatValue(Evaluate(arena, folded[0])));
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(arena, folded[1]).Data), 42);
        UNIT_ASSERT_EXCEPTION(arena.Member(record, "missing"), TFrontendError);
        UNIT_ASSERT_EXCEPTION(arena.Member(fields[0].second, "z"), TFrontendError);
        UNIT_ASSERT_EXCEPTION(arena.InCollection(fields[1].second, record, true), TFrontendError);
        const TItem duplicate[]{fields[0], fields[0]};
        UNIT_ASSERT_EXCEPTION(arena.Struct(duplicate), TFrontendError);
    }

    Y_UNIT_TEST(ImportPreservesNestedShapesAndLiteralOwnership) {
        TExpressionArena source;
        const TItem fields[]{{"value", source.Literal({K::Int64}, std::int64_t{17})}};
        const auto record = source.Struct(fields);
        const TExprId records[]{record, record};
        const auto list = source.Collection(records, true);
        const TItem nestedFields[]{{"list", list}, {"record", record}};
        const auto nested = source.Struct(nestedFields);
        const auto literal = source.FoldConstants(std::span(&nested, 1)).front();
        TExpressionArena target;
        const TType unrelated[]{TType{K::Double}, TType{K::Bool}};
        target.TupleType(unrelated);
        const auto copied = target.Import(source, literal, {});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(source, literal)), FormatValue(Evaluate(target, copied)));
        UNIT_ASSERT_VALUES_EQUAL(target.StructNames(target[copied].Type)[1], "record");
        const auto inner = target.Member(target.Member(copied, "record"), "value");
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(target, inner).Data), 17);
        const auto type = target.ImportType(source, source[nested].Type.Optional());
        UNIT_ASSERT(type.Required() == target[copied].Type);
        UNIT_ASSERT_VALUES_EQUAL(type.OptionalDepth, 1);
    }

    Y_UNIT_TEST(RebindingUsesFieldNamesAndActualArgumentTypes) {
        auto source = std::make_shared<TExpressionArena>();
        const TField formalFields[]{{"value", {K::Int32}}};
        const auto formal = source->Column(40, source->StructType(formalFields));
        const auto root = source->Member(formal, "value");
        const TColumnId parameters[]{40};
        TScalarBinding binding(source, root, parameters);
        TExpressionArena target;
        const TItem actualFields[]{{"before", target.Null()}, {"value", target.Literal({K::String}, std::string{"actual"})}};
        const auto record = target.Struct(actualFields);
        const TExprId arguments[]{record};
        const auto imported = binding.Apply(target, arguments);
        UNIT_ASSERT(target[imported].Type == TType{K::String});
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(Evaluate(target, imported).Data), "actual");
        const auto rewritten = source->RewriteColumns(root, [&](auto, auto, auto, auto) {
            const TItem fields[]{{"value", source->Literal({K::Int64, true}, {})}};
            return source->Struct(fields);
        });
        UNIT_ASSERT(source->operator[](rewritten).Type == (TType{K::Int64, true}));
        UNIT_ASSERT(Evaluate(*source, rewritten).IsNull());
    }

    Y_UNIT_TEST(OptionalRecordsPreserveFieldOptionality) {
        TExpressionArena arena;
        const TField fields[]{{"null", {K::Null}}, {"required", {K::Int32}}, {"optional", {K::Int64, true}}};
        const auto type = arena.StructType(fields);
        const auto record = arena.Parameter("r", type.Optional());
        UNIT_ASSERT(arena[arena.Member(record, "null")].Type == TType{K::Null});
        UNIT_ASSERT(arena[arena.Member(record, "required")].Type == (TType{K::Int32, true}));
        UNIT_ASSERT(arena[arena.Member(record, "optional")].Type == (TType{K::Int64, true}));
        TEvaluationContext context;
        context.Parameter = [&](auto) { return TValue{type.Optional(), {}}; };
        for (const auto& name : arena.StructNames(type)) UNIT_ASSERT(Evaluate(arena, arena.Member(record, name), context).IsNull());
        auto nestedOptional = type; nestedOptional.OptionalDepth = 2;
        UNIT_ASSERT_EXCEPTION(arena.Member(arena.Parameter("nested", nestedOptional), "required"), TFrontendError);
    }

    Y_UNIT_TEST(NamedAndPositionalShapesShareTheSameDepthBound) {
        TExpressionArena arena;
        TType type{K::Int64};
        for (unsigned depth = 0; depth < 64; ++depth) {
            const TField fields[]{{"next", type}};
            type = depth % 2 ? arena.StructType(fields) : arena.ListType(type);
        }
        const TField fields[]{{"next", type}};
        UNIT_ASSERT_EXCEPTION(arena.StructType(fields), TResourceLimitError);
        TExpressionArena target;
        const auto imported = target.ImportType(arena, type);
        target.ValidateType(imported);
    }
}
