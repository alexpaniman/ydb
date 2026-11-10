#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
std::string Fingerprint(TParsedExpression parsed) {
    TStatement binding{EStatementKind::Binding, {}};
    binding.Name = "value";
    binding.Expression = parsed.Root;
    return SyntaxFingerprint({std::move(parsed.Expressions), {std::move(binding)}});
}
}

Y_UNIT_TEST_SUITE(KqpNativeRowExpressions) {
    Y_UNIT_TEST(ScalarEntryPointsUseTheOrdinaryExpressionGrammar) {
        for (const auto* sql : {"id * 10 + COALESCE(payload, 0)", "CASE WHEN id > 0 THEN payload ELSE 7 END",
                "CAST(id AS Int64)", "IF(payload IS NULL, 0, payload)", "'a; SELECT b'", "(1 + 2) * 3"}) {
            UNIT_ASSERT_VALUES_EQUAL_C(Fingerprint(ParseExpression(sql)), Fingerprint(ParseGrammarExpression(sql)), sql);
        }
        for (const auto* sql : {"", "id +", "id; SELECT 1", "id, payload", "id FROM t", "1 AS x", "PRAGMA x"}) {
            UNIT_ASSERT_EXCEPTION_C(ParseExpression(sql), TFrontendError, sql);
            UNIT_ASSERT_EXCEPTION_C(ParseGrammarExpression(sql), TFrontendError, sql);
        }
        TParseSettings settings;
        settings.AnsiQuotes = true;
        UNIT_ASSERT_VALUES_EQUAL(Fingerprint(ParseExpression("\"id\" + 1", settings)),
            Fingerprint(ParseGrammarExpression("\"id\" + 1", settings)));
    }

    Y_UNIT_TEST(ExplicitRowBindingsPreserveTypesNullsAndFolding) {
        const TCatalogColumn columns[]{{"id", {ETypeKind::Int32}}, {"payload", {ETypeKind::Int32, true}}};
        const auto value = CompileRowExpression("id * (2 + 8) + COALESCE(payload, 0)", columns, {ETypeKind::Int32, true});
        UNIT_ASSERT(value.Columns == (std::vector<std::string>{"id", "payload"}));
        UNIT_ASSERT(value.Dependencies == (std::vector<TColumnId>{0, 1}));
        UNIT_ASSERT(value.Expressions[value.Root].Type == (TType{ETypeKind::Int32, true}));
        for (const auto payload : {std::optional<std::int64_t>{7}, std::optional<std::int64_t>{}}) {
            TEvaluationContext context;
            context.Column = [&](TColumnId id) -> TValue {
                UNIT_ASSERT(id < 2);
                return {columns[id].Type, id == 0 ? TScalar{std::int64_t{3}} : payload ? TScalar{*payload} : TScalar{}};
            };
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(value.Expressions, value.Root, context).Data),
                30 + payload.value_or(0));
        }
        const auto constant = CompileRowExpression("(2 + 8) * 3", {}, {ETypeKind::Int32});
        UNIT_ASSERT(constant.Dependencies.empty());
        UNIT_ASSERT(constant.Expressions[constant.Root].Op == EOpcode::Literal);
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(constant.Expressions, constant.Root).Data), 30);
        const auto unicode = CompileRowExpression("'привет'u", {}, {ETypeKind::Utf8});
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(Evaluate(unicode.Expressions, unicode.Root).Data), "привет");
    }

    Y_UNIT_TEST(DependenciesDescribeTheFoldedRoot) {
        const TCatalogColumn columns[]{{"unused", {ETypeKind::Int32}}, {"id", {ETypeKind::Int32}},
            {"payload", {ETypeKind::Int32, true}}};
        for (const auto* sql : {"CASE WHEN TRUE THEN id ELSE payload END", "COALESCE(id, payload)",
                "IF(FALSE, unused, id + id)"}) {
            const auto value = CompileRowExpression(sql, columns, {ETypeKind::Int32, true});
            UNIT_ASSERT_C(value.Dependencies == (std::vector<TColumnId>{1}), sql);
            TEvaluationContext context;
            context.Column = [&](TColumnId id) -> TValue {
                UNIT_ASSERT_VALUES_EQUAL(id, 1);
                return {columns[id].Type, std::int64_t{3}};
            };
            UNIT_ASSERT(!Evaluate(value.Expressions, value.Root, context).IsNull());
        }
    }

    Y_UNIT_TEST(RowExpressionsAreClosedAndBounded) {
        const TCatalogColumn columns[]{{"id", {ETypeKind::Int32}}};
        for (const auto* sql : {"missing + 1", "$parameter", "$f(id)", "SUM(id)", "ROW_NUMBER() OVER ()",
                "(SELECT 1)", "id IN (SELECT id FROM t)", "id; SELECT 1", "'text'"}) {
            UNIT_ASSERT_EXCEPTION_C(CompileRowExpression(sql, columns, {ETypeKind::Int32}), TFrontendError, sql);
        }
        const TCatalogColumn duplicate[]{{"id", {ETypeKind::Int32}}, {"id", {ETypeKind::Int32}}};
        UNIT_ASSERT_EXCEPTION(CompileRowExpression("id", duplicate, {ETypeKind::Int32}), TFrontendError);
        TParseSettings limits;
        limits.MaxQueryBytes = 3;
        UNIT_ASSERT_EXCEPTION(CompileRowExpression("id + 1", columns, {ETypeKind::Int32}, limits), TResourceLimitError);
        limits.MaxQueryBytes = 100;
        limits.MaxDepth = 2;
        UNIT_ASSERT_EXCEPTION(CompileRowExpression("((((id))))", columns, {ETypeKind::Int32}, limits), TResourceLimitError);
    }
}
