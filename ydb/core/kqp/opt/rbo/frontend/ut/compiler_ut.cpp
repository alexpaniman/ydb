#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/parser.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/resource/resource.h>
#include <algorithm>
#include <type_traits>

using namespace NKikimr::NKqp::NFrontend;

static_assert(!std::is_constructible_v<TCompiledProgram, TBoundProgram>);
static_assert(!std::is_default_constructible_v<TCompiledProgram>);
static_assert(std::is_same_v<decltype(std::declval<TCompiledProgram>().Get()), const TBoundProgram&>);

namespace {
struct TKeyword { std::string_view Word; unsigned Identifiers; };
#include <ydb/core/kqp/opt/rbo/frontend/grammar_keywords.inc>

TCatalogTable Catalog(std::string_view name, TSourcePosition) {
    return {std::string(name), {{"id", {ETypeKind::Uint64}}, {"value", {ETypeKind::Int32, true}}}};
}

void AssertParserAgreement(std::string_view sql, bool ansi = false) {
    TCompilerSettings fast;
    fast.Parsing.AnsiQuotes = ansi;
    std::optional<TParsedQuery> parsed;
    try { parsed = ParseNative(sql, fast); } catch (const TFrontendError&) {}
    std::optional<TParsedQuery> grammar;
    try { grammar = ParseGrammar(sql, fast.Parsing); } catch (const TFrontendError&) {}
    UNIT_ASSERT_VALUES_EQUAL_C(bool(parsed), bool(grammar), sql);
    if (parsed) UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(*parsed), SyntaxFingerprint(*grammar), sql);
}
}

