#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <library/cpp/testing/unittest/registar.h>
#include <algorithm>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TCatalogTable WriteCatalog(std::string_view name, TSourcePosition) {
    return {std::string(name), {{"id", {ETypeKind::Uint64}}, {"value", {ETypeKind::Int32, true}},
        {"name", {ETypeKind::Utf8, true}}}, TWriteSchema{{"id"}, {}, false}};
}

const TBoundEffect& Effect(const TBoundProgram& program, std::size_t index = 0) {
    const auto& relation = program.Relations.at(program.Results.at(index).Root);
    UNIT_ASSERT(relation.Kind == ERelation::Effect);
    UNIT_ASSERT(relation.Effect);
    UNIT_ASSERT_VALUES_EQUAL(relation.Inputs.size(), 1);
    return *relation.Effect;
}
}

Y_UNIT_TEST_SUITE(KqpNativeWrites) {
    Y_UNIT_TEST(IndependentParsersAgreeOnWriteStructure) {
        for (const auto* sql : {
            "INSERT INTO t (id, value) VALUES (1, 2), (3, NULL) RETURNING id, value;",
            "INSERT OR ABORT INTO t (id) SELECT id FROM s;",
            "INSERT INTO t (id) VALUES (1), RETURNING id;",
            "INSERT OR REVERT INTO t SELECT id FROM s RETURNING *;",
            "UPSERT INTO t (id) VALUES (1); REPLACE INTO t SELECT id FROM s;",
            "UPDATE t SET value = value + 1, name = 'updated'u WHERE id = 1 RETURNING name;",
            "UPDATE t ON (id, value) VALUES (1, 2);",
            "UPDATE t ON SELECT id, value FROM s;",
            "DELETE FROM t WHERE id = 1 RETURNING *;",
            "DELETE FROM t; DELETE FROM t ON (id) VALUES (1), (2);",
            "DELETE FROM t ON SELECT id FROM s;",
            "BATCH UPDATE t SET value = 1 WHERE id = 2;",
            "BATCH DELETE FROM t WHERE value IS NULL;",
        }) {
            try {
                const auto fast = Parse(sql);
                const auto grammar = ParseGrammar(sql);
                UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(fast), SyntaxFingerprint(grammar), sql);
            } catch (const TFrontendError& error) {
                UNIT_FAIL(TStringBuilder() << sql << ": " << error.what());
            }
        }
        for (const auto* sql : {"INSERT INTO t (id) VALUES (1),,", "UPDATE t SET", "DELETE FROM", "INSERT INTO t VALUES ()"}) {
            UNIT_ASSERT_EXCEPTION_C(Parse(sql), TFrontendError, sql);
            UNIT_ASSERT_EXCEPTION_C(ParseGrammar(sql), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(SingleValuesRowIsNotAUnion) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* rows : {"(1ul, 2)", "(1ul, 2), (3ul, NULL)"}) {
                const auto result = CompileNative(std::string("UPSERT INTO t (id, value) VALUES ") + rows, WriteCatalog, settings);
                const auto& program = result.Program->Get();
                const auto& effect = program.Relations[program.Results[0].Root];
                const auto& input = program.Relations[effect.Inputs[0]];
                if (std::string_view(rows) == "(1ul, 2)") UNIT_ASSERT(input.Kind == ERelation::Map);
                else { UNIT_ASSERT(input.Kind == ERelation::UnionAll); UNIT_ASSERT_VALUES_EQUAL(input.Inputs.size(), 2); }
                for (const auto& relation : program.Relations)
                    if (relation.Kind == ERelation::UnionAll) UNIT_ASSERT(relation.Inputs.size() > 1);
                for (const auto& [column, name] : effect.Effect->Columns) {
                    UNIT_ASSERT_VALUES_EQUAL(program.Columns[column].Name, name);
                    UNIT_ASSERT(std::find(input.Columns.begin(), input.Columns.end(), column) != input.Columns.end());
                }
            }
        }
    }

    Y_UNIT_TEST(ValuesAndSelectBindToOrdinaryRelationsAndTypedEffects) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("INSERT INTO t (id) VALUES (1), (2) RETURNING value, id", WriteCatalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& effect = Effect(program);
            UNIT_ASSERT(effect.Kind == EWriteKind::Insert);
            UNIT_ASSERT_VALUES_EQUAL(effect.Columns.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(effect.Returning.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(effect.Returning[0].second, "value");
            UNIT_ASSERT_VALUES_EQUAL(effect.Returning[1].second, "id");
            UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int32, true}));
            std::size_t unions = 0, nulls = 0;
            for (const auto& relation : program.Relations) {
                if (relation.Kind == ERelation::UnionAll) {
                    ++unions;
                    UNIT_ASSERT_VALUES_EQUAL(relation.Inputs.size(), 2);
                    UNIT_ASSERT_VALUES_EQUAL(relation.Columns.size(), 1);
                }
                for (const auto& definition : relation.Definitions) {
                    const auto& value = program.Expressions[definition.Expression];
                    if (value.Op == EOpcode::Literal && std::holds_alternative<std::monostate>(value.Value)) ++nulls;
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(unions, 1);
            UNIT_ASSERT_VALUES_EQUAL(nulls, 2);
            for (const auto* sql : {"UPSERT INTO t SELECT id, value FROM s", "UPDATE t ON SELECT id, value FROM s",
                    "DELETE FROM t ON SELECT id FROM s", "REPLACE INTO t (id) SELECT id FROM s"}) {
                const auto bound = CompileNative(sql, WriteCatalog, settings);
                UNIT_ASSERT(bound.Program->Get().Results[0].Columns.empty());
                UNIT_ASSERT(Effect(bound.Program->Get()).Columns.size() >= 1);
            }
        }
    }

    Y_UNIT_TEST(UpdateExpressionsUseTheOriginalRowAndGlobalsKeepTheirDefinitionScope) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto bound = CompileNative(R"(
                $delta = 1;
                $increment = ($x) -> { RETURN $x + $delta; };
                $delta = 100;
                UPDATE t SET value = $increment(value) WHERE id = 1 RETURNING *;
                DELETE FROM t WHERE value IS NULL;
            )", WriteCatalog, settings);
            const auto& program = bound.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(program.Results.size(), 2);
            UNIT_ASSERT(Effect(program).Kind == EWriteKind::Update);
            UNIT_ASSERT_VALUES_EQUAL(Effect(program).Columns.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(Effect(program).Returning.size(), 3);
            UNIT_ASSERT(Effect(program, 1).Kind == EWriteKind::Delete);
            bool checked = false;
            for (const auto& relation : program.Relations) for (const auto& definition : relation.Definitions) {
                const auto& expression = program.Expressions[definition.Expression];
                if (expression.Op != EOpcode::Add) continue;
                const auto& delta = program.Expressions[expression.Arguments[1]];
                UNIT_ASSERT(delta.Op == EOpcode::Literal);
                UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(delta.Value), 1);
                checked = true;
            }
            UNIT_ASSERT(checked);
        }
    }

    Y_UNIT_TEST(WriteTargetsAndNestedInputsAreDiscoveredBeforeBinding) {
        const auto syntax = ParseNative(R"(
            PRAGMA TablePathPrefix='/Root';
            $q = SELECT id FROM source;
            INSERT INTO target SELECT id FROM $q;
            UPDATE target SET value = (SELECT MAX(value) FROM nested) WHERE id IN (SELECT id FROM ids);
            DELETE FROM target WHERE id = 0;
        )");
        const auto tables = CollectTables(syntax);
        std::vector<std::string> names;
        for (const auto& table : tables) names.push_back(table.Name);
        UNIT_ASSERT(names == (std::vector<std::string>{"/Root/target", "/Root/source", "/Root/ids", "/Root/nested"}));
    }

    Y_UNIT_TEST(WriteContractsRejectInvalidNamesTypesKeysAndDefaults) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                "UPSERT INTO t (id, id) VALUES (1, 2)",
                "UPSERT INTO t (missing) VALUES (1)",
                "UPSERT INTO t (value) VALUES (1)",
                "UPSERT INTO t (id, value) VALUES (1)",
                "UPSERT INTO t (id) VALUES (1, 2)",
                "UPSERT INTO t (id) VALUES (NULL)",
                "UPSERT INTO t (id, value) VALUES (1, 2147483648l)",
                "UPSERT INTO t (id) SELECT id, value FROM s",
                "UPDATE t SET id = 1",
                "UPDATE t SET value = 1, value = 2",
                "DELETE FROM t RETURNING unknown",
                "DELETE FROM t RETURNING id, id",
            }) UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, WriteCatalog, settings), TFrontendError, sql);
            UNIT_ASSERT_EXCEPTION(CompileNative("INSERT INTO t (id) VALUES (1)", [](auto name, auto pos) {
                auto table = WriteCatalog(name, pos); table.Columns[1].Type.OptionalDepth = 0; return table;
            }, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("UPSERT INTO t (id) VALUES (1)", [](auto name, auto pos) {
                auto table = WriteCatalog(name, pos); table.WriteSchema->GeneratedColumns = {{"value", "id", false}}; return table;
            }, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("DELETE FROM t", [](auto name, auto pos) {
                auto table = WriteCatalog(name, pos); table.WriteSchema.reset(); return table;
            }, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(OmittedDefaultsAreEffectMetadataAndExplicitValuesTakePrecedence) {
        const auto catalog = [](auto name, auto pos) {
            auto table = WriteCatalog(name, pos);
            table.WriteSchema->Defaults = {{"id", EColumnDefault::Sequence}, {"value", EColumnDefault::Literal}};
            return table;
        };
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* verb : {"INSERT INTO", "UPSERT INTO", "REPLACE INTO"}) {
                const auto omitted = CompileNative(std::string(verb) + " t (name) VALUES ('row'u) RETURNING id, value", catalog, settings);
                const auto& effect = Effect(omitted.Program->Get());
                UNIT_ASSERT(effect.DefaultColumns == (std::vector<std::string>{"id", "value"}));
                UNIT_ASSERT_VALUES_EQUAL(effect.Columns.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(effect.Columns[0].second, "name");
                UNIT_ASSERT_VALUES_EQUAL(effect.Returning.size(), 2);
                // A supplied NULL is a value; it must not invoke the default.
                const auto supplied = CompileNative(std::string(verb) + " t (id, value) VALUES (7ul, NULL)", catalog, settings);
                UNIT_ASSERT(Effect(supplied.Program->Get()).DefaultColumns.empty());
            }
            for (const auto* sql : {"UPDATE t ON (name) VALUES ('row'u)", "DELETE FROM t ON (name) VALUES ('row'u)"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog, settings), TFrontendError, sql);
            for (const auto* sql : {"UPDATE t SET value = 3 WHERE id = 7ul", "DELETE FROM t WHERE id = 7ul"}) {
                const auto bound = CompileNative(sql, catalog, settings);
                UNIT_ASSERT(Effect(bound.Program->Get()).DefaultColumns.empty());
            }
        }
    }

    Y_UNIT_TEST(WriteCoercionsAcceptOnlyFittingLiteralConversions) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto bound = CompileNative("UPSERT INTO t (id, value, name) VALUES (1, 2147483647l, 'valid')", WriteCatalog, settings);
            const auto& program = bound.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(Effect(program).Columns.size(), 3);
            UNIT_ASSERT(Effect(program).Returning.empty());
            const auto& values = program.Relations.at(program.Relations.at(program.Results[0].Root).Inputs[0]);
            UNIT_ASSERT(values.Kind == ERelation::Map);
            UNIT_ASSERT(program.Columns[values.Columns[0]].Type == (TType{ETypeKind::Uint64}));
            UNIT_ASSERT(program.Columns[values.Columns[1]].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT(program.Columns[values.Columns[2]].Type == (TType{ETypeKind::Utf8, true}));
        }
    }

    Y_UNIT_TEST(GeneratedValuesUseThePreparedRowAndShareCatalogCompilation) {
        const auto catalog = [](auto name, auto pos) {
            auto table = WriteCatalog(name, pos);
            table.Columns[0].Type = {ETypeKind::Int32};
            table.Columns.push_back({"g", {ETypeKind::Int32, true}});
            table.WriteSchema->Defaults = {{"id", EColumnDefault::Sequence}, {"value", EColumnDefault::Literal}};
            table.WriteSchema->GeneratedColumns = {{"g", "id * 10 + COALESCE(value, 0)"}};
            return table;
        };
        const auto bound = CompileNative(R"(
            INSERT INTO t (name) VALUES ('a'u), ('b'u) RETURNING id, value, g;
            REPLACE INTO t (id) VALUES (3) RETURNING g;
            UPSERT INTO t (id, value) VALUES (3, NULL) RETURNING g;
            UPDATE t SET value = 9 WHERE id = 3 RETURNING g;
            UPDATE t SET name = 'changed'u WHERE id = 3;
            DELETE FROM t WHERE id = 3 RETURNING g;
        )", catalog);
        const auto& program = bound.Program->Get();
        UNIT_ASSERT(Effect(program).DefaultColumns == (std::vector<std::string>{"id", "value"}));
        UNIT_ASSERT_VALUES_EQUAL(Effect(program).Columns.size(), 1);
        const auto& generated = Effect(program).GeneratedColumns;
        UNIT_ASSERT_VALUES_EQUAL(generated.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(generated[0].first, "g");
        for (std::size_t i = 1; i < 4; ++i)
            UNIT_ASSERT(Effect(program, i).GeneratedColumns[0].second == generated[0].second);
        UNIT_ASSERT(Effect(program, 4).GeneratedColumns.empty());
        UNIT_ASSERT(Effect(program, 5).GeneratedColumns.empty());
        const auto& value = *generated[0].second;
        TEvaluationContext context;
        context.Column = [&](TColumnId id) -> TValue {
            if (value.Columns[id] == "id") return {{ETypeKind::Int32}, std::int64_t{2}};
            UNIT_ASSERT_VALUES_EQUAL(value.Columns[id], "value");
            return {{ETypeKind::Int32, true}, std::int64_t{7}};
        };
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(value.Expressions, value.Root, context).Data), 27);
        // Partial UPSERT must read existing dependencies, including defaults
        // used only for newly inserted rows. Never compute from an invented NULL.
        UNIT_ASSERT_EXCEPTION(CompileNative("UPSERT INTO t (id) VALUES (3)", catalog), TUnsupportedFeature);
        for (const auto* sql : {"INSERT INTO t (g) VALUES (7)", "REPLACE INTO t (id, g) VALUES (1, NULL)",
                "UPDATE t SET g = 7", "UPDATE t ON (id, g) VALUES (1, 7)", "UPSERT INTO t SELECT id, g FROM t"})
            UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog), TFrontendError, sql);
    }

    Y_UNIT_TEST(GeneratedMetadataRejectsInvalidDependenciesAndDefinitions) {
        for (const auto* expression : {"g + 1", "missing + 1", "$query_parameter", "SUM(value)", "(SELECT id FROM t)",
                "id; SELECT 1", "CAST(RandomUuid(id) AS String)"}) {
            const auto catalog = [&](auto name, auto pos) {
                auto table = WriteCatalog(name, pos);
                table.Columns.push_back({"g", {ETypeKind::Int32, true}});
                table.WriteSchema->GeneratedColumns = {{"g", expression}};
                return table;
            };
            UNIT_ASSERT_EXCEPTION_C(CompileNative("INSERT INTO t (id) VALUES (1)", catalog), TFrontendError, expression);
        }
        const auto unknown = [](auto name, auto pos) {
            auto table = WriteCatalog(name, pos);
            table.WriteSchema->GeneratedColumns = {{"missing", "1"}};
            return table;
        };
        UNIT_ASSERT_EXCEPTION(CompileNative("INSERT INTO t (id) VALUES (1)", unknown), TFrontendError);
    }
}
