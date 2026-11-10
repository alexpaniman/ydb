#include "kqp_native_scalar_test_helpers.h"

#include <limits>

namespace NKikimr::NKqp {
using NTests::CheckScalar;
Y_UNIT_TEST_SUITE(KqpNativeScalarBoundaries) {
    Y_UNIT_TEST(ScalarMinMaxAndListMetadataEmitTerminalOperations) {
        for (const auto* sql : {
            "SELECT ListLength([1, 2])", "SELECT ListLength([])", "SELECT ListLength(NULL)",
            "SELECT ListHasItems([1])", "SELECT ListHasItems([])",
            "SELECT ListLength(ListFromRange(Just(1), 6, 2))",
            "SELECT ListHasItems(ListFromRange(CAST(NULL AS Int32?), 6))",
            "SELECT Starts_With('abc', 'a')", "SELECT Ends_With('abc', 'c')",
            "SELECT Starts_With(CAST(NULL AS String?), 'a')",
            "SELECT MaxOf(1, 2l)", "SELECT MinOf(1, 2l)",
            "SELECT Greatest(1, 2l, Just(3))", "SELECT Least(1, NULL, 3)",
            "SELECT MaxOf('abc', 'ab')", "SELECT MinOf(TRUE, FALSE)",
            "SELECT MinOf(Just(Date('2020-01-01')), Date('2021-01-01'))",
            "SELECT MaxOf(Decimal('1.25', 9, 2), Decimal('2.50', 9, 2))",
        }) CheckScalar(sql);
        using namespace NFrontend;
        for (const auto kind : {ETypeKind::Float, ETypeKind::Double, ETypeKind::Decimal})
        for (const bool optional : {false, true})
        for (const auto op : {EOpcode::Minimum, EOpcode::Maximum}) {
            const TType type = kind == ETypeKind::Decimal ? TType{kind, optional, 12, 2} : TType{kind, optional};
            const std::vector<TScalar> values = kind == ETypeKind::Decimal
                ? std::vector<TScalar>{TDecimal{-300}, TDecimal{200}, TDecimal{TDecimal::Inf()}, TDecimal{TDecimal::NaN()}}
                : std::vector<TScalar>{-3.0, 2.0, -0.0, 0.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
            for (const auto& left : values) for (const auto& right : values) {
                TExpressionArena arena;
                const auto root = arena.Binary(op, arena.Literal(type, left), arena.Literal(type, right));
                CheckScalar(arena, root, "scalar min/max NULL/NaN/zero contract");
                const auto folded = arena.FoldConstants(std::span{&root, 1}).front();
                UNIT_ASSERT(arena[folded].Op == EOpcode::Literal);
                CheckScalar(arena, folded, "folded scalar min/max contract");
                if (optional) CheckScalar(arena, arena.Binary(op, arena.Literal(type, {}), arena.Literal(type, left)),
                    "scalar min/max absent input");
            }
        }
    }

}
} // namespace NKikimr::NKqp