Y_UNIT_TEST_SUITE(KqpNativeCompiler) {
    Y_UNIT_TEST(ValueByAggregatesLowerToExistingMinMaxStates) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                "SELECT MIN_BY(value, id) AS lo, MAX_BY(value, id) AS hi FROM a",
                "SELECT id, MIN_BY(id, value) AS lo, MAX_BY(id, value) AS hi FROM a GROUP BY id",
                "SELECT MIN_BY(value, NULL) AS lo FROM a",
                "SELECT MIN_BY(NULL, id) AS lo FROM a"}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                bool found = false;
                for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Aggregate) {
                    found = true;
                    for (const auto& aggregate : relation.Aggregates) {
                        UNIT_ASSERT(aggregate.Function == "min" || aggregate.Function == "max");
                        UNIT_ASSERT(program.Columns[aggregate.Input].Type.Kind == ETypeKind::Tuple);
                    }
                }
                UNIT_ASSERT_C(found, sql);
                for (const auto& column : program.Results.front().Columns) UNIT_ASSERT(!column.Type.IsCollection());
            }
        }
    }

    Y_UNIT_TEST(DecimalListMembershipPreservesTheExactKeyType) {
        const auto compiled = CompileNative(R"(
            DECLARE $items AS List<Decimal(1,0)?>;
            DECLARE $key AS Decimal(1,0)?;
            SELECT $key IN $items AS found;
        )", Catalog);
        UNIT_ASSERT(compiled.Program->Get().Results.front().Columns.front().Type == (TType{ETypeKind::Bool, true}));
    }


    Y_UNIT_TEST(LikeEscapeHasOneNativeSyntaxAndBindingPath) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                "DECLARE $p AS String; SELECT 'a_b' LIKE $p ESCAPE '#' AS yes",
                "SELECT 'a_b' LIKE 'a#_b' ESCAPE '#' AS yes",
                "SELECT 'a%b' NOT LIKE 'a#%c' ESCAPE '#' AS yes",
                "SELECT 'a#b' LIKE 'a##b' ESCAPE '#' AS yes",
                "SELECT 'a.b' LIKE 'a_b' AS yes",
                "SELECT 'a.b' LIKE 'a._' ESCAPE '.' AS yes"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.Program, sql);
                UNIT_ASSERT(result.Program->Get().Results.front().Columns.front().Type == TType{ETypeKind::Bool});
            }
            for (const auto* sql : {
                "SELECT 'x' LIKE 'x' ESCAPE ''", "SELECT 'x' LIKE 'x' ESCAPE 'ab'",
                "SELECT 'x' LIKE 'x' ESCAPE '%'", "SELECT 'x' LIKE 'x' ESCAPE '_'",
                "SELECT 'x' LIKE 'x' ESCAPE 'é'", "SELECT 'x' LIKE 'x' ESCAPE Utf8('#')",
                "SELECT 'x' LIKE 'x#' ESCAPE '#'",
                "DECLARE $e AS String; SELECT 'x' LIKE 'x' ESCAPE $e"})
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TFrontendError);
        }
    }


    Y_UNIT_TEST(StandardWindowAliasesAndUuidCrossTheCheckedBoundary) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                SELECT CAST(RandomUuid(id) AS Utf8) AS uuid,
                    ROW_NUMBER() OVER () AS number, DENSE_RANK() OVER (ORDER BY id) AS rank FROM t;
            )", Catalog, settings);
            const auto& result = compiled.Program->Get().Results.front();
            UNIT_ASSERT(result.Columns[0].Type == TType{ETypeKind::Utf8});
            UNIT_ASSERT(result.Columns[1].Type == TType{ETypeKind::Uint64});
            UNIT_ASSERT(result.Columns[2].Type == TType{ETypeKind::Uint64});
        }
    }

    Y_UNIT_TEST(TemporalListMembershipBindsWithNativeKeyTypes) {
        const auto compiled = CompileNative(R"(
            DECLARE $values AS List<Timestamp64>;
            DECLARE $key AS Timestamp64?;
            SELECT $key IN $values AS found;
        )", Catalog);
        UNIT_ASSERT(compiled.Program->Get().Results.front().Columns.front().Type == (TType{ETypeKind::Bool, true}));
    }

    Y_UNIT_TEST(ExtendedTemporalCatalogAndParametersUseTheCheckedCompiler) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                DECLARE $boundary AS Timestamp64;
                SELECT CAST(ts AS Timestamp64) AS ts, CAST(ts AS Datetime64) AS seconds,
                    CAST(ts AS Int64) AS ticks FROM t WHERE ts <= $boundary;
            )", [](std::string_view name, TSourcePosition) {
                return TCatalogTable{std::string(name), {{"ts", {ETypeKind::Timestamp64}}}};
            }, settings);
            const auto& columns = compiled.Program->Get().Results.at(0).Columns;
            UNIT_ASSERT(columns[0].Type == TType{ETypeKind::Timestamp64});
            UNIT_ASSERT(columns[1].Type == TType{ETypeKind::Datetime64});
            UNIT_ASSERT(columns[2].Type == TType{ETypeKind::Int64});
        }
    }

    Y_UNIT_TEST(QuotedStructTagsBindThroughBothParsers) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                DECLARE $items AS List<Struct<'select':Int64, "mixed name":String?, `value`:Int32>>;
                SELECT `select`, `mixed name`, value FROM AS_TABLE($items);
            )", Catalog, settings);
            const auto& result = compiled.Program->Get().Results.at(0).Columns;
            UNIT_ASSERT_VALUES_EQUAL(result.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(result[0].Name, "select");
            UNIT_ASSERT_VALUES_EQUAL(result[1].Name, "mixed name");
            UNIT_ASSERT(result[0].Type == TType{ETypeKind::Int64});
            UNIT_ASSERT(result[1].Type == (TType{ETypeKind::String, true}));
        }
    }

    // Borrow the pragma spellings and ordered overrides from PR 58587's
    // KqpNewYqlOptimizerPragma. The syntax is decoded once by the parser.
    Y_UNIT_TEST(PlannerPragmaFormsShareOneParsedRepresentation) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "PRAGMA ydb.CostBasedOptimizationLevel = '0'; SELECT 1",
                    "PRAGMA KiKiMr.Cost_Based_Optimization_Level('0'); SELECT 1",
                    "PRAGMA ydb.CostBasedOptimizationLevel(+0); SELECT 1",
                    "--!ansi_lexer\nPRAGMA \"ydb\".\"CostBasedOptimizationLevel\" = '0'; SELECT 1"}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& pragmas = compiled.Program->Get().PlannerSettings;
                UNIT_ASSERT_VALUES_EQUAL(pragmas.size(), 1);
                UNIT_ASSERT(pragmas[0].Kind == EPlannerSetting::CostBasedOptimizationLevel);
                UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint32_t>(pragmas[0].Value), 0);
            }
            for (const auto* value : {"'invalid'", "'-1'", "'4294967296'", "default", "''"}) {
                const auto sql = std::string("PRAGMA ydb.CostBasedOptimizationLevel(") + value + "); SELECT 1";
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TFrontendError);
            }
            const auto compiled = CompileNative(
                "PRAGMA ydb.CostBasedOptimizationLevel('4'); PRAGMA ydb.CostBasedOptimizationLevel('0'); SELECT 1",
                Catalog, settings);
            const auto& pragmas = compiled.Program->Get().PlannerSettings;
            UNIT_ASSERT_VALUES_EQUAL(pragmas.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint32_t>(pragmas[0].Value), 4);
            UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint32_t>(pragmas[1].Value), 0);
        }
    }

    Y_UNIT_TEST(ScalarListsBindWithoutRelationalizingMembership) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("DECLARE $p AS List<Int32?>; SELECT id IN $p AS found FROM t", Catalog, settings);
            const auto& program = compiled.Program->Get();
            UNIT_ASSERT(program.Parameters.at("p").ScalarList()->Item == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Bool, true}));
            UNIT_ASSERT(std::any_of(program.Expressions.Nodes().begin(), program.Expressions.Nodes().end(),
                [](const auto& node) { return node.Op == EOpcode::InCollection; }));
            UNIT_ASSERT(std::none_of(program.Relations.begin(), program.Relations.end(),
                [](const auto& relation) { return relation.Kind == ERelation::Join; }));
            settings.Binding.ParameterTypes.emplace("p", TParameterType::TList{{ETypeKind::Int32, true}});
            UNIT_ASSERT(!CompileNative("SELECT id NOT IN $p FROM t", Catalog, settings).UsesLegacy());
            UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $p AS List<Int64>; SELECT id IN $p FROM t", Catalog, settings), TFrontendError);
        }
        for (const auto* sql : {
            "SELECT Yql::SqlIn((1, NULL), 2, AsTuple(AsTuple(AsAtom('ansi'))))",
            "SELECT Yql::SqlIn([1, 2], 2, AsTuple())",
            "SELECT Yql::SqlIn(AsList(), NULL, AsTuple())"}) {
            const auto compiled = CompileNative(sql, Catalog);
            UNIT_ASSERT(!compiled.UsesLegacy());
        }
    }

    Y_UNIT_TEST(RowParametersBindAsRelationsWithScalarColumns) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* declaration : {
                    "List<Struct<value:String?,id:Int32>>",
                    "List<Struct<value:Optional<String>,id:Int32,>>",
                    "List<Struct<id:Int32,value:Optional<String>>>"}) {
                const auto sql = std::string("DECLARE $p AS ") + declaration +
                    "; SELECT p.id,p.value FROM AS_TABLE($p) AS p WHERE p.id > 0 ORDER BY p.id";
                const auto syntax = ParseNative(sql, settings);
                UNIT_ASSERT(CollectTables(syntax).empty());
                const auto compiled = BindNative(syntax, [](auto, auto) -> TCatalogTable {
                    throw std::logic_error("row parameter requested catalog metadata");
                });
                const auto& program = compiled.Get();
                const auto& fields = *program.Parameters.at("p").Rows();
                UNIT_ASSERT_VALUES_EQUAL(fields.size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(fields[0].Name, "id");
                UNIT_ASSERT(fields[1].Type == (TType{ETypeKind::String, true}));
                UNIT_ASSERT(program.Relations.front().Kind == ERelation::Parameter);
                UNIT_ASSERT_VALUES_EQUAL(program.Relations.front().Table, "p");
                UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int32}));
                UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::String, true}));
                for (const auto& node : program.Expressions.Nodes()) UNIT_ASSERT(node.Op != EOpcode::Parameter);
            }
            for (const auto* sql : {
                    "DECLARE $p AS List<Struct<>>; SELECT COUNT(*) FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct< > >; SELECT 1 FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<id:Int32,value:Optional<String>>>; SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<`a.b`:Int32>>; SELECT p.`a.b` FROM AS_TABLE($p) p",
                    "DECLARE $p AS List<Struct<id:Int32>>; DECLARE $x AS Int32; SELECT id + $x FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<id:Int32>>; $q = SELECT id FROM AS_TABLE($p); $p = 42; SELECT a.id FROM $q a JOIN $q b ON a.id=b.id"})
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            const auto syntax = ParseNative("DECLARE $p AS List<Struct<id:Uint64>>; SELECT a.id FROM AS_TABLE($p) a JOIN t b ON a.id=b.id", settings);
            const auto tables = CollectTables(syntax);
            UNIT_ASSERT_VALUES_EQUAL(tables.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(tables[0].Name, "t");
        }
    }

    Y_UNIT_TEST(SuppliedRowParameterSchemasMatchDeclarationsByFieldName) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            settings.Binding.ParameterTypes.emplace("p", TParameterType::TRows{{"z", {ETypeKind::String, true}}, {"a", {ETypeKind::Int32}}});
            for (const auto* sql : {"SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<a:Int32,z:String?>>; SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<z:String?,a:Int32>>; SELECT * FROM AS_TABLE($p)"}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& columns = compiled.Program->Get().Results[0].Columns;
                UNIT_ASSERT_VALUES_EQUAL(columns.size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(columns[0].Name, "a");
                UNIT_ASSERT_VALUES_EQUAL(columns[1].Name, "z");
            }
            UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $p AS List<Struct<a:Int64,z:String?>>; SELECT * FROM AS_TABLE($p)", Catalog, settings), TFrontendError);
            settings.Binding.ParameterTypes["p"] = TParameterType::TRows{{"a", {ETypeKind::Int32}}, {"a", {ETypeKind::Int32}}};
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT * FROM AS_TABLE($p)", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(RowParameterErrorsAreDistinctFromUnsupportedContainers) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    "SELECT * FROM AS_TABLE($unknown)",
                    "DECLARE $p AS List<Struct<a:Optional<Int32>>>>; SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS Int32; SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<a:Int32,a:Int32>>; SELECT * FROM AS_TABLE($p)"}) {
                bool failed = false;
                try { CompileNative(sql, Catalog, settings); }
                catch (const TUnsupportedFeature&) { UNIT_FAIL(sql); }
                catch (const TFrontendError&) { failed = true; }
                UNIT_ASSERT_C(failed, sql);
            }
            for (const auto* sql : {
                    "DECLARE $p AS List<Int32>; SELECT $p",
                    "DECLARE $p AS List<Struct<a:List<Int32>>>; SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<a:Int32>>?; SELECT * FROM AS_TABLE($p)",
                    "DECLARE $p AS List<Struct<a:Int32>>; SELECT $p",
                    "DECLARE $p AS List<Struct<a:Int32>>; $p=1; SELECT * FROM AS_TABLE($p)",
                    "SELECT * FROM AS_TABLE(AsList(AsStruct(1 AS a)))"})
                UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            settings.Binding.MaxNodes = 1;
            settings.Binding.ParameterTypes.emplace("p", TParameterType::TRows{{"a", {ETypeKind::Int32}}, {"b", {ETypeKind::String}}});
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT * FROM AS_TABLE($p)", Catalog, settings), TResourceLimitError);
        }
    }

    Y_UNIT_TEST(JsonValuesHaveAnExplicitNativeExecutionProfile) {
        const auto catalog = [](std::string_view name, TSourcePosition) {
            return TCatalogTable{std::string(name), {{"j", {ETypeKind::Json, true}}, {"d", {ETypeKind::JsonDocument}}}};
        };
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("SELECT JSON_VALUE(j, '$.a'), JSON_VALUE(d, 'strict $.a[0]') FROM t", catalog, settings);
            UNIT_ASSERT(compiled.Program);
            size_t calls = 0;
            for (const auto& node : compiled.Program->Get().Expressions.Nodes()) if (node.Op == EOpcode::JsonValue) {
                ++calls;
                UNIT_ASSERT(node.Type == (TType{ETypeKind::Utf8, true}));
                UNIT_ASSERT_VALUES_EQUAL(node.Arguments.size(), 1);
            }
            UNIT_ASSERT_VALUES_EQUAL(calls, 2);
            for (const auto* sql : {"SELECT JSON_VALUE(j, '$.a' RETURNING Utf8) FROM t",
                    "SELECT JSON_VALUE(j, '$.a' NULL ON EMPTY) FROM t", "SELECT JSON_VALUE(j, '$x' PASSING 1 AS x) FROM t",
                    "SELECT JSON_VALUE(j, '$ ? (@.a > 0)') FROM t", "SELECT JSON_VALUE(j, '$.a + 1') FROM t",
                    "SELECT JSON_VALUE(j, '$[last]') FROM t", "SELECT JSON_VALUE(j, '$[0, 1]') FROM t",
                    "SELECT j = j FROM t", "SELECT j FROM t ORDER BY j", "SELECT COUNT(DISTINCT j) FROM t",
                    "SELECT MIN(j) FROM t", "SELECT MIN(j) OVER() FROM t", "SELECT Json('{}')",
                    "SELECT `JSON_VALUE`(j, '$.a') FROM t"}) {
                settings.Mode = ECompilerMode::Auto;
                const auto result = CompileNative(sql, catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported, sql);
            }
            for (const auto* sql : {"SELECT JSON_VALUE(j, '$.') FROM t", "SELECT JSON_VALUE(j, '$[') FROM t",
                    "SELECT JSON_VALUE(j, '$[]') FROM t", "SELECT JSON_VALUE('text', '$.a')", "SELECT JSON_VALUE(NULL, '$')"}) {
                settings.Mode = ECompilerMode::Auto;
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog, settings), TFrontendError, sql);
            }
        }
        for (const auto* sql : {"SELECT JSON_VALUE(j, '$.a') FROM t", "SELECT JSON_VALUE(j, '$.*[0].x') FROM t",
                "SELECT JSON_VALUE(j, 'lax $.a') FROM t"}) AssertParserAgreement(sql);
        for (const auto* sql : {"$p = '$.a'; SELECT JSON_VALUE(j, $p) FROM t",
                "SELECT JSON_VALUE(j, ('$.a')) FROM t", "SELECT JSON_VALUE(ALL j, '$.a') FROM t",
                "SELECT JSON_VALUE(DISTINCT j, '$.a') FROM t", "SELECT JSON_VALUE(j, '$.a',) FROM t",
                "SELECT JSON_VALUE(j, '$' || '.a') FROM t", "SELECT JSON_VALUE(j, NULL) FROM t",
                "SELECT JSON_VALUE(j, 1) FROM t", "SELECT JSON_VALUE(j) FROM t"}) {
            AssertParserAgreement(sql);
            TCompilerSettings settings; settings.Mode = ECompilerMode::Auto;
            bool rejected = false;
            try { CompileNative(sql, catalog, settings); }
            catch (const TUnsupportedFeature&) { UNIT_FAIL("invalid JSON_VALUE syntax requested fallback"); }
            catch (const TFrontendError&) { rejected = true; }
            UNIT_ASSERT_C(rejected, sql);
        }
    }

    Y_UNIT_TEST(PlannerPragmasKeepTypedValuesAndSourceOrder) {
        using S = EPlannerSetting;
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                PRAGMA YDB.Opt_Disable_Auto_Index_Selection = 'OFF';
                PRAGMA kikimr.OptDisableAutoIndexSelection;
                PRAGMA ydb.CostBasedOptimizationLevel = '+4';
                PRAGMA ydb.HashJoinMode = 'grace';
                PRAGMA ydb.OptimizerHints = 'Rows(t # 123) JoinType(t u Shuffle)';
                SELECT 1;
                PRAGMA ydb.OptDisableAutoIndexSelection = 'no';
                PRAGMA ydb.OptimizerHints;
                PRAGMA ydb.CostBasedOptimization = 'AuTo';
                PRAGMA ydb.OptFallbackToLegacyOptimizer = 'false';
                SELECT 2;
            )", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& options = program.PlannerSettings;
            UNIT_ASSERT_VALUES_EQUAL(program.Results.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(options.size(), 9);
            UNIT_ASSERT(options[0].Kind == S::OptDisableAutoIndexSelection && !std::get<bool>(options[0].Value));
            UNIT_ASSERT(options[1].Kind == S::OptDisableAutoIndexSelection && std::get<bool>(options[1].Value));
            UNIT_ASSERT(options[2].Kind == S::CostBasedOptimizationLevel && std::get<std::uint32_t>(options[2].Value) == 4);
            UNIT_ASSERT(options[3].Kind == S::HashJoinMode && std::get<std::string>(options[3].Value) == "grace");
            UNIT_ASSERT(options[4].Kind == S::OptimizerHints && std::get<std::string>(options[4].Value) == "Rows(t # 123) JoinType(t u Shuffle)");
            UNIT_ASSERT(options[5].Kind == S::OptDisableAutoIndexSelection && !std::get<bool>(options[5].Value));
            UNIT_ASSERT(options[6].Kind == S::OptimizerHints && std::get<std::string>(options[6].Value).empty());
            UNIT_ASSERT(options[7].Kind == S::CostBasedOptimization && std::get<std::string>(options[7].Value) == "auto");
            UNIT_ASSERT(options[8].Kind == S::OptFallbackToLegacyOptimizer && !std::get<bool>(options[8].Value));
            for (size_t i = 1; i < options.size(); ++i) UNIT_ASSERT(options[i - 1].Position.Line < options[i].Position.Line);
            for (const auto* enabled : {"", "true", "TRUE", "t", "yes", "y", "on", "1", "da"}) {
                const auto sql = std::string("PRAGMA ydb.WindowFunctionsV2='") + enabled + "'; SELECT 1";
                UNIT_ASSERT(std::get<bool>(CompileNative(sql, Catalog, settings).Program->Get().PlannerSettings[0].Value));
            }
            for (const auto* disabled : {"false", "FALSE", "f", "no", "n", "off", "0", "net"}) {
                const auto sql = std::string("PRAGMA ydb.WindowFunctionsV2='") + disabled + "'; SELECT 1";
                UNIT_ASSERT(!std::get<bool>(CompileNative(sql, Catalog, settings).Program->Get().PlannerSettings[0].Value));
            }
        }
    }

    Y_UNIT_TEST(PlannerPragmasRejectInvalidValuesAndExplicitFallback) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* pragma : {"ydb.WindowFunctionsV2='maybe'", "ydb.CostBasedOptimizationLevel='-1'",
                    "ydb.CostBasedOptimizationLevel='4294967296'", "ydb.DqChannelVersion='1.0'", "ydb.DqChannelVersion=''",
                    "ydb.CostBasedOptimization='sometimes'",
                    "ydb.Opt__DisableAutoIndexSelection='true'", "ydb.O_p_t_Disable_Auto_IndexSelection='true'"}) {
                const auto sql = std::string("PRAGMA ") + pragma + "; SELECT 1";
                bool failed = false;
                try { CompileNative(sql, Catalog, settings); }
                catch (const TUnsupportedFeature& error) { UNIT_FAIL(std::string("invalid value incorrectly requested fallback: ") + error.what()); }
                catch (const TFrontendError&) { failed = true; }
                UNIT_ASSERT_C(failed, sql);
            }
            for (const auto* pragma : {"ydb.OptFallbackToLegacyOptimizer='true'", "ydb.OptFallbackToLegacyOptimizer",
                    "ydb.FuturePlannerOption='true'", "other.WindowFunctionsV2='true'"}) {
                const auto sql = std::string("PRAGMA ") + pragma + "; SELECT 1";
                UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
                auto required = settings; required.Mode = ECompilerMode::NativeRequired;
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, required), TUnsupportedFeature, sql);
            }
        }
    }

    Y_UNIT_TEST(DecimalCoalesceWidensPrecisionWithoutChangingScale) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative(R"(
                DECLARE $a AS Decimal(7,2)?; DECLARE $b AS Decimal(35,2)?; DECLARE $c AS Decimal(12,2);
                SELECT COALESCE($a,$b), COALESCE($b,$a), COALESCE($a,$c,$b), COALESCE($c,$b),
                    COALESCE($a,Decimal('0',35,2)), COALESCE(NULL,$a,$b)
            )", Catalog, settings);
            const auto& columns = compiled.Program->Get().Results[0].Columns;
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 6);
            for (std::size_t i = 0; i < columns.size(); ++i)
                UNIT_ASSERT(columns[i].Type == (TType{ETypeKind::Decimal, i < 2 || i == 5, 35, 2}));
            settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    "SELECT COALESCE(IF(false,Decimal('1',7,2)),Decimal('2',8,3))",
                    "SELECT COALESCE(IF(false,Decimal('1',7,2)),1)",
                    "SELECT Decimal('1',7,2) + Decimal('2',12,2)"})
                UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
        }
    }

    Y_UNIT_TEST(SampleStandardDeviationHasExplicitTypesAndBoundaries) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* alias : {"STDDEV", "STDDEV_SAMPLE", "stddev_samp", "stddev_s", "StdDevSample", "StdDevSamp", "StdDevS"}) {
                for (const auto* type : {"Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64", "Float", "Double"}) {
                    for (const bool nullable : {false, true}) for (const bool grouped : {false, true}) {
                        const auto catalog = [&](std::string_view name, TSourcePosition) {
                            auto valueType = ParseType(type); valueType.OptionalDepth = nullable;
                            return TCatalogTable{std::string(name), {{"k", {ETypeKind::Int32}}, {"v", valueType}}};
                        };
                        const auto sql = std::string("SELECT ") + alias + "(v) AS s FROM t" + (grouped ? " GROUP BY k" : "");
                        const auto compiled = CompileNative(sql, catalog, settings);
                        const auto& program = compiled.Program->Get();
                        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Double, nullable || !grouped}));
                        for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Aggregate)
                            UNIT_ASSERT_VALUES_EQUAL(relation.Aggregates[0].Function, "stddev_samp");
                    }
                }
            }
            for (const auto* sql : {"SELECT STDDEV_SAMPLE(DISTINCT value) FROM t", "SELECT STDDEV(value), AVG(value) FROM t",
                    "SELECT value, STDDEV(id) FROM t GROUP BY ROLLUP(value)",
                    "$f = ($x) -> { RETURN CASE WHEN Math::IsNaN($x) THEN NULL ELSE $x END; }; SELECT $f(STDDEV(value)) FROM t"})
                UNIT_ASSERT(!CompileNative(sql, Catalog, settings).UsesLegacy());
            settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {"SELECT STDDEV(Decimal('1', 3, 2))", "SELECT STDDEV(value), COUNT(DISTINCT id) FROM t",
                    "SELECT STDDEV(DISTINCT value), COUNT(*) FROM t",
                    "SELECT STDDEV_POP(value) FROM t", "SELECT VARIANCE(value) FROM t"})
                UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            // Window syntax uses generated-parser dispatch; it is outside
            // both this aggregate feature and handwritten verification.
            if (parser != EParser::Verify)
                UNIT_ASSERT(CompileNative("SELECT STDDEV(value) OVER () FROM t", Catalog, settings).UsesLegacy());
        }
    }

    Y_UNIT_TEST(JoinStarsRequireUniqueOutputNames) {
        const auto catalog = [](std::string_view name, TSourcePosition) {
            const std::string prefix(name);
            return TCatalogTable{prefix, {{prefix + "id", {ETypeKind::Int32}},
                {prefix + "value", {ETypeKind::Utf8, true}}}};
        };
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* kind : {"CROSS", "INNER", "LEFT", "RIGHT", "FULL", "LEFT SEMI", "RIGHT SEMI", "LEFT ONLY", "RIGHT ONLY"}) {
                const auto join = std::string(kind);
                const auto sql = "SELECT * FROM a AS l " + join + " JOIN b AS r" +
                    (join == "CROSS" ? "" : " ON l.aid = r.bid");
                const auto compiled = CompileNative(sql, catalog, settings);
                const auto& columns = compiled.Program->Get().Results[0].Columns;
                const bool left = join != "RIGHT SEMI" && join != "RIGHT ONLY";
                const bool right = join != "LEFT SEMI" && join != "LEFT ONLY";
                UNIT_ASSERT_VALUES_EQUAL_C(columns.size(), 2 * (left + right), sql);
                std::size_t index = 0;
                for (const auto side : {std::pair{"a", left}, std::pair{"b", right}}) {
                    if (!side.second) continue;
                    const bool outer = join == "FULL" || (side.first == std::string("a") ? join == "RIGHT" : join == "LEFT");
                    UNIT_ASSERT_VALUES_EQUAL_C(columns[index].Name, std::string(side.first) + "id", sql);
                    UNIT_ASSERT_C(columns[index++].Type == (TType{ETypeKind::Int32, outer}), sql);
                    UNIT_ASSERT_VALUES_EQUAL_C(columns[index].Name, std::string(side.first) + "value", sql);
                    UNIT_ASSERT_C(columns[index++].Type == (TType{ETypeKind::Utf8, true}), sql);
                }
            }
            const auto reversed = CompileNative("SELECT r.*, l.* FROM a AS l LEFT JOIN b AS r ON l.aid = r.bid", catalog, settings);
            const auto& columns = reversed.Program->Get().Results[0].Columns;
            UNIT_ASSERT_VALUES_EQUAL(columns[0].Name, "bid");
            UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT_VALUES_EQUAL(columns[2].Name, "aid");
            UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Int32}));
            for (const auto* sql : {
                    "SELECT * FROM a AS l JOIN a AS r ON l.aid = r.aid",
                    "SELECT l.*, r.* FROM a AS l JOIN a AS r ON l.aid = r.aid",
                    "SELECT l.*, l.aid AS aid FROM a AS l JOIN b AS r ON l.aid = r.bid"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog, settings), TUnsupportedFeature, sql);
            for (const auto* sql : {
                    "SELECT missing.* FROM a AS l JOIN b AS r ON l.aid = r.bid",
                    "SELECT r.* FROM a AS l LEFT SEMI JOIN b AS r ON l.aid = r.bid"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog, settings), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(UnicodeCaseHasUtf8InputsAndResults) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("DECLARE $x AS Utf8?; SELECT Unicode::ToUpper($x), Unicode::ToLower(Utf8('AZ')), Unicode::ToUpper('a'), Unicode::ToLower(NULL)", Catalog, settings);
            const auto& columns = compiled.Program->Get().Results[0].Columns;
            for (std::size_t i = 0; i < columns.size(); ++i)
                UNIT_ASSERT(columns[i].Type == (TType{ETypeKind::Utf8, i == 0 || i == 3}));
            for (const auto* sql : {"DECLARE $x AS String; SELECT Unicode::ToUpper($x)", "SELECT Unicode::ToLower(1)",
                    "SELECT Unicode::ToUpper()", "SELECT Unicode::ToLower('a','b')"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(UnnamedGroupingCapturesItsDefinitionScope) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 AS k, COUNT(*) FROM t GROUP BY value + 1 HAVING value + 1 > 0 ORDER BY value + 1",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 AS k, COUNT(*), GROUPING(value + 1) FROM t GROUP BY ROLLUP(value + 1)",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; $q = (SELECT value + 1 AS k FROM t GROUP BY value + 1); PRAGMA DisableYqlSelectAllowUnnamedGroupByExpr; SELECT k FROM $q",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT k FROM (SELECT value + 1 AS k FROM t GROUP BY value + 1) AS q"})
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            const auto duplicate = CompileNative("PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 FROM t GROUP BY value + 1, value + 1", Catalog, settings);
            for (const auto& relation : duplicate.Program->Get().Relations)
                if (relation.Kind == ERelation::Aggregate) UNIT_ASSERT_VALUES_EQUAL(relation.GroupKeys.size(), 1);
            for (const auto* sql : {
                    "SELECT value + 1 FROM t GROUP BY value + 1",
                    "$q = (SELECT value + 1 AS k FROM t GROUP BY value + 1); PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT k FROM $q",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; PRAGMA DisableYqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 FROM t GROUP BY value + 1",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr='false'; SELECT 1"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TUnsupportedFeature, sql);
            for (const auto* sql : {
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value FROM t GROUP BY value + 1",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT expr2 FROM t GROUP BY value + 1"}) {
                try { CompileNative(sql, Catalog, settings); UNIT_FAIL(sql); }
                catch (const TUnsupportedFeature&) { UNIT_FAIL("invalid grouped column delegated"); }
                catch (const TFrontendError&) {}
            }
        }
    }

    Y_UNIT_TEST(GroupingSetsBindNullableOutputsAndExecutableMasks) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("SELECT id, value, SUM(id), COUNT(*), GROUPING(id,value) AS g FROM t GROUP BY ROLLUP(id,value)", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& outputs = program.Results[0].Columns;
            UNIT_ASSERT(outputs[0].Type == (TType{ETypeKind::Uint64, true}));
            UNIT_ASSERT(outputs[1].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT(outputs[2].Type == (TType{ETypeKind::Uint64, true}));
            UNIT_ASSERT(outputs[3].Type == (TType{ETypeKind::Uint64}));
            UNIT_ASSERT(outputs[4].Type == (TType{ETypeKind::Uint64}));
            const TRelation* group = nullptr;
            std::optional<TExprId> mask;
            for (const auto& relation : program.Relations) {
                if (relation.Kind == ERelation::Aggregate) group = &relation;
                for (const auto& definition : relation.Definitions) if (definition.Column == outputs[4].Column) mask = definition.Expression;
            }
            UNIT_ASSERT(group && mask);
            UNIT_ASSERT_VALUES_EQUAL(group->GroupingSets.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(group->GroupingColumns.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(group->GroupingIndicators.size(), 2);
            for (const auto& [output, input] : group->GroupingColumns) UNIT_ASSERT_VALUES_UNEQUAL(output, input);
            for (std::uint64_t bits = 0; bits < 4; ++bits) {
                TEvaluationContext eval;
                eval.Column = [&](TColumnId id) {
                    for (std::size_t i = 0; i < group->GroupingIndicators.size(); ++i)
                        if (group->GroupingIndicators[i].first == id) return TValue{{ETypeKind::Uint64}, (bits >> (1 - i)) & 1};
                    throw std::logic_error("unexpected grouping mask input");
                };
                UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(program.Expressions, *mask, eval).Data), bits);
            }
            const auto common = CompileNative("SELECT id, value, SUM(id) FROM t GROUP BY id, ROLLUP(value)", Catalog, settings);
            UNIT_ASSERT(!common.Program->Get().Results[0].Columns[0].Type.OptionalDepth);
            UNIT_ASSERT(!common.Program->Get().Results[0].Columns[2].Type.OptionalDepth);
            const auto duplicate = CompileNative("SELECT id, GROUPING(id) FROM t GROUP BY GROUPING SETS ((id,id),(id),(id))", Catalog, settings);
            for (const auto& relation : duplicate.Program->Get().Relations) if (relation.Kind == ERelation::Aggregate) {
                UNIT_ASSERT_VALUES_EQUAL(relation.GroupKeys.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(relation.GroupingSets.size(), 3);
                for (const auto& set : relation.GroupingSets) UNIT_ASSERT_VALUES_EQUAL(set.size(), 1);
            }
            for (const auto* sql : {
                    "SELECT COUNT(*) FROM t GROUP BY ()",
                    "SELECT id, GROUPING(id) FROM t GROUP BY id",
                    "SELECT k, GROUPING(k) FROM t GROUP BY ROLLUP(id + 1ul AS k)",
                    "SELECT id, SUM(value) FROM t GROUP BY ROLLUP(id) HAVING GROUPING(id) = 1ul"})
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            for (const auto* sql : {
                    "SELECT GROUPING() FROM t GROUP BY id",
                    "SELECT GROUPING(value) FROM t GROUP BY id",
                    "SELECT GROUPING(SUM(id)) FROM t GROUP BY id",
                    "SELECT GROUPING(id) FROM t",
                    "SELECT id FROM t WHERE GROUPING(id) = 0ul"}) {
                try { CompileNative(sql, Catalog, settings); UNIT_FAIL(sql); }
                catch (const TUnsupportedFeature&) { UNIT_FAIL("invalid GROUPING delegated"); }
                catch (const TFrontendError&) {}
            }
            settings.Mode = ECompilerMode::Auto;
            const auto bareNull = CompileNative("SELECT k FROM t GROUP BY ROLLUP(NULL AS k)", Catalog, settings);
            UNIT_ASSERT(bareNull.UsesLegacy() && bareNull.Unsupported->Feature == EFeature::GroupingSets);
        }
    }

    Y_UNIT_TEST(OrderExpressionsUseOutputAliasesAboveProjection) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "SELECT value + 1 AS id FROM t ORDER BY id + 2",
                    "SELECT id, GROUPING(id) AS g, SUM(value) AS s FROM t GROUP BY ROLLUP(id) ORDER BY CASE WHEN g = 0ul THEN id ELSE NULL END, s + 1"}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Map) {
                    const auto& input = program.Relations[relation.Inputs[0]].Columns;
                    for (const auto& definition : relation.Definitions) {
                        std::vector<TExprId> pending{definition.Expression};
                        while (!pending.empty()) {
                            const auto& node = program.Expressions[pending.back()]; pending.pop_back();
                            if (node.Op == EOpcode::Column)
                                UNIT_ASSERT_C(std::find(input.begin(), input.end(), node.ColumnId) != input.end(), sql);
                            pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
                        }
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(MixedDistinctAggregatesIncludeAverageStates) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    "SELECT COUNT(DISTINCT value), COUNT(*), SUM(value), MIN(value), MAX(value), SOME(value) FROM t",
                    "SELECT value, SUM(DISTINCT id), COUNT(DISTINCT id), COUNT(*) FROM t GROUP BY value",
                    "SELECT SUM(DISTINCT value), SUM(DISTINCT id) FROM t",
                    "SELECT AVG(DISTINCT value) FROM t", "SELECT AVG(value), AVG(id) FROM t",
                    "SELECT COUNT(DISTINCT id), AVG(value) FROM t", "SELECT AVG(DISTINCT value), COUNT(*) FROM t",
                    "SELECT AVG(DISTINCT id), AVG(DISTINCT value), SUM(value) FROM t"}) {
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            }

        }
    }

    Y_UNIT_TEST(MixedNumericOperationsUseNativePromotion) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {"SELECT id < value, value >= id, id = value, value != id FROM t",
                    "SELECT id FROM t WHERE id BETWEEN -1 AND 100",
                    "SELECT COUNT(*) AS n FROM t HAVING COUNT(*) > 0",
                    "SELECT a.id FROM t AS a FULL JOIN t AS b ON a.id = b.value"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
            }
            for (const auto& [sql, nullable] : {std::pair{"SELECT id + value FROM t", true},
                    {"SELECT IF(true, id, value) FROM t", true}, {"SELECT COALESCE(value, id) FROM t", false}}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
                UNIT_ASSERT((result.Program->Get().Results[0].Columns[0].Type == TType{ETypeKind::Uint64, nullable}));
            }
            const auto floating = CompileNative("SELECT id + Float('1'), value + Double('1'), Float('1') + Double('2') FROM t", Catalog, settings);
            const auto& columns = floating.Program->Get().Results[0].Columns;
            UNIT_ASSERT((columns[0].Type == TType{ETypeKind::Float}));
            UNIT_ASSERT((columns[1].Type == TType{ETypeKind::Double, true}));
            UNIT_ASSERT((columns[2].Type == TType{ETypeKind::Double}));
        }
    }

    Y_UNIT_TEST(InvalidJoinConstraintsAreErrorsWithoutDelegation) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* source : {"t AS a JOIN t AS b", "t AS a LEFT JOIN t AS b", "t AS a FULL JOIN t AS b",
                    "t AS a RIGHT ONLY JOIN t AS b", "t AS a CROSS JOIN t AS b ON a.id = b.id", "t AS a, t AS b ON true"}) {
                try {
                    CompileNative(std::string("SELECT a.id FROM ") + source, Catalog, settings);
                    UNIT_FAIL("invalid JOIN was accepted");
                } catch (const TUnsupportedFeature&) {
                    UNIT_FAIL("invalid JOIN was delegated");
                } catch (const TFrontendError& error) {
                    UNIT_ASSERT_C(std::string(error.what()).find("ON or USING") != std::string::npos, error.what());
                }
            }
        }
    }

    Y_UNIT_TEST(FullJoinResidualsHaveAnExplicitBackendBoundary) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            settings.Mode = ECompilerMode::Auto;
            for (const auto* predicate : {"a.id = b.id", "a.id = b.id AND a.value = b.value", "b.value = a.value AND a.id = b.id"}) {
                const auto result = CompileNative(std::string("SELECT a.id FROM t AS a FULL JOIN t AS b ON ") + predicate, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), predicate);
            }
            for (const auto* predicate : {"a.id = b.id AND a.value > b.value", "a.id = b.id AND a.value > 0",
                    "a.id = b.id AND a.value = a.value", "a.id + 1ul = b.id", "a.id = b.id OR a.value = b.value", "true"}) {
                const auto sql = std::string("SELECT a.id FROM t AS a FULL JOIN t AS b ON ") + predicate;
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported->Feature == EFeature::Backend, predicate);
                auto required = settings; required.Mode = ECompilerMode::NativeRequired;
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, required), TUnsupportedFeature);
            }
        }
    }

    Y_UNIT_TEST(WindowsBindToSharedRelationalOperators) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto plain = CompileNative("SELECT SUM(value) OVER(PARTITION BY id), AVG(value) OVER(PARTITION BY id), SUM(value) OVER(PARTITION BY id) FROM t", Catalog, settings);
            std::size_t groups = 0;
            for (const auto& relation : plain.Program->Get().Relations) {
                UNIT_ASSERT(relation.Kind != ERelation::Aggregate);
                if (relation.Kind == ERelation::Window) {
                    ++groups;
                    UNIT_ASSERT_VALUES_EQUAL(relation.WindowFunctions.size(), 2);
                    UNIT_ASSERT_VALUES_EQUAL(relation.GroupKeys.size(), 1);
                    UNIT_ASSERT(relation.WindowFrame.End == EWindowBound::UnboundedFollowing);
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(groups, 1);
            const auto& outputs = plain.Program->Get().Results[0].Columns;
            UNIT_ASSERT(outputs[0].Type == (TType{ETypeKind::Int64, true}));
            UNIT_ASSERT(outputs[1].Type == (TType{ETypeKind::Double, true}));
            const auto grouped = CompileNative("SELECT id, SUM(value), AVG(SUM(value)) OVER(), Rank() OVER(ORDER BY SUM(value) DESC) AS r FROM t GROUP BY id HAVING SUM(value) > 0 ORDER BY r", Catalog, settings);
            std::size_t aggregates = 0, windows = 0;
            for (const auto& relation : grouped.Program->Get().Relations) {
                if (relation.Kind == ERelation::Aggregate) { ++aggregates; UNIT_ASSERT_VALUES_EQUAL(relation.Aggregates.size(), 1); }
                if (relation.Kind == ERelation::Window) ++windows;
            }
            UNIT_ASSERT_VALUES_EQUAL(aggregates, 1);
            UNIT_ASSERT_VALUES_EQUAL(windows, 2);
            const auto forward = CompileNative("SELECT SUM(id) OVER(ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING), COUNT(*) OVER(), Rank() OVER(ORDER BY id DESC), DenseRank() OVER(ORDER BY id) FROM t", Catalog, settings);
            UNIT_ASSERT(forward.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Uint64, true}));
            UNIT_ASSERT(forward.Program->Get().Results[0].Columns[1].Type == (TType{ETypeKind::Uint64}));
        }
    }

    Y_UNIT_TEST(WindowDefaultsAndExplicitUnsupportedBoundaries) {
        for (const bool ansi : {false, true}) {
            TCompilerSettings settings; settings.Binding.AnsiCurrentRow = ansi;
            const auto captured = CompileNative(std::string("$q = (SELECT SUM(value) OVER(ORDER BY id) AS s FROM t); ") +
                (ansi ? "PRAGMA DisableAnsiCurrentRow; " : "PRAGMA AnsiCurrentRow; ") + "SELECT s FROM $q", Catalog, settings);
            for (const auto& relation : captured.Program->Get().Relations) if (relation.Kind == ERelation::Window)
                UNIT_ASSERT(relation.WindowFrame.Type == (ansi ? EWindowFrame::Range : EWindowFrame::Rows));
            for (const std::string pragma : {"", "PRAGMA AnsiCurrentRow; ", "PRAGMA DisableAnsiCurrentRow; "}) {
                const auto result = CompileNative(pragma + "SELECT SUM(value) OVER(ORDER BY id) FROM t", Catalog, settings);
                bool found = false;
                for (const auto& relation : result.Program->Get().Relations) if (relation.Kind == ERelation::Window) {
                    const bool expectedAnsi = pragma.empty() ? ansi : pragma == "PRAGMA AnsiCurrentRow; ";
                    UNIT_ASSERT(relation.WindowFrame.Type == (expectedAnsi ? EWindowFrame::Range : EWindowFrame::Rows));
                    found = true;
                }
                UNIT_ASSERT(found);
            }
        }
        for (const auto* sql : {
            "SELECT SUM(DISTINCT value) OVER() FROM t",
            "SELECT Lag(value) OVER(ORDER BY id) FROM t",
            "SELECT SUM(value) FILTER(WHERE value > 0) OVER() FROM t",
            "SELECT SUM(value) OVER(ORDER BY id GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t",
            "SELECT SUM(value) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING EXCLUDE CURRENT ROW) FROM t",
            "SELECT SUM(value) OVER(RANGE UNBOUNDED PRECEDING) FROM t",
            "DECLARE $n AS Int32; SELECT SUM(value) OVER(ORDER BY id ROWS $n PRECEDING) FROM t",
            "SELECT SUM(SUM(value) OVER()) OVER() FROM t",
            "SELECT id FROM t WHERE RowNumber() OVER() > 0ul",
            "SELECT SUM((SELECT 1)) OVER() FROM t",
            "SELECT id FROM t ORDER BY SUM(value) OVER()"})
            UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog), TUnsupportedFeature, sql);
        for (const auto* sql : {
            "SELECT SUM(value) OVER(ROWS BETWEEN UNBOUNDED FOLLOWING AND CURRENT ROW) FROM t",
            "SELECT SUM(value) OVER(ROWS BETWEEN CURRENT ROW AND UNBOUNDED PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ROWS -1 PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ROWS 1L PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ORDER BY id, value RANGE 1 PRECEDING) FROM t"})
            UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog), TFrontendError, sql);
    }

    Y_UNIT_TEST(WindowSubqueriesRequireNoExternalCaptures) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "SELECT (SELECT SUM(value) OVER() FROM t LIMIT 1)",
                    "SELECT id FROM t WHERE value IN (SELECT s FROM (SELECT SUM(value) OVER() AS s FROM t) AS w)",
                    "SELECT EXISTS (SELECT id FROM (SELECT id, RowNumber() OVER(ORDER BY id) AS n FROM t) AS w WHERE n = 1ul)",
                    "$w = (SELECT value, RowNumber() OVER(ORDER BY id) AS n FROM t); SELECT (SELECT value FROM $w WHERE n = 1ul)",
                    "SELECT (SELECT (SELECT SUM(value) OVER() FROM t LIMIT 1))",
                    "SELECT (SELECT SUM(value) OVER() FROM t AS a LIMIT 1) FROM t AS a"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
                const auto& relations = result.Program->Get().Relations;
                UNIT_ASSERT_C(std::any_of(relations.begin(), relations.end(), [](const auto& r) { return r.Kind == ERelation::Window; }), sql);
            }
            settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    "SELECT (SELECT SUM(b.value) OVER() FROM t AS b WHERE b.id = a.id LIMIT 1) FROM t AS a",
                    "SELECT EXISTS (SELECT RowNumber() OVER(PARTITION BY a.id ORDER BY b.id) FROM t AS b) FROM t AS a",
                    "SELECT a.id FROM t AS a WHERE a.value IN (SELECT s FROM (SELECT SUM(b.value) OVER() AS s FROM t AS b WHERE b.id = a.id) AS w)",
                    "SELECT (SELECT w.s FROM (SELECT SUM(value) OVER() AS s, id FROM t) AS w WHERE w.id = a.id LIMIT 1) FROM t AS a",
                    "SELECT (SELECT (SELECT SUM(b.value) OVER() FROM t AS b WHERE b.id = a.id LIMIT 1)) FROM t AS a",
                    "SELECT (SELECT SUM(value) OVER() AS v FROM t UNION ALL SELECT a.value AS v) FROM t AS a"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy(), sql);
                UNIT_ASSERT_C(result.Unsupported->Feature == EFeature::Backend, sql);
                UNIT_ASSERT_C(result.Unsupported->Message.find("window decorrelation") != std::string::npos, sql);
            }
        }
    }

    Y_UNIT_TEST(RuntimeDatesAndIntervalsUseExecutionOperations) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto result = CompileNative("DECLARE $s AS Utf8?; DECLARE $d AS Int32?; SELECT CAST($s AS Date), CAST($s AS Date) + DateTime::IntervalFromDays($d), DateTime::IntervalFromHours($d), DateTime::IntervalFromMinutes($d)", Catalog, settings);
            UNIT_ASSERT(!result.UsesLegacy());
            const auto& columns = result.Program->Get().Results[0].Columns;
            UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Date, true}));
            UNIT_ASSERT(columns[1].Type == (TType{ETypeKind::Date, true}));
            UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Interval, true}));
            UNIT_ASSERT(columns[3].Type == (TType{ETypeKind::Interval, true}));
            UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $s AS String; SELECT CAST($s AS Timestamp)", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $n AS Int64; SELECT DateTime::IntervalFromDays($n)", Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(CalendarBindingsAreScalarAndCannotBePersisted) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto result = CompileNative("$d = DateTime::ShiftMonths(Date('2020-01-31'), 1); $f = ($x)->{RETURN DateTime::MakeDate($x);}; SELECT $f($d), DateTime::GetYear($d), CAST(Date('2020-01-01') AS Timestamp)", Catalog, settings);
            UNIT_ASSERT(!result.UsesLegacy());
            UNIT_ASSERT(result.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Date, true}));
            UNIT_ASSERT(result.Program->Get().Results[0].Columns[1].Type == (TType{ETypeKind::Uint16, true}));
            UNIT_ASSERT(result.Program->Get().Results[0].Columns[2].Type == (TType{ETypeKind::Timestamp}));
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::Split(Date('2020-01-01'))", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::MakeDate(d) FROM (SELECT DateTime::Split(Date('2020-01-01')) AS d)", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::ShiftMonths(Date('2020-01-01'), 2147483648L)", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::GetYear(NULL)", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::MakeDate(DateTime::ShiftMonths(NULL, 1))", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(FastParserAndGrammarRouting) {
        for (const auto* sql : {"SELECT 1", "SELECT value FROM t WHERE value > 0", "SELECT Int32('1')", "SELECT 1 IN (1, 2,)",
                               "SELECT 1 IN [1, 2,]", "SELECT NULL IN []", "SELECT 1 NOT IN [,]",
                               "SELECT 'x' AS `from`", "SELECT 'x'u", "SELECT x FROM t AS a JOIN t AS b ON a.id = b.id"}) {
            UNIT_ASSERT_C(!ParseNative(sql).UsedGeneratedParser, sql);
            AssertParserAgreement(sql);
        }
        for (const auto* sql : {"SELECT 'привет'", "SELECT @@multiline@@", "SELECT 1 NOT NULL"}) {
            UNIT_ASSERT_C(ParseNative(sql).UsedGeneratedParser, sql);
            AssertParserAgreement(sql);
        }
        for (const auto* sql : {"SELECT * AS x FROM t", "SELECT NOT NOT true", "SELECT FROM FROM t", "SELECT 1 IN ()",
                               "SELECT 1 IS NULL = true", "SELECT 1 < 2 < 3", "SELECT 1 == 2 == 3",
                               "SELECT 1 IN [1,,2]", "SELECT 1 IN [1,2", "SELECT 1 IN [1] + 1",
                               "SELECT 1 UNION ALL SELECT 2 LIMIT 1", "SELECT a.x.y.* FROM a"}) AssertParserAgreement(sql);
    }

    Y_UNIT_TEST(CorrespondenceAcrossBenchmarkCorpus) {
        for (const auto& [suite, count] : {std::pair{"tpch", 22}, {"tpcds", 99}, {"clickbench", 43}}) {
            for (int i = 1; i <= count; ++i) {
                TString sql;
                UNIT_ASSERT(NResource::FindExact(TStringBuilder() << suite << "-q" << i, &sql));
                AssertParserAgreement(std::string_view(sql.data(), sql.size()));
                const auto withHelpers = TString(R"(
                    $to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };
                    $to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };
                    $round = ($x,$y) -> {return $x;};
                )") + sql;
                AssertParserAgreement(std::string_view(withHelpers.data(), withHelpers.size()));
            }
        }
    }

    Y_UNIT_TEST(PrecedenceAndSyntaxMutationCorrespondence) {
        const std::vector<std::string> atoms{"1", "true", "NULL", "a", "a.x", "'x'", "(a + 1)", "IF(true, 1, 2)", "CAST(1 AS Int64)"};
        const std::vector<std::string> ops{" + ", " * ", " || ", " ?? ", " = ", " < ", " AND ", " OR ", " IN ", " IS NULL ", " BETWEEN ", ","};
        for (const auto& a : atoms) for (const auto& b : atoms) for (const auto& op : ops) {
            AssertParserAgreement("SELECT " + a + op + b);
            AssertParserAgreement("SELECT " + a + op + b + " = true OR false");
        }
        for (const std::string sql : {"SELECT x + 1 AS y FROM t WHERE x > 2 ORDER BY y DESC LIMIT 10;",
                                     "SELECT CASE WHEN true THEN 1 ELSE 2 END, IF(false, 1);",
                                     "$f = ($x) -> { RETURN $x + 1; }; SELECT $f(1);",
                                     "WITH a AS (SELECT 1 AS x) SELECT x FROM a;"}) {
            for (std::size_t i = 0; i < sql.size(); ++i) {
                AssertParserAgreement(sql.substr(0, i) + sql.substr(i + 1));
                AssertParserAgreement(sql.substr(0, i) + "," + sql.substr(i));
            }
        }
        for (const auto* sql : {"SELECT 'a''b'", "SELECT 'a\\n'", "SELECT \"a\"\"b\"", "SELECT `a``b` FROM t",
                               "SELECT 1--comment\r,2", "SELECT 1\v", "SELECT 1; /* outer /* inner */ outer */ SELECT 2"}) {
            AssertParserAgreement(sql);
            AssertParserAgreement(sql, true);
        }
    }

    Y_UNIT_TEST(KeywordContextsFollowSharedGrammar) {
        for (const auto& keyword : Keywords) {
            const std::string word(keyword.Word);
            for (const auto& sql : {"SELECT " + word, "SELECT 1 AS " + word, "SELECT 1 " + word,
                                    "SELECT * FROM " + word, "SELECT x FROM t " + word,
                                    "SELECT " + word + "(1)", "SELECT a." + word + " FROM t AS a"}) {
                AssertParserAgreement(sql);
            }
        }
        for (const auto* sql : {"$unused = *; SELECT 1", "SELECT COUNT(DISTINCT *)", "SELECT COUNT(*,)",
                               "SELECT 1.2f", "SELECT 1foo", "SELECT 1e2u", "SELECT 0xfaceul", "SELECT 1in(1)",
                               "SELECT * + 1 FROM t", "SELECT (t.*) FROM t", "SELECT 1 WHERE *", "SELECT COUNT(ALL 1)",
                               "SELECT COUNT(ALL)", "SELECT COUNT(DISTINCT)", "SELECT t.* AS x FROM t"}) AssertParserAgreement(sql);
        for (const auto& keyword : Keywords) {
            const std::string word(keyword.Word);
            AssertParserAgreement("SELECT a.x FROM a " + word + " JOIN b ON a.x = b.x");
            AssertParserAgreement("SELECT 1 " + word + " BY x");
        }
        for (const auto* sql : {"SELECT 1 UNION ALL WITH a AS (SELECT 2) SELECT * FROM a",
                               "SELECT a.x FROM a NATURAL JOIN b ON a.x = b.x", "SELECT * FROM t OUTER JOIN s",
                               "SELECT 1 AS x UNION ALL SELECT 2 AS x", "WITH a AS (SELECT 1 AS x), SELECT x FROM a"})
            AssertParserAgreement(sql);
    }

    Y_UNIT_TEST(OnlyUnsupportedPermitsFallback) {
        TCompilerSettings settings;
        settings.Mode = ECompilerMode::Auto;
        for (const auto* sql : {"SELECT ListLength([1, 2])", "SELECT LAG(value) OVER () FROM a", "SELECT * FROM a JOIN b USING(id)",
                               "PRAGMA foo='x'; SELECT 1",
                               "SELECT CASE WHEN true THEN 1 END", "SELECT * FROM a CROSS JOIN b",
                               "SELECT 'a''b'", "SELECT 1 AS 'answer'", "$'x' = 1; SELECT 1",
                               "SELECT value + 1 FROM a GROUP BY value + 1",
                               "SELECT id, STDDEV_POP(value) FROM a GROUP BY id",
                               "SELECT id, VAR_SAMP(value) FROM a GROUP BY id"}) {
            const auto result = CompileNative(sql, Catalog, settings);
            UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported, sql);
        }
        for (const auto* sql : {"SELECT FROM", "SELECT missing FROM a", "SELECT 1 + true", "DECLARE $x AS Int32; DECLARE $x AS Int32; SELECT $x"}) {
            UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TFrontendError);
        }
        settings.Parsing.MaxQueryBytes = 3;
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT 1", Catalog, settings), TResourceLimitError);
        settings.Parsing.MaxQueryBytes = 1000;
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT * FROM a", [](auto, auto) -> TCatalogTable { throw std::logic_error("catalog bug"); }, settings), std::logic_error);
    }

    Y_UNIT_TEST(RequiredAndLegacyModesAreExplicit) {
        TCompilerSettings settings;
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT ListLength([1, 2])", Catalog, settings), TUnsupportedFeature);
        settings.Mode = ECompilerMode::Legacy;
        const auto result = CompileNative("not parsed in legacy mode", [](auto, auto) -> TCatalogTable { UNIT_FAIL("unexpected catalog call"); return {}; }, settings);
        UNIT_ASSERT(result.UsesLegacy() && !result.Unsupported);
    }

    Y_UNIT_TEST(DirectivesApplyOnlyToLeadingComments) {
        UNIT_ASSERT(CompileNative("SELECT '--!syntax_pg' AS text", Catalog).Program);
        UNIT_ASSERT(CompileNative("-- ordinary comment\n--!syntax_pg\nSELECT 1", Catalog).Program);
        UNIT_ASSERT(CompileNative("--!syntax_v1\nSELECT 1", Catalog).Program);
        UNIT_ASSERT(CompileNative("--!ansi_lexer\nSELECT 'a''b'", Catalog).Program);
        UNIT_ASSERT_EXCEPTION(CompileNative("--!syntax_pg\nSELECT 1", Catalog), TUnsupportedFeature);
        TCompilerSettings settings;
        settings.Mode = ECompilerMode::Auto;
        UNIT_ASSERT_EXCEPTION(CompileNative("--!bad\nSELECT 1", Catalog, settings), TFrontendError);
    }

    Y_UNIT_TEST(CompatibleResultTypesAndParserAgreement) {
        TCompilerSettings settings;
        settings.Parser = EParser::Verify;
        auto result = CompileNative("SELECT Int32('1') AS literal, CAST(1 AS Int64) AS widened, Length('abc') AS size", Catalog, settings);
        const auto& columns = result.Program->Get().Results[0].Columns;
        UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Int32}));
        UNIT_ASSERT(columns[1].Type == (TType{ETypeKind::Int64}));
        UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Uint32}));
        auto scalar = CompileNative("SELECT 1.0 / 2.0 AS ratio, Substring('abc', 1) AS part, CAST(1 AS Double) AS converted", Catalog, settings);
        const auto& scalarColumns = scalar.Program->Get().Results[0].Columns;
        UNIT_ASSERT(scalarColumns[0].Type == (TType{ETypeKind::Double}));
        UNIT_ASSERT(scalarColumns[1].Type == (TType{ETypeKind::String}));
        UNIT_ASSERT(scalarColumns[2].Type == (TType{ETypeKind::Double}));
        auto promoted = CompileNative("SELECT 1u + 2, IF(true, 1L, 2ul)", Catalog, settings);
        UNIT_ASSERT((promoted.Program->Get().Results[0].Columns[0].Type == TType{ETypeKind::Int32}));
        UNIT_ASSERT((promoted.Program->Get().Results[0].Columns[1].Type == TType{ETypeKind::Int64}));
        for (const auto* sql : {"SELECT LOWER('ABC')",
            "SELECT CAST(Float('1') AS Int32)", "SELECT CAST(Float('1') AS Uint64)"}) {
            UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(DecimalProfileIsExplicit) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto result = CompileNative("SELECT Decimal('1.25',8,2), CAST(1 AS Decimal(8,2)), CAST('inf' AS Decimal(8,2)), Decimal('1',8,2) / Decimal('0',8,2)", Catalog, settings);
            const auto& columns = result.Program->Get().Results[0].Columns;
            for (std::size_t i = 0; i < columns.size(); ++i)
                UNIT_ASSERT(columns[i].Type == (TType{ETypeKind::Decimal, i == 2, 8, 2}));
            const auto common = CompileNative("DECLARE $i AS Int64; SELECT IF(true, Decimal('1.25',8,2), $i), Decimal('1',8,2) < $i", Catalog, settings);
            UNIT_ASSERT(common.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Decimal, false, 22, 2}));
            UNIT_ASSERT(common.Program->Get().Results[0].Columns[1].Type == TType{ETypeKind::Bool});
            const auto products = CompileNative("DECLARE $i AS Int64; DECLARE $u AS Uint64?; SELECT Decimal('1.25',3,2) * $i, $i * Decimal('1.25',3,2), Decimal('0',3,2) * $u", Catalog, settings);
            const auto& productColumns = products.Program->Get().Results[0].Columns;
            for (std::size_t i = 0; i < productColumns.size(); ++i)
                UNIT_ASSERT(productColumns[i].Type == (TType{ETypeKind::Decimal, i == 2, 3, 2}));
            const auto divisions = CompileNative("DECLARE $i AS Int64; DECLARE $u AS Uint64?; SELECT Decimal('1.25',3,2) / $i, Decimal('0',35,35) / $u, CAST(Decimal('125',8,2) / 50 AS int)", Catalog, settings);
            const auto& divisionColumns = divisions.Program->Get().Results[0].Columns;
            UNIT_ASSERT(divisionColumns[0].Type == (TType{ETypeKind::Decimal, false, 3, 2}));
            UNIT_ASSERT(divisionColumns[1].Type == (TType{ETypeKind::Decimal, true, 35, 35}));
            UNIT_ASSERT(divisionColumns[2].Type == (TType{ETypeKind::Int32, true}));
            for (const auto* sql : {"SELECT Decimal('1e2',8,2)",
                "SELECT Decimal('1',8,2) + 1", "SELECT Decimal('1',8,2) + Decimal('1',12,2)",
                "SELECT 2 / Decimal('1',8,2)", "SELECT Decimal('1',8,2) % 2",
                "SELECT COALESCE(IF(false, Decimal('1.25',8,2)), 1L)"})
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(DecimalTextAndNestedOptionalCastsUseNativeTypesInBothParsers) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto result = CompileNative(R"(
                DECLARE $s AS String?;
                SELECT CAST($s AS Decimal(8,2)) AS parsed, CAST('1e2' AS Decimal(8,2)) AS exponent,
                    CAST($s AS String??) AS nested, CAST(NULL AS String??) AS absent,
                    Unwrap(CAST($s AS String??)) AS unwrapped;
            )", Catalog, settings);
            UNIT_ASSERT(result.Program);
            const auto& columns = result.Program->Get().Results[0].Columns;
            UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Decimal, 1, 8, 2}));
            UNIT_ASSERT(columns[1].Type == columns[0].Type);
            UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::String, 2}));
            UNIT_ASSERT(columns[3].Type == columns[2].Type);
            UNIT_ASSERT(columns[4].Type == (TType{ETypeKind::String, 1}));
            const auto combined = CompileNative(R"(SELECT CAST('a' AS String?) AS x UNION ALL SELECT CAST('b' AS String??) AS x)", Catalog, settings);
            UNIT_ASSERT(combined.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::String, 2}));
            settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    R"(SELECT SUM(CAST(1 AS Int32??)))", R"(SELECT AVG(CAST(1 AS Int32??)))",
                    R"(SELECT CAST(1 AS Int32??) IN (SELECT CAST(1 AS Int32??)))",
                    R"(SELECT IF(true, CAST(1 AS Int32??), CAST(2 AS Int64??)))"})
                UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
        }
    }

    Y_UNIT_TEST(TypedStringSuffixesUseNativeLiteralsInBothParsers) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto result = CompileNative(R"(SELECT 'abc'u AS a, "def"U AS b, 'ghi's AS c, "jkl"S AS d)", Catalog, settings);
            const auto& columns = result.Program->Get().Results[0].Columns;
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 4);
            for (size_t i = 0; i < columns.size(); ++i)
                UNIT_ASSERT(columns[i].Type == (TType{i < 2 ? ETypeKind::Utf8 : ETypeKind::String}));
        }
    }

    Y_UNIT_TEST(SqlScalarAliasesUseCanonicalTypes) {
        using K = ETypeKind;
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto& [alias, kind] : {std::pair{"tinyint", K::Int8}, {"SMALLINT", K::Int16},
                    {"int", K::Int32}, {"Integer", K::Int32}, {"bigint", K::Int64}, {"varchar", K::String}}) {
                const auto sql = std::string("DECLARE $x AS ") + alias + "?; SELECT $x, CAST('12' AS " + alias + "), " + alias + "('12')";
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                UNIT_ASSERT(program.Parameters.at("x") == (TType{kind, true}));
                const auto& columns = program.Results[0].Columns;
                UNIT_ASSERT(columns[0].Type == (TType{kind, true}));
                UNIT_ASSERT(columns[1].Type == (TType{kind, kind != K::String}));
                UNIT_ASSERT(columns[2].Type == (TType{kind}));
            }
            for (const auto* sql : {"SELECT CAST(1 AS byte)", "SELECT CAST('a' AS varchar123)", "SELECT CAST('a' AS varchar(12))"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(DiscoverOnlyReachableTablesWithBindingPrefixAndCteScope) {
        const auto syntax = ParseNative("$unused = SELECT * FROM unused; $q = SELECT * FROM a; PRAGMA TablePathPrefix='/Root'; WITH c AS (SELECT * FROM b) SELECT * FROM $q CROSS JOIN c; SELECT * FROM a");
        const auto tables = CollectTables(syntax);
        UNIT_ASSERT_VALUES_EQUAL(tables.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(tables[0].Name, "/Root/a");
        UNIT_ASSERT_VALUES_EQUAL(tables[1].Name, "/Root/b");
    }
}
