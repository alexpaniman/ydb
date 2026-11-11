#include <ydb/core/kqp/opt/rbo/frontend/bindings.h>
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TExprId Projection(const TBoundProgram& program, std::size_t index) {
    const auto column = program.Results.at(0).Columns.at(index).Column;
    for (const auto& relation : program.Relations)
        for (const auto& definition : relation.Definitions) if (definition.Column == column) return definition.Expression;
    throw std::logic_error("missing scalar projection");
}

std::shared_ptr<TExternalBindings> Bindings() {
    auto arena = std::make_shared<TExpressionArena>();
    const auto argument = arena->Column(17, {ETypeKind::Int64});
    const TColumnId parameters[]{17};
    auto bindings = std::make_shared<TExternalBindings>();
    bindings->Values.emplace("p", TScalarBinding(arena, arena->Literal({ETypeKind::Int64}, std::int64_t{7})));
    bindings->Callables.emplace("f", TScalarBinding(arena, argument, parameters));
    bindings->Callables.emplace("Demo::Identity", TScalarBinding(arena, argument, parameters));
    bindings->Callables.emplace("Demo::Increment", TScalarBinding(arena,
        arena->Binary(EOpcode::Add, argument, arena->Literal({ETypeKind::Int64}, std::int64_t{1})), parameters));
    return bindings;
}

