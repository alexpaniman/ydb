// Reference name-resolution contracts from PR #58587, plus identity boundaries.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/execution.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TCompilerSettings Settings(EParser parser) {
    TCompilerSettings settings;
    settings.Parser = parser;
    settings.Binding.DefaultCluster = "local";
    settings.Binding.TablePathPrefix = "/Root";
    settings.Binding.ClusterPrefixes = {{"other", "/remote"}, {"remote", "/Data"}};
    return settings;
}

std::vector<TTableAddress> Addresses(const std::vector<TTableRequest>& requests) {
    std::vector<TTableAddress> result;
    for (const auto& request : requests) result.push_back(request.Address);
    return result;
}
}

Y_UNIT_TEST_SUITE(KqpNativeClusterNames) {
    Y_UNIT_TEST(TableCollectionKeepsCteScopeAndPathPragmaOrder) {
        const std::string sql =
            "PRAGMA TablePathPrefix = '/first'; $q = (SELECT * FROM t); "
            "PRAGMA TablePathPrefix = '/second'; "
            "WITH t AS (SELECT * FROM t), u AS (SELECT * FROM t) "
            "SELECT * FROM u UNION ALL SELECT * FROM $q; "
            "SELECT * FROM u; SELECT * FROM other.t; SELECT * FROM `/absolute`;";
        // Preserve the reference SQL, scopes and complete address set. Native
        // discovery visits dependencies at first use rather than loading every
        // named declaration, including unused ones, in declaration order.
        const std::vector<TTableAddress> expected{
            {"local", "/second/t"}, {"local", "/first/t"}, {"local", "/second/u"},
            {"other", "/remote/t"}, {"local", "/absolute"}};
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            UNIT_ASSERT(Addresses(CollectTables(ParseNative(sql, settings), settings.Binding)) == expected);
        }
    }

    Y_UNIT_TEST(QuotedNamesAndClusterPrefixes) {
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            std::vector<TTableAddress> lookups;
            auto result = CompileNative("SELECT a.`some``column` FROM remote.`strange``table` AS a;",
                [&](const TTableAddress& address, TSourcePosition) {
                    lookups.push_back(address);
                    return TCatalogTable{address.Name, {{"some`column", {ETypeKind::String}}}};
                }, settings);
            const std::vector<TTableAddress> expected{{"remote", "/Data/strange`table"}};
            UNIT_ASSERT(lookups == expected);
            const auto& program = result.Program->Get();
            UNIT_ASSERT_VALUES_EQUAL(program.Results.front().Columns.front().Name, "some`column");
            for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Read)
                UNIT_ASSERT(relation.Table == expected.front());
        }
    }

    Y_UNIT_TEST(EqualPathsInDifferentClustersHaveIndependentCatalogs) {
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            const std::string sql = "SELECT id FROM local.`/same`; SELECT id FROM remote.`/same`; "
                "SELECT id FROM local.`/same`;";
            const std::vector<TTableAddress> expected{{"local", "/same"}, {"remote", "/same"}};
            UNIT_ASSERT(Addresses(CollectTables(ParseNative(sql, settings), settings.Binding)) == expected);
            std::vector<TTableAddress> lookups;
            auto result = CompileNative(sql, [&](const TTableAddress& address, TSourcePosition) {
                lookups.push_back(address);
                return TCatalogTable{address.Name, {{"id", {address.Cluster == "local" ? ETypeKind::Int64 : ETypeKind::String}}}};
            }, settings);
            UNIT_ASSERT(lookups == expected);
            const auto& outputs = result.Program->Get().Results;
            UNIT_ASSERT(outputs[0].Columns[0].Type == TType{ETypeKind::Int64});
            UNIT_ASSERT(outputs[1].Columns[0].Type == TType{ETypeKind::String});
            UNIT_ASSERT(outputs[2].Columns[0].Type == TType{ETypeKind::Int64});
        }
    }

    Y_UNIT_TEST(QualifiedTablesBypassCteNamesAndKeepQuotedDots) {
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            const std::string sql = "WITH t AS (SELECT 1l AS id) SELECT id FROM remote.t; "
                "SELECT id FROM `remote.t`; SELECT id FROM `remote.dot`.`t.part`;";
            const std::vector<TTableAddress> expected{
                {"remote", "/Data/t"}, {"local", "/Root/remote.t"}, {"remote.dot", "/Root/t.part"}};
            UNIT_ASSERT(Addresses(CollectTables(ParseNative(sql, settings), settings.Binding)) == expected);
        }
        UNIT_ASSERT_VALUES_UNEQUAL(SyntaxFingerprint(Parse("SELECT * FROM remote.t")),
            SyntaxFingerprint(Parse("SELECT * FROM `remote.t`")));
    }

    Y_UNIT_TEST(QuotedDollarTableNamesAreNotQueryBindings) {
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            const std::string sql = "$q = SELECT 1l AS id; SELECT id FROM `$q`; "
                "SELECT id FROM remote.`$q`; SELECT id FROM $q;";
            const std::vector<TTableAddress> expected{{"local", "/Root/$q"}, {"remote", "/Data/$q"}};
            UNIT_ASSERT(Addresses(CollectTables(ParseNative(sql, settings), settings.Binding)) == expected);
            std::vector<TTableAddress> lookups;
            auto result = CompileNative(sql, [&](const TTableAddress& address, TSourcePosition) {
                lookups.push_back(address);
                return TCatalogTable{address.Name, {{"id", {ETypeKind::Int64}}}};
            }, settings);
            UNIT_ASSERT(lookups == expected);
            UNIT_ASSERT_VALUES_EQUAL(result.Program->Get().Results.size(), 3);
        }
        UNIT_ASSERT_VALUES_UNEQUAL(SyntaxFingerprint(Parse("SELECT * FROM $q")),
            SyntaxFingerprint(Parse("SELECT * FROM `$q`")));
    }

    Y_UNIT_TEST(AnsiClusterAndTableQuotesPreserveExactNames) {
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            const auto syntax = ParseNative("--!ansi_lexer\nSELECT \"a\".\"Id\" "
                "FROM \"remote\"\"cluster\".\"Mixed\"\"Table\" AS \"a\";", settings);
            const std::vector<TTableAddress> expected{{"remote\"cluster", "/Root/Mixed\"Table"}};
            UNIT_ASSERT(Addresses(CollectTables(syntax, settings.Binding)) == expected);
        }
    }

    Y_UNIT_TEST(EffectDependenciesUseBothClusterAndPath) {
        for (auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            auto settings = Settings(parser);
            auto compiled = CompileNative("UPSERT INTO local.`/same` (id) VALUES (1l); "
                "UPSERT INTO remote.`/same` (id) VALUES (2l); "
                "UPSERT INTO local.`/same` (id) VALUES (3l);",
                [](const TTableAddress& address, TSourcePosition) {
                    return TCatalogTable{address.Name, {{"id", {ETypeKind::Int64}}}, TWriteSchema{{"id"}, {}, false}};
                }, settings);
            const auto plan = PrepareExecution(std::move(*compiled.Program));
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots[0].Phase, 1);
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots[1].Phase, 1);
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots[2].Phase, 2);
        }
    }

    Y_UNIT_TEST(GeneratedColumnsUseTheirOwnCatalogSnapshot) {
        auto settings = Settings(EParser::Verify);
        const auto compiled = CompileNative("INSERT INTO local.`/same` (id) VALUES (10l); "
            "INSERT INTO remote.`/same` (id) VALUES (10l);",
            [](const TTableAddress& address, TSourcePosition) {
                TWriteSchema writes{{"id"}, {}, false};
                writes.GeneratedColumns.push_back({"g", address.Cluster == "local" ? "id + 1l" : "id + 2l", true});
                return TCatalogTable{address.Name, {{"id", {ETypeKind::Int64}}, {"g", {ETypeKind::Int64}}}, std::move(writes)};
            }, settings);
        std::vector<std::int64_t> values;
        for (const auto& relation : compiled.Program->Get().Relations) if (relation.Effect) {
            const auto& columns = relation.Effect->GeneratedColumns;
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 1);
            const auto& expression = *columns.front().second;
            TEvaluationContext context;
            context.Column = [](TColumnId) { return TValue{{ETypeKind::Int64}, std::int64_t{10}}; };
            values.push_back(std::get<std::int64_t>(Evaluate(expression.Expressions, expression.Root, context).Data));
        }
        UNIT_ASSERT(values == (std::vector<std::int64_t>{11, 12}));
    }
}
