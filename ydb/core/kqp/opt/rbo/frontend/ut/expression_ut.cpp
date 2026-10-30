#include <ydb/core/kqp/opt/rbo/frontend/expression.h>

#include <library/cpp/testing/unittest/registar.h>

#include <array>
#include <cmath>
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

    Y_UNIT_TEST(TwoArgumentIfProducesOptionalAndSkipsAbsentBranch) {
        TExpressionArena arena;
        const auto value = Integer(arena, 42);
        const TExprId present[]{arena.Literal({K::Bool}, true), value};
        const auto result = arena.Call("If", present);
        UNIT_ASSERT((arena[result].Type == TType{K::Int64, true}));
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, result)), "42");
        const auto missing = arena.Parameter("missing", {K::Int64});
        for (const auto condition : {arena.Literal({K::Bool}, false), arena.Null()}) {
            const TExprId absent[]{condition, missing};
            UNIT_ASSERT(Evaluate(arena, arena.Call("If", absent)).IsNull());
        }
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
        for (const auto* invalid : {"1.5", " 1", "1 ", "+", "1e2"}) {
            auto input = arena.Literal({K::String}, std::string{invalid});
            UNIT_ASSERT(Evaluate(arena, arena.Cast(input, {K::Int64})).IsNull());
        }
        auto fractional = arena.Literal({K::Double}, 127.5);
        UNIT_ASSERT(Evaluate(arena, arena.Cast(fractional, {K::Int8})).IsNull());
        auto negativeFractional = arena.Literal({K::Double}, -0.5);
        UNIT_ASSERT(Evaluate(arena, arena.Cast(negativeFractional, {K::Uint64})).IsNull());
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

    Y_UNIT_TEST(TextCastsFoldAndDynamicParsingIsRejected) {
        TExpressionArena arena;
        auto text = arena.Literal({K::String}, std::string{"123"});
        auto converted = arena.Cast(text, {K::Int32});
        UNIT_ASSERT(arena[converted].Op == O::Literal);
        UNIT_ASSERT((arena[converted].Type == TType{K::Int32, true}));
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, converted)), "123");
        auto invalid = arena.Cast(arena.Literal({K::String}, std::string{"invalid"}), {K::Date});
        UNIT_ASSERT(arena[invalid].Op == O::Literal);
        UNIT_ASSERT(Evaluate(arena, invalid).IsNull());
        auto date = arena.Cast(arena.Literal({K::String}, std::string{"1970-01-02"}), {K::Date});
        UNIT_ASSERT(arena[date].Op == O::Literal);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, date)), "1");

        auto dynamic = arena.Parameter("text", {K::String});
        try {
            arena.Cast(dynamic, {K::Int32}, {12, 3, 5});
            UNIT_FAIL("Expected dynamic text parsing to be rejected");
        } catch (const TFrontendError& error) {
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Offset, 12);
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 3);
            UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, 5);
        }
        TEvaluationContext context;
        context.Parameter = [](std::string_view) { return TValue{{K::String}, std::string{"\xD0\xAF"}}; };
        auto utf8 = arena.Cast(dynamic, {K::Utf8});
        UNIT_ASSERT(arena[utf8].Op == O::Cast);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, utf8, context)), std::string{"\xD0\xAF"});
    }

    Y_UNIT_TEST(DecimalScaleAndOverflowAreExplicit) {
        TExpressionArena arena;
        auto a = arena.Literal({K::Decimal, false, 8, 2}, TDecimal{123});
        auto b = arena.Literal({K::Decimal, false, 8, 2}, TDecimal{200});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Multiply, a, b))), "2.46");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Binary(O::Divide, a, b))), "0.62");
        auto string = arena.Literal({K::String}, std::string{"-12.345"});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(string, {K::Decimal, false, 8, 2}))), "-12.34");
        UNIT_ASSERT_EXCEPTION(arena.Literal({K::Decimal, false, 2, 1}, TDecimal{100}), TFrontendError);
        UNIT_ASSERT_EXCEPTION((CommonType({K::Decimal, false, 8, 2}, {K::Double})), TFrontendError);
        auto rounding = arena.Literal({K::String}, std::string{"12.355"});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(rounding, {K::Decimal, false, 8, 2}))), "12.36");
        auto roundedInteger = arena.Literal({K::Decimal, false, 8, 2}, TDecimal{150});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(roundedInteger, {K::Int64}))), "2");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(roundedInteger, {K::String}))), "1.5");
        auto carry = arena.Literal({K::Decimal, false, 2, 1}, TDecimal{99});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(carry, {K::Int64}))), "10");
        auto fractional = arena.Literal({K::Decimal, false, 2, 2}, TDecimal{99});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(fractional, {K::Int64}))), "1");
        __int128 oneCoefficient = 1;
        for (unsigned i = 0; i < 30; ++i) oneCoefficient *= 10;
        auto one = arena.Literal({K::Decimal, false, 35, 30}, TDecimal{oneCoefficient});
        UNIT_ASSERT(std::get<TDecimal>(Evaluate(arena, arena.Binary(O::Multiply, one, one)).Data).Coefficient == oneCoefficient);
        UNIT_ASSERT(std::get<TDecimal>(Evaluate(arena, arena.Binary(O::Divide, one, one)).Data).Coefficient == oneCoefficient);
        auto huge = arena.Literal({K::Decimal, false, 2, 0}, TDecimal{99});
        UNIT_ASSERT_EXCEPTION(Evaluate(arena, arena.Binary(O::Add, huge, huge)), TFrontendError);
        UNIT_ASSERT_EXCEPTION(arena.Cast(arena.Literal({K::Double}, 1.2), {K::Decimal, false, 8, 2}), TFrontendError);
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
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(nextDay, {K::Timestamp}))), "86400000000");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(nextDay, {K::Datetime}))), "86400");
        auto lateNextDay = temporal("1970-01-02T12:34:56.123Z", K::Timestamp);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(lateNextDay, {K::Date}))), "1");
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

    Y_UNIT_TEST(TemporalConstructorsRequireTimezoneAndPreserveMicroseconds) {
        TExpressionArena arena;
        auto constructor = [&](std::string_view name, std::string text) {
            const TExprId argument[]{arena.Literal({K::String}, std::move(text))};
            return arena.Call(name, argument);
        };
        auto date = constructor("Date", "1970-1-2");
        UNIT_ASSERT(!arena[date].Type.Nullable);
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, date)), "1");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena,
            constructor("Datetime", "1970-01-01T01:30:00+01:30"))), "0");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena,
            constructor("Datetime", "1970-1-1T0:0:0-1:30"))), "5400");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena,
            constructor("Timestamp", "1970-01-01T01:00:01.23456789+01:00"))), "1234567");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena,
            constructor("Timestamp", "1970-01-01T00:00:00.0000009Z"))), "0");
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(arena,
            constructor("Timestamp", "2105-12-31T23:59:59.999999Z")).Data),
            std::uint64_t{49'673} * 86'400'000'000ULL - 1);

        for (const auto* value : {"1970-01-01T00:00:00", "1970-01-01T00:00:00z", "1970-01-01T00:00:00+00",
            "1970-01-01T00:00:00+00:01", "2105-12-31T23:59:59-00:01", "2106-01-01T00:00:00+01:00"}) {
            UNIT_ASSERT_EXCEPTION(constructor("Datetime", value), TFrontendError);
            const auto text = arena.Literal({K::String}, std::string(value));
            UNIT_ASSERT(Evaluate(arena, arena.Cast(text, {K::Datetime})).IsNull());
        }
        UNIT_ASSERT_EXCEPTION(constructor("Timestamp", "1970-01-01T00:00:00.Z"), TFrontendError);
        UNIT_ASSERT_EXCEPTION(constructor("Datetime", "1970-01-01T00:00:00.1Z"), TFrontendError);
        UNIT_ASSERT_EXCEPTION(constructor("Date", "2106-01-01"), TFrontendError);
    }

    Y_UNIT_TEST(FloatingPointTextIsLocaleIndependent) {
        TExpressionArena arena;
        auto text = arena.Literal({K::String}, std::string{"1.25e2"});
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(text, {K::Double}))), "125");
        auto junk = arena.Literal({K::String}, std::string{"1.25suffix"});
        UNIT_ASSERT(Evaluate(arena, arena.Cast(junk, {K::Double})).IsNull());
    }

    Y_UNIT_TEST(Utf8CastsValidateAndFloatsRoundAtConstruction) {
        TExpressionArena arena;
        auto valid = arena.Literal({K::String}, std::string{"\xD0\xAF"});
        UNIT_ASSERT(!Evaluate(arena, arena.Cast(valid, {K::Utf8})).IsNull());
        auto invalid = arena.Literal({K::String}, std::string{"\xC0\xAF"});
        UNIT_ASSERT(Evaluate(arena, arena.Cast(invalid, {K::Utf8})).IsNull());
        UNIT_ASSERT_EXCEPTION(arena.Literal({K::Utf8}, std::string{"\xED\xA0\x80"}), TFrontendError);
        auto single = arena.Literal({K::Float}, 0.1);
        UNIT_ASSERT(std::get<double>(arena[single].Value) == static_cast<double>(0.1F));
        auto utf8 = arena.Literal({K::Utf8}, std::string{"\xD0\xAF"});
        const std::array<TExprId, 3> args{arena.Cast(utf8, {K::String}), Integer(arena, 1), Integer(arena, 1)};
        auto byteSlice = arena.Call("Substring", args);
        UNIT_ASSERT(arena[byteSlice].Type.Kind == K::String);
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(Evaluate(arena, byteSlice).Data), std::string{"\xAF"});
    }

    Y_UNIT_TEST(FloatingConversionsPreserveNonFiniteValues) {
        TExpressionArena arena;
        auto infinity = arena.Literal({K::Float}, std::numeric_limits<double>::infinity());
        auto wide = arena.Convert(infinity, {K::Double});
        UNIT_ASSERT(std::isinf(std::get<double>(Evaluate(arena, wide).Data)));
        auto nan = arena.Literal({K::Float}, std::numeric_limits<double>::quiet_NaN());
        UNIT_ASSERT(std::isnan(std::get<double>(Evaluate(arena, arena.Convert(nan, {K::Double})).Data)));
        UNIT_ASSERT(Evaluate(arena, arena.Cast(nan, {K::Bool})).IsNull());
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(infinity, {K::Bool}))), "true");
        auto denormal = arena.Literal({K::Float}, static_cast<double>(std::numeric_limits<float>::denorm_min()));
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, arena.Cast(denormal, {K::Bool}))), "false");
        auto text = arena.Literal({K::String}, std::string{"-Inf"});
        UNIT_ASSERT(std::isinf(std::get<double>(Evaluate(arena, arena.Cast(text, {K::Double})).Data)));
    }

    Y_UNIT_TEST(FloatingDivisionPreservesIeeeValuesAndNullability) {
        TExpressionArena arena;
        for (const auto kind : {K::Float, K::Double}) {
            const auto one = arena.Literal({kind}, 1.0);
            const auto zero = arena.Literal({kind}, 0.0);
            const auto infinity = arena.Binary(O::Divide, one, zero);
            UNIT_ASSERT(arena[infinity].Type == (TType{kind}));
            UNIT_ASSERT(std::isinf(std::get<double>(Evaluate(arena, infinity).Data)));
            UNIT_ASSERT(std::isnan(std::get<double>(Evaluate(arena, arena.Binary(O::Divide, zero, zero)).Data)));
            const auto absent = arena.Cast(arena.Null(), {kind});
            const auto nullable = arena.Binary(O::Divide, one, absent);
            UNIT_ASSERT(arena[nullable].Type == (TType{kind, true}));
            UNIT_ASSERT(Evaluate(arena, nullable).IsNull());
            const auto converted = arena.Cast(Integer(arena, 7), {kind});
            UNIT_ASSERT(arena[converted].Type == (TType{kind}));
        }
    }

    Y_UNIT_TEST(SubstringDefaultsAndTypeAreResolvedAtConstruction) {
        TExpressionArena arena;
        const auto text = arena.Literal({K::String}, std::string{"abcdef"});
        const auto null = arena.Null();
        for (const auto& [args, expected] : std::vector<std::pair<std::vector<TExprId>, std::string>>{
            {{text, null}, "abcdef"}, {{text, Integer(arena, 2), null}, "cdef"},
            {{text, null, Integer(arena, 2)}, "ab"}, {{text, Integer(arena, 99)}, ""}}) {
            const auto result = arena.Call("Substring", args);
            UNIT_ASSERT(arena[result].Type == (TType{K::String}));
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, result)), expected);
        }
        const auto offset = arena.Parameter("start", {K::Uint32, true});
        const auto count = arena.Parameter("count", {K::Uint32, true});
        const TExprId args[]{text, offset, count};
        const auto result = arena.Call("Substring", args);
        UNIT_ASSERT(arena[result].Type == (TType{K::String}));
        TEvaluationContext context;
        context.Parameter = [](std::string_view) { return TValue{{K::Uint32, true}, {}}; };
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, result, context)), "abcdef");
        const TExprId invalid[]{text, Integer(arena, -1)};
        UNIT_ASSERT_EXCEPTION(arena.Call("Substring", invalid), TFrontendError);
        const TExprId wide[]{text, arena.Parameter("wide", {K::Uint64})};
        UNIT_ASSERT_EXCEPTION(arena.Call("Substring", wide), TUnsupportedFeature);
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

    Y_UNIT_TEST(ColumnRewriteRetainsUnchangedSubgraphsAndOriginalRoots) {
        TExpressionArena arena;
        auto left = arena.Column(1, {K::Int32});
        auto right = arena.Column(2, {K::Int32});
        auto one = arena.Literal({K::Int32}, std::int64_t{1});
        auto unchanged = arena.Binary(O::Add, right, one);
        auto branch = arena.Binary(O::Add, left, unchanged);
        auto root = arena.Binary(O::Add, branch, branch);
        const auto before = arena.Size();
        unsigned visits = 0;
        auto rewritten = arena.RewriteColumns(root, [&](TExprId original, TColumnId column, TType type, TSourcePosition position) {
            ++visits;
            return column == 1 ? arena.Column(3, type, position) : original;
        });
        UNIT_ASSERT_VALUES_EQUAL(visits, 2);
        UNIT_ASSERT_VALUES_EQUAL(arena.Size(), before + 3); // Changed column, branch, root.
        const auto newBranch = arena[rewritten].Arguments[0];
        UNIT_ASSERT_VALUES_EQUAL(newBranch, arena[rewritten].Arguments[1]);
        UNIT_ASSERT_VALUES_EQUAL(arena[newBranch].Arguments[1], unchanged);
        UNIT_ASSERT_VALUES_EQUAL(arena[branch].Arguments[0], left);
        UNIT_ASSERT_VALUES_EQUAL(arena[left].ColumnId, 1);
        TEvaluationContext context;
        context.Column = [](TColumnId id) { return TValue{{K::Int32}, std::int64_t{id == 3 ? 100 : id == 1 ? 10 : 20}}; };
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, root, context)), "62");
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(arena, rewritten, context)), "242");
        const auto after = arena.Size();
        auto identical = arena.RewriteColumns(rewritten, [](TExprId id, TColumnId, TType, TSourcePosition) { return id; });
        UNIT_ASSERT_VALUES_EQUAL(identical, rewritten);
        UNIT_ASSERT_VALUES_EQUAL(arena.Size(), after);
    }

    Y_UNIT_TEST(ColumnRewriteIsSimultaneousAndRetypesChangedPaths) {
        TExpressionArena arena;
        auto a = arena.Column(1, {K::Int32});
        auto b = arena.Column(2, {K::Int32});
        auto root = arena.Binary(O::Subtract, a, b);
        auto swapped = arena.RewriteColumns(root, [&](TExprId, TColumnId column, TType type, TSourcePosition position) {
            return arena.Column(column == 1 ? 2 : 1, type, position);
        });
        UNIT_ASSERT_VALUES_EQUAL(arena[arena[swapped].Arguments[0]].ColumnId, 2);
        UNIT_ASSERT_VALUES_EQUAL(arena[arena[swapped].Arguments[1]].ColumnId, 1);
        auto widened = arena.RewriteColumns(root, [&](TExprId id, TColumnId column, TType, TSourcePosition position) {
            return column == 1 ? arena.Column(column, {K::Int64}, position) : id;
        });
        UNIT_ASSERT(arena[widened].Type == TType{K::Int64});
        const auto convertedRight = arena[widened].Arguments[1];
        UNIT_ASSERT(arena[convertedRight].Op == O::Convert);
        UNIT_ASSERT_VALUES_EQUAL(arena[convertedRight].Arguments[0], b);
        UNIT_ASSERT(arena[root].Type == TType{K::Int32});
        UNIT_ASSERT_EXCEPTION(arena.RewriteColumns(root, [&](TExprId id, TColumnId column, TType, TSourcePosition position) {
            return column == 1 ? arena.Column(column, {K::Bool}, position) : id;
        }), TFrontendError);
        UNIT_ASSERT(arena[root].Type == TType{K::Int32});
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
        UNIT_ASSERT_VALUES_EQUAL(arena.RewriteColumns(expression, {}), expression);
        UNIT_ASSERT_VALUES_EQUAL(arena.Size(), 100001);
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