TBoundProgram Compile(std::string_view sql, EParser parser, std::shared_ptr<const TExternalBindings> bindings) {
    TCompilerSettings settings;
    settings.Parser = parser;
    settings.Binding.ExternalBindings = std::move(bindings);
    auto compiled = CompileNative(sql, {}, settings);
    UNIT_ASSERT(compiled.Program);
    return std::move(*compiled.Program).Release();
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeExternalBindings) {
    // PR58587 ParameterAndResolvedCallableBindings: the caller supplies a
    // value and a lambda through both variable and qualified callable bindings.
    Y_UNIT_TEST(ParameterAndResolvedCallableBindings) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            const auto program = Compile("SELECT $f($p), Demo::Identity($p);", parser, Bindings());
            UNIT_ASSERT(program.Parameters.empty());
            for (std::size_t i = 0; i < 2; ++i) {
                UNIT_ASSERT(program.Results[0].Columns[i].Type == TType{ETypeKind::Int64});
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(program.Expressions, Projection(program, i))), "7");
            }
        }
    }

    Y_UNIT_TEST(LexicalShadowingAndHigherOrderAliases) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            const auto program = Compile(R"(
                $capture = ($x) -> { RETURN $f($p) + $x; };
                $alias = $f;
                $p = 40l;
                $f = ($x) -> { RETURN $x * 2; };
                $apply = ($function, $argument) -> { RETURN $function($argument); };
                $qualified = Udf(Demo::Identity);
                SELECT $capture(3l), $f($p), $apply($alias, $p), $qualified($p);
            )", parser, Bindings());
            const char* expected[]{"10", "80", "40", "40"};
            for (std::size_t i = 0; i < 4; ++i)
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(program.Expressions, Projection(program, i))), expected[i]);
        }
    }

    Y_UNIT_TEST(ActualArgumentTypesAndRuntimeParameters) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            const auto program = Compile("DECLARE $x AS Int64?; SELECT $f('text'), Demo::Increment($x);", parser, Bindings());
            UNIT_ASSERT(program.Results[0].Columns[0].Type == TType{ETypeKind::String});
            UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int64, true}));
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(program.Expressions, Projection(program, 0))), "text");
            for (const bool present : {false, true}) {
                TEvaluationContext context;
                context.Parameter = [&](std::string_view name) -> TValue {
                    UNIT_ASSERT_VALUES_EQUAL(name, "x");
                    return {{ETypeKind::Int64, true}, present ? TScalar{std::int64_t{8}} : TScalar{}};
                };
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(program.Expressions, Projection(program, 1), context)), present ? "9" : "NULL");
            }
        }
    }

    Y_UNIT_TEST(DiscoveryOwnsBindingsAndResultsOwnImportedValues) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TBoundProgram program;
            {
                TCompilerSettings settings;
                settings.Parser = parser;
                settings.Binding.ExternalBindings = Bindings();
                auto discovered = DiscoverNative(ParseNative("SELECT $f($p);", settings), settings.Binding);
                settings.Binding.ExternalBindings.reset();
                UNIT_ASSERT(discovered.Tables().empty());
                program = BindNative(discovered, {}).Release();
            }
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(program.Expressions, Projection(program, 0))), "7");
        }
    }

    Y_UNIT_TEST(UnusedBindingsStayOutsideTheQueryArena) {
        auto bindings = Bindings();
        auto arena = std::make_shared<TExpressionArena>();
        auto body = arena->Literal({ETypeKind::Int64}, std::int64_t{1});
        for (unsigned i = 0; i < 1000; ++i) body = arena->Binary(EOpcode::Add, body, body);
        bindings->Values.emplace("unused", TScalarBinding(arena, body));
        const auto ordinary = Compile("SELECT 1;", EParser::Fast, {});
        const auto supplied = Compile("SELECT 1;", EParser::Fast, bindings);
        UNIT_ASSERT_VALUES_EQUAL(ordinary.Expressions.Size(), supplied.Expressions.Size());
    }

    Y_UNIT_TEST(CollectionShapesAreImportedIntoTheQueryArena) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            auto bindings = Bindings();
            auto arena = std::make_shared<TExpressionArena>();
            const TExprId items[]{arena->Literal({ETypeKind::String}, std::string("first")),
                arena->Literal({ETypeKind::String}, std::string("second"))};
            const auto list = arena->Collection(items, true);
            bindings->Values.emplace("items", TScalarBinding(arena, list));
            const auto program = Compile("SELECT [1,2][0u], $items[0u], Demo::Identity($items)[1u];", parser, bindings);
            const char* expected[]{"1", "first", "second"};
            for (std::size_t i = 0; i < 3; ++i)
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(program.Expressions, Projection(program, i))), expected[i]);
        }
    }

    Y_UNIT_TEST(InvalidDefinitionsAndInvocationsAreErrors) {
        auto arena = std::make_shared<TExpressionArena>();
        const auto column = arena->Column(5, {ETypeKind::Int64});
        const auto parameter = arena->Parameter("free", {ETypeKind::Int64});
        UNIT_ASSERT_EXCEPTION((TScalarBinding({}, 0)), TFrontendError);
        UNIT_ASSERT_EXCEPTION((TScalarBinding(arena, 100)), TFrontendError);
        UNIT_ASSERT_EXCEPTION((TScalarBinding(arena, column)), TFrontendError);
        UNIT_ASSERT_EXCEPTION((TScalarBinding(arena, parameter)), TFrontendError);
        const TColumnId duplicate[]{5, 5};
        UNIT_ASSERT_EXCEPTION((TScalarBinding(arena, column, duplicate)), TFrontendError);
        const auto constant = arena->Literal({ETypeKind::Int64}, std::int64_t{1});
        for (const auto name : {"", "$bad", "$Module::Bad"}) {
            auto bindings = Bindings();
            bindings->Callables.emplace(name, TScalarBinding(arena, constant));
            UNIT_ASSERT_EXCEPTION(Compile("SELECT 1;", EParser::Fast, bindings), TFrontendError);
        }
        auto conflict = Bindings();
        conflict->Callables.emplace("p", TScalarBinding(arena, constant));
        UNIT_ASSERT_EXCEPTION(Compile("SELECT 1;", EParser::Fast, conflict), TFrontendError);
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            for (const auto sql : {"SELECT\n  $f();", "SELECT\n  Demo::Identity(1, 2);", "SELECT\n  Demo::Increment('wrong');"}) {
                try {
                    Compile(sql, parser, Bindings());
                    UNIT_FAIL("expected invalid external call to fail");
                } catch (const TFrontendError& error) {
                    UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 2);
                }
            }
        }
    }
}
