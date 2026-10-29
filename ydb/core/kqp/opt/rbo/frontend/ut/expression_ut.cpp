#include <ydb/core/kqp/opt/rbo/frontend/expression.h>

#include <library/cpp/testing/unittest/registar.h>

#include <array>
#include <limits>

using namespace NKikimr::NKqp::NFrontend;

namespace {

using K = ETypeKind;
using O = EOpcode;

TExprId Integer(TExpressionArena& arena, std::int64_t value) {
    return arena.Literal({K::Int64}, value);
}

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeExpressions) {
    Y_UNIT_TEST(TypeConstructionIsLocalAndExplicit) {
        TExpressionArena arena;
        auto a = arena.Literal({K::Int32}, std::int64_t{4});
        auto b = arena.Literal({K::Uint32}, std::uint64_t{5});
        auto sum = arena.Binary(O::Add, a, b);
        UNIT_ASSERT(arena[sum].Type == TType{K::Int64});
        for (auto arg : arena[sum].Arguments) {
            UNIT_ASSERT(arena[arg].Op == O::Convert);
            UNIT_ASSERT(arg < sum);
        }
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, sum)), "9");
        UNIT_ASSERT((CommonType({K::Int64}, {K::Uint64}) == TType{K::Decimal, false, 20, 0}));
    }

    Y_UNIT_TEST(ThreeValuedBooleanTables) {
        TExpressionArena arena;
        const std::array<TExprId, 3> inputs{
            arena.Literal({K::Bool}, false), arena.Literal({K::Bool}, true), arena.Cast(arena.Null(), {K::Bool})};
        const std::array<std::array<const char*, 3>, 3> andExpected{{
            {{"false", "false", "false"}}, {{"false", "true", "NULL"}}, {{"false", "NULL", "NULL"}}}};
        const std::array<std::array<const char*, 3>, 3> orExpected{{
            {{"false", "true", "NULL"}}, {{"true", "true", "true"}}, {{"NULL", "true", "NULL"}}}};
        for (std::size_t a = 0; a < 3; ++a) {
            for (std::size_t b = 0; b < 3; ++b) {
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::And, inputs[a], inputs[b]))), andExpected[a][b]);
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Or, inputs[a], inputs[b]))), orExpected[a][b]);
            }
        }
    }

    Y_UNIT_TEST(LazyBranchesDoNotReadMissingBindings) {
        TExpressionArena arena;
        auto missing = arena.Parameter("missing", {K::Int64});
        auto value = Integer(arena, 42);
        auto condition = arena.Literal({K::Bool}, true);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.If(condition, value, missing))), "42");
        const std::array<TExprId, 2> arguments{value, missing};
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Coalesce(arguments))), "42");
        auto absentBool = arena.Parameter("missing_bool", {K::Bool});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Or, condition, absentBool))), "true");
        UNIT_ASSERT_EXCEPTION(Evaluate(arena, missing), TFrontendError);
    }

    Y_UNIT_TEST(CastsAreCheckedAndNullPropagates) {
        TExpressionArena arena;
        auto text = arena.Literal({K::String}, std::string{"123"});
        auto converted = arena.Cast(text, {K::Int16});
        UNIT_ASSERT((arena[converted].Type == TType{K::Int16, true}));
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, converted)), "123");
        auto bad = arena.Literal({K::String}, std::string{"bad"});
        UNIT_ASSERT(Evaluate(arena, arena.Cast(bad, {K::Int64})).IsNull());
        UNIT_ASSERT(Evaluate(arena, arena.Cast(Integer(arena, 256), {K::Uint8})).IsNull());
        auto null = arena.Cast(arena.Null(), {K::Int64});
        UNIT_ASSERT(Evaluate(arena, arena.Binary(O::Equal, null, Integer(arena, 1))).IsNull());
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Unary(O::IsNull, null))), "true");
    }

    Y_UNIT_TEST(IntegerOverflowHasDefinedWrappingSemantics) {
        TExpressionArena arena;
        auto max = Integer(arena, std::numeric_limits<std::int64_t>::max());
        auto min = Integer(arena, std::numeric_limits<std::int64_t>::min());
        auto one = Integer(arena, 1);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Add, max, one))), "-9223372036854775808");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Subtract, min, one))), "9223372036854775807");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Unary(O::Negate, min))), "-9223372036854775808");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Multiply, max, max))), "1");
        UNIT_ASSERT(Evaluate(arena, arena.Binary(O::Divide, min, Integer(arena, -1))).IsNull());
        UNIT_ASSERT(Evaluate(arena, arena.Binary(O::Modulo, min, Integer(arena, -1))).IsNull());
        UNIT_ASSERT(Evaluate(arena, arena.Binary(O::Divide, one, Integer(arena, 0))).IsNull());
        auto narrow = arena.Literal({K::Int8}, std::int64_t{127});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Add, narrow, arena.Literal({K::Int8}, std::int64_t{1})))), "-128");
    }

    Y_UNIT_TEST(DecimalScaleAndOverflowAreExplicit) {
        TExpressionArena arena;
        auto a = arena.Literal({K::Decimal, false, 8, 2}, TDecimal{123});
        auto b = arena.Literal({K::Decimal, false, 8, 2}, TDecimal{200});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Multiply, a, b))), "2.46");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Divide, a, b))), "0.61");
        auto string = arena.Literal({K::String}, std::string{"-12.345"});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(string, {K::Decimal, false, 8, 2}))), "-12.34");
        UNIT_ASSERT_EXCEPTION(arena.Literal({K::Decimal, false, 2, 1}, TDecimal{100}), TFrontendError);
        UNIT_ASSERT_EXCEPTION((CommonType({K::Decimal, false, 8, 2}, {K::Double})), TFrontendError);
    }

    Y_UNIT_TEST(SharedSubexpressionsAreEvaluatedOnce) {
        TExpressionArena arena;
        auto column = arena.Column(7, {K::Int64});
        auto expression = arena.Binary(O::Add, column, column);
        unsigned reads = 0;
        TEvaluationContext context;
        context.Column = [&](TColumnId id) {
            UNIT_ASSERT_VALUES_EQUAL(id, 7);
            ++reads;
            return TValue{{K::Int64}, std::int64_t{3}};
        };
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, expression, context)), "6");
        UNIT_ASSERT_VALUES_EQUAL(reads, 1);
    }

    Y_UNIT_TEST(TemporalLiteralsAndArithmetic) {
        TExpressionArena arena;
        auto temporal = [&](std::string text, K kind) {
            return arena.Cast(arena.Literal({K::String}, std::move(text)), {kind});
        };
        auto epoch = temporal("1970-01-01", K::Date);
        auto nextDay = temporal("1970-01-02", K::Date);
        auto day = temporal("P1D", K::Interval);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, epoch)), "0");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Add, epoch, day))), "1");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Subtract, nextDay, epoch))), "86400000000");
        UNIT_ASSERT(Evaluate(arena, arena.Binary(O::Subtract, epoch, day)).IsNull());
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, temporal("1970-01-01T00:00:01.25Z", K::Timestamp))), "1250000");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, temporal("1970-01-01T01:00:00Z", K::Datetime))), "3600");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, temporal("-PT1.25S", K::Interval))), "-1250000");
        UNIT_ASSERT(!Evaluate(arena, temporal("2000-02-29", K::Date)).IsNull());
        UNIT_ASSERT(Evaluate(arena, temporal("2001-02-29", K::Date)).IsNull());
        UNIT_ASSERT(Evaluate(arena, temporal("1969-12-31", K::Date)).IsNull());
        UNIT_ASSERT(Evaluate(arena, temporal("P1.0D", K::Interval)).IsNull());
        UNIT_ASSERT(Evaluate(arena, temporal("PT1M1H", K::Interval)).IsNull());
    }

    Y_UNIT_TEST(FloatingPointTextIsLocaleIndependent) {
        TExpressionArena arena;
        auto text = arena.Literal({K::String}, std::string{"1.25e2"});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(text, {K::Double}))), "125");
        auto junk = arena.Literal({K::String}, std::string{"1.25suffix"});
        UNIT_ASSERT(Evaluate(arena, arena.Cast(junk, {K::Double})).IsNull());
    }

    Y_UNIT_TEST(SubstitutionRetypesAndPreservesSharing) {
        TExpressionArena original;
        auto a = original.Column(10, {K::Int32});
        auto expression = original.Binary(O::Add, a, a);
        TExpressionArena target;
        unsigned bindings = 0;
        auto copied = target.Import(original, expression, [&](TColumnId id, TType, TSourcePosition pos) {
            UNIT_ASSERT_VALUES_EQUAL(id, 10);
            ++bindings;
            return target.Column(11, {K::Int64}, pos);
        });
        UNIT_ASSERT_VALUES_EQUAL(bindings, 1);
        UNIT_ASSERT(target[copied].Type == TType{K::Int64});
        UNIT_ASSERT_VALUES_EQUAL(target[copied].Arguments[0], target[copied].Arguments[1]);
    }

    Y_UNIT_TEST(DeepExpressionsDoNotRecurse) {
        TExpressionArena arena;
        auto one = Integer(arena, 1);
        auto expression = one;
        for (unsigned i = 0; i < 100000; ++i) {
            expression = arena.Binary(O::Add, expression, one);
        }
        UNIT_ASSERT_VALUES_EQUAL(arena.Size(), 100001);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, expression)), "100001");
        TExpressionArena copied;
        auto result = copied.Import(arena, expression, {});
        UNIT_ASSERT_VALUES_EQUAL(copied.Size(), arena.Size());
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(copied, result)), "100001");
    }

    Y_UNIT_TEST(ErrorsPreserveSourceAndRejectInvalidTypes) {
        TExpressionArena arena;
        auto input = Integer(arena, 1);
        try {
            arena.Unary(O::Not, input, {10, 2, 4});
            UNIT_FAIL("Expected a type error");
        } catch (const TFrontendError& error) {
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Offset, 10);
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 2);
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, 4);
        }
        UNIT_ASSERT_EXCEPTION(ParseType("List<Int64>"), TFrontendError);
        UNIT_ASSERT_EXCEPTION(ParseType("Decimal(36,2)"), TFrontendError);
        UNIT_ASSERT((ParseType("Decimal(12,2)?") == TType{K::Decimal, true, 12, 2}));
    }
}
