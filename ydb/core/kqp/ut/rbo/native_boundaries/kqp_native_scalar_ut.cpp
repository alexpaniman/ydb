#include "kqp_native_scalar_test_helpers.h"

#include <limits>

namespace NKikimr::NKqp {
using NTests::CheckScalar;
Y_UNIT_TEST_SUITE(KqpNativeScalarBoundaries) {
    Y_UNIT_TEST(BitwiseAndNullSafePredicatesEmitTerminalOperations) {
        using namespace NFrontend;
        for (const auto* sql : {"SELECT ~1u", "SELECT 7u & 3u | 1u", "SELECT 1u << 2u",
                "SELECT -1 >> 1", "SELECT 1u ^ 3ul", "SELECT ~NULL", "SELECT NULL & 1u",
                "SELECT NULL IS DISTINCT FROM 1", "SELECT NULL IS NOT DISTINCT FROM NULL",
                "SELECT Just(1) IS NOT DISTINCT FROM 1", "SELECT 1 IS DISTINCT FROM 2ul",
                "SELECT CAST('nan' AS Double) IS NOT DISTINCT FROM CAST('nan' AS Double)",
                "SELECT CAST('nan' AS Float) IS NOT DISTINCT FROM CAST('nan' AS Double)"}) {
            for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
                TCompilerSettings settings; settings.Parser = parser;
                const auto compiled = CompileNative(sql, {}, settings);
                const auto& program = compiled.Program->Get();
                CheckScalar(program.Expressions, NTests::Projection(program), sql);
            }
        }
        for (const auto kind : {ETypeKind::Int8, ETypeKind::Int16, ETypeKind::Int32, ETypeKind::Int64,
                ETypeKind::Uint8, ETypeKind::Uint16, ETypeKind::Uint32, ETypeKind::Uint64})
        for (const bool optional : {false, true}) {
            TExpressionArena arena;
            const TType type{kind, optional};
            for (const auto input : {0, 1, 127}) {
                const auto value = arena.Literal(type, type.IsSigned() ? TScalar{std::int64_t{-input}} : TScalar{std::uint64_t(input)});
                for (const auto count : {0u, 1u, 7u, 8u, 16u, 31u, 32u, 63u, 64u, 255u})
                for (const auto op : {EOpcode::ShiftLeft, EOpcode::ShiftRight})
                    CheckScalar(arena, arena.Binary(op, value, arena.Literal({ETypeKind::Uint8}, std::uint64_t{count})), "shift width/signedness");
                if (!type.IsSigned()) {
                    CheckScalar(arena, arena.Unary(EOpcode::BitNot, value), "bitwise complement");
                    for (const auto op : {EOpcode::BitAnd, EOpcode::BitOr, EOpcode::BitXor})
                        CheckScalar(arena, arena.Binary(op, value, arena.Literal({ETypeKind::Uint64}, std::uint64_t{255})), "bitwise width promotion");
                }
            }
            if (optional) CheckScalar(arena, arena.Binary(EOpcode::ShiftRight, arena.Literal(type, {}),
                arena.Literal({ETypeKind::Uint8}, std::uint64_t{1})), "absent shift input");
        }
        for (const auto kind : {ETypeKind::Float, ETypeKind::Double, ETypeKind::Decimal})
        for (const bool optional : {false, true}) {
            TExpressionArena arena;
            const TType type = kind == ETypeKind::Decimal ? TType{kind, optional, 12, 2} : TType{kind, optional};
            auto values = kind == ETypeKind::Decimal
                ? std::vector<TScalar>{TDecimal{0}, TDecimal{200}, TDecimal{TDecimal::Inf()}, TDecimal{TDecimal::NaN()}}
                : std::vector<TScalar>{0.0, -0.0, 2.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
            if (optional) values.emplace_back();
            for (const auto& a : values) for (const auto& b : values) {
                const auto root = arena.Binary(EOpcode::IsNotDistinct, arena.Literal(type, a), arena.Literal(type, b));
                CheckScalar(arena, root, "null-safe equality unordered/absent input");
                CheckScalar(arena, arena.Unary(EOpcode::Not, root), "null-safe inequality");
            }
        }
    }

    Y_UNIT_TEST(BetweenModesExecuteWithoutSemanticLowering) {
        for (const auto parser : {NFrontend::EParser::Handwritten, NFrontend::EParser::Grammar})
        for (const auto* operation : {" BETWEEN ", " NOT BETWEEN ", " BETWEEN SYMMETRIC ", " NOT BETWEEN SYMMETRIC "})
        for (const auto* value : {"0.0", "2.0", "4.0", "NULL", "CAST('nan' AS Double)"})
        for (const auto* bounds : {"1.0 AND 3.0", "3.0 AND 1.0", "NULL AND 3.0", "1.0 AND NULL"}) {
            const auto sql = std::string("SELECT ") + value + operation + bounds;
            NFrontend::TCompilerSettings settings; settings.Parser = parser;
            auto compiled = NFrontend::CompileNative(sql, {}, settings);
            const auto& program = compiled.Program->Get();
            CheckScalar(program.Expressions, NTests::Projection(program), sql);
        }
        CheckScalar("SELECT 2 IN COMPACT (1, NULL)");
    }

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
