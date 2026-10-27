#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include "kqp_rbo_test_helpers.h"

#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_join_builder.h>

#include <yql/essentials/ast/yql_ast.h>
#include <yql/essentials/ast/yql_expr.h>

#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace NKikimr::NKqp {
namespace {

using TSeed = std::vector<std::pair<std::optional<std::string>, i32>>;
using TMarkerBag = std::vector<std::pair<std::optional<i32>, std::optional<i32>>>;
using TPayloadBag = std::vector<std::tuple<std::optional<i32>, std::optional<i32>,
    std::optional<std::string>, std::optional<std::string>>>;

void CollectNodes(const TExprNode::TPtr& node, TStringBuf name, TExprNode::TListType& result);

void CheckDecimalJoins(bool blockJoin, const TString& mode) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(true);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetDefaultLangVer(202501);
    service->SetDefaultCostBasedOptimizationLevel(0);
    service->SetUseBlockHashJoin(blockJoin);
    TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
    auto tableClient = kikimr.GetTableClient();
    auto tableSession = tableClient.CreateSession().GetValueSync().GetSession();
    const auto created = tableSession.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/DecimalDirectLeft` (
            Id Int32 NOT NULL, N Int32 NOT NULL, K Decimal(10,0), K2 Decimal(10,0), KR Decimal(10,0) NOT NULL, Marker Int32, PRIMARY KEY (Id));
        CREATE TABLE `/Root/DecimalDirectRight` (
            Id Int32 NOT NULL, N Int32 NOT NULL, K Decimal(10,0), K2 Decimal(10,0), KR Decimal(10,0) NOT NULL, Marker Int32, PRIMARY KEY (Id));
        CREATE TABLE `/Root/DecimalEmptyRight` (
            Id Int32 NOT NULL, N Int32 NOT NULL, K Decimal(10,0), K2 Decimal(10,0), KR Decimal(10,0) NOT NULL, Marker Int32, PRIMARY KEY (Id));
    )").GetValueSync();
    UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
    const TSeed left{{"7", 10}, {"7", 10}, {std::nullopt, 30}, {"nan", 40},
        {"inf", 50}, {"-inf", 60}, {"-3", 70}, {"2", 80}};
    const TSeed right{{"7", 100}, {"7", 101}, {std::nullopt, 300}, {"nan", 400},
        {"inf", 500}, {"-inf", 600}, {"-3", 700}, {"3", 800}};
    const auto seed = [&](const TString& table, const TSeed& values) {
        NYdb::TValueBuilder rows;
        rows.BeginList();
        for (size_t index = 0; index < values.size(); ++index) {
            rows.AddListItem().BeginStruct()
                .AddMember("Id").Int32(static_cast<i32>(index + 1))
                .AddMember("N").Int32(static_cast<i32>(index + 1))
                .AddMember("Marker").OptionalInt32(values[index].second);
            for (const std::string& name : {std::string("K"), std::string("K2")}) {
                rows.AddMember(name);
                if (values[index].first) {
                    rows.BeginOptional().Decimal(NYdb::TDecimalValue(*values[index].first, 10, 0)).EndOptional();
                } else {
                    rows.EmptyOptional(NYdb::TTypeBuilder().Decimal(NYdb::TDecimalType(10, 0)).Build());
                }
            }
            rows.AddMember("KR").Decimal(NYdb::TDecimalValue(values[index].first.value_or("nan"), 10, 0));
            rows.EndStruct();
        }
        const auto result = tableClient.BulkUpsert(table, rows.EndList().Build()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), table << "\n" << result.GetIssues().ToString());
    };
    seed("/Root/DecimalDirectLeft", left);
    seed("/Root/DecimalDirectRight", right);
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();

    // Tie the join oracle to ordinary equality on the exact stored input,
    // including present NaN versus NULL. No constant-folded NaN literals.
    const std::vector<std::optional<bool>> expectedScalar{
        true, true, std::nullopt, false, true, true, true, true};
    for (const TString& table : {TString("/Root/DecimalDirectLeft"), TString("/Root/DecimalDirectRight")}) {
        const TString query = TStringBuilder() << "PRAGMA YqlSelect = 'force'; "
            << "SELECT Id AS id, K = K AS eq FROM `" << table << "` ORDER BY Id;";
        const auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        const auto& rs = result.GetResultSet(0);
        UNIT_ASSERT(!rs.Truncated());
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Name, "id");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[1].Name, "eq");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Type.ToString(), "Int32");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[1].Type.ToString(), "Bool?");
        NYdb::TResultSetParser parser(rs);
        size_t count = 0;
        while (parser.TryNextRow()) {
            UNIT_ASSERT(count < expectedScalar.size());
            UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser(0).GetInt32(), static_cast<i32>(count + 1));
            UNIT_ASSERT_C(parser.ColumnParser(1).GetOptionalBool() == expectedScalar[count],
                query << "\n" << FormatResultSetYson(rs));
            ++count;
        }
        UNIT_ASSERT_VALUES_EQUAL(count, expectedScalar.size());
    }

    const TMarkerBag matches{{10, 100}, {10, 101}, {10, 100}, {10, 101},
        {50, 500}, {60, 600}, {70, 700}};
    auto leftBag = matches;
    leftBag.insert(leftBag.end(), {{30, std::nullopt}, {40, std::nullopt}, {80, std::nullopt}});
    auto fullBag = leftBag;
    fullBag.insert(fullBag.end(), {{std::nullopt, 300}, {std::nullopt, 400}, {std::nullopt, 800}});
    auto rightBag = matches;
    rightBag.insert(rightBag.end(), {{std::nullopt, 300}, {std::nullopt, 400}, {std::nullopt, 800}});
    struct TCase {
        TString Kind;
        TString Condition;
        TMarkerBag Expected;
        bool RightEmpty = false;
        bool RequiredKey = false;
    };
    const TString ordinary = "l.K = r.K";
    std::vector<TCase> cases{
        {"LEFT", ordinary, leftBag},
        {"INNER", ordinary, matches},
        {"LEFT SEMI", ordinary, {{10, std::nullopt}, {10, std::nullopt},
            {50, std::nullopt}, {60, std::nullopt}, {70, std::nullopt}}},
        {"LEFT ONLY", ordinary, {{30, std::nullopt}, {40, std::nullopt}, {80, std::nullopt}}},
        // N duplicates Id's values without its PK metadata: a PK equality
        // would select an index lookup instead of the tested hash-join core.
        {"LEFT", "l.K = r.K AND l.N = r.N", {{10, 100}, {10, 101},
            {30, std::nullopt}, {40, std::nullopt}, {50, 500}, {60, 600}, {70, 700}, {80, std::nullopt}}},
        // Repeated left key occurrences use distinct right IUs.
        {"LEFT", "l.K = r.K AND l.K = r.K2", leftBag},
        {"LEFT", "l.KR = r.KR", leftBag, false, true},
        {"INNER", "l.KR = r.KR", matches, false, true},
        {"LEFT SEMI", "l.KR = r.KR", {{10, std::nullopt}, {10, std::nullopt},
            {50, std::nullopt}, {60, std::nullopt}, {70, std::nullopt}}, false, true},
        {"LEFT ONLY", "l.KR = r.KR", {{30, std::nullopt}, {40, std::nullopt}, {80, std::nullopt}}, false, true},
        // A correlation value remains a domain identity even when ordinary
        // equality is not reflexive for that value. Every row has an unequal
        // right key here, including both stored required NaNs.
        {"LEFT SEMI", "l.KR != r.KR", {{10, std::nullopt}, {10, std::nullopt},
            {30, std::nullopt}, {40, std::nullopt}, {50, std::nullopt},
            {60, std::nullopt}, {70, std::nullopt}, {80, std::nullopt}}, false, true},
        {"LEFT ONLY", "l.KR != r.KR", {}, false, true},
        {"LEFT", ordinary, {{10, std::nullopt}, {10, std::nullopt}, {30, std::nullopt},
            {40, std::nullopt}, {50, std::nullopt}, {60, std::nullopt},
            {70, std::nullopt}, {80, std::nullopt}}, true},
    };
    // Block and Map only implement the left-sided families above; the planner
    // canonicalizes RIGHT and uses Grace for FULL. Do not claim a Block FULL test.
    if (!blockJoin && mode == "grace") {
        cases.push_back({"RIGHT", ordinary, rightBag});
        cases.push_back({"FULL", ordinary, fullBag});
    }
    if (blockJoin) {
        cases.push_back({"LEFT", "l.K = r.K AND l.Marker < 20", {{10, 100}, {10, 101},
            {10, 100}, {10, 101}, {30, std::nullopt}, {40, std::nullopt},
            {50, std::nullopt}, {60, std::nullopt}, {70, std::nullopt}, {80, std::nullopt}}});
    }
    const auto keyForMarker = [](const TSeed& input, const std::optional<i32>& marker, bool required) -> std::optional<std::string> {
        if (!marker) {
            return std::nullopt;
        }
        const auto found = std::find_if(input.begin(), input.end(), [&](const auto& item) { return item.second == *marker; });
        UNIT_ASSERT(found != input.end());
        return required ? std::optional<std::string>{found->first.value_or("nan")} : found->first;
    };
    const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
    const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
    const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
    TStringBuilder mismatches;
    for (const auto& item : cases) {
        const bool leftOnly = item.Kind == "LEFT SEMI" || item.Kind == "LEFT ONLY";
        const TString leftKeyProjection = item.RequiredKey ? "CAST(l.KR AS Decimal(10,0)?)" : "l.K";
        const TString rightKeyProjection = leftOnly ? "CAST(NULL AS Decimal(10,0))"
            : item.RequiredKey ? "CAST(r.KR AS Decimal(10,0)?)" : "r.K";
        const TString rightTable = item.RightEmpty ? "/Root/DecimalEmptyRight" : "/Root/DecimalDirectRight";
        // The YqlSelect frontend rejects explicit SEMI/ONLY join syntax. Use
        // supported correlated EXISTS routes and check their physical kind.
        const TString from = leftOnly
            ? TString(TStringBuilder() << "FROM `/Root/DecimalDirectLeft` AS l WHERE "
                << (item.Kind == "LEFT ONLY" ? "NOT " : "")
                << "EXISTS (SELECT 1 FROM `" << rightTable << "` AS r WHERE " << item.Condition << ")")
            : TString(TStringBuilder() << "FROM `/Root/DecimalDirectLeft` AS l " << item.Kind
                << " JOIN `" << rightTable << "` AS r ON " << item.Condition);
        const TString query = TStringBuilder()
            << "PRAGMA YqlSelect = 'force'; PRAGMA ydb.CostBasedOptimizationLevel = '0'; "
            << "PRAGMA ydb.HashJoinMode = '" << mode << "'; "
            << "SELECT l.Marker AS zeta, " << (leftOnly ? "CAST(NULL AS Int32)" : "r.Marker") << " AS alpha, "
            << leftKeyProjection << " AS left_key, " << rightKeyProjection << " AS right_key "
            << from << ";";
        const auto beforeSuccess = successes->Val();
        const auto beforeFailure = failures->Val();
        const auto explain = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NYdb::NQuery::EExecMode::Explain)).GetValueSync();
        UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
        UNIT_ASSERT(successes->Val() > beforeSuccess);
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
        UNIT_ASSERT(explain.GetStats());
        UNIT_ASSERT(explain.GetStats()->GetAst());
        const auto ast = *explain.GetStats()->GetAst();
        const std::string expectedCore = blockJoin ? "BlockHashJoinCore" : mode == "map" ? "MapJoinCore" : "GraceJoinCore";
        UNIT_ASSERT_C(ast.find(expectedCore) != std::string::npos, query << "\n" << ast);
        if (leftOnly) {
            const auto parsed = NYql::ParseAst(TStringBuf(ast));
            UNIT_ASSERT_C(parsed.IsOk(), ast);
            NYql::TExprContext exprCtx;
            NYql::TExprNode::TPtr exprRoot;
            UNIT_ASSERT_C(NYql::CompileExpr(*parsed.Root, exprRoot, exprCtx, nullptr, nullptr), ast);
            TExprNode::TListType cores;
            CollectNodes(exprRoot, TStringBuf(expectedCore), cores);
            const TStringBuf expectedKind = item.Kind == "LEFT SEMI" ? "LeftSemi" : "LeftOnly";
            UNIT_ASSERT_C(std::any_of(cores.begin(), cores.end(), [&](const auto& core) {
                return core->ChildrenSize() > 2 && core->Child(2)->IsAtom()
                    && core->Child(2)->Content() == expectedKind;
            }), query << "\n" << ast);
        }
        const auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        const auto& rs = result.GetResultSet(0);
        UNIT_ASSERT(!rs.Truncated());
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 4);
        const std::vector<std::string> names{"zeta", "alpha", "left_key", "right_key"};
        for (size_t column = 0; column < names.size(); ++column) {
            UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[column].Name, names[column]);
            UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[column].Type.ToString(), column < 2 ? "Int32?" : "Decimal(10,0)?");
        }
        TPayloadBag expected;
        for (const auto& [leftMarker, rightMarker] : item.Expected) {
            expected.emplace_back(leftMarker, rightMarker,
                keyForMarker(left, leftMarker, item.RequiredKey), keyForMarker(right, rightMarker, item.RequiredKey));
        }
        TPayloadBag actual;
        NYdb::TResultSetParser parser(rs);
        while (parser.TryNextRow()) {
            const auto leftKey = parser.ColumnParser(2).GetOptionalDecimal();
            const auto rightKey = parser.ColumnParser(3).GetOptionalDecimal();
            actual.emplace_back(parser.ColumnParser(0).GetOptionalInt32(), parser.ColumnParser(1).GetOptionalInt32(),
                leftKey ? std::make_optional(leftKey->ToString()) : std::nullopt,
                rightKey ? std::make_optional(rightKey->ToString()) : std::nullopt);
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        if (actual != expected) {
            mismatches << query << "\n" << FormatResultSetYson(rs) << "\n";
            Cerr << "[DECIMAL-JOIN-MISMATCH]\n" << query << "\n" << ast
                 << "\n" << FormatResultSetYson(rs) << Endl;
        } else {
            Cerr << "[DECIMAL-JOIN-PASS] " << query << Endl;
        }
    }
    UNIT_ASSERT_C(mismatches.empty(), mismatches);
}

void CheckDecimalIdentityJoins(bool blockJoin, const TString& mode) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(true);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetDefaultLangVer(202501);
    service->SetDefaultCostBasedOptimizationLevel(0);
    service->SetUseBlockHashJoin(blockJoin);
    service->SetEnableInlineJoinFiltersAfterCBO(true);
    TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
    auto tableClient = kikimr.GetTableClient();
    auto tableSession = tableClient.CreateSession().GetValueSync().GetSession();
    const auto created = tableSession.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/DecimalIdentityLeft` (
            KR Decimal(10,0) NOT NULL, Id Int32 NOT NULL, Marker Int32,
            PRIMARY KEY (KR, Id));
        CREATE TABLE `/Root/DecimalIdentityRight` (
            Id Int32 NOT NULL, N Int32 NOT NULL, Marker Int32, PRIMARY KEY (Id));
    )").GetValueSync();
    UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
    const std::vector<std::string> keys{"7", "7", "nan", "nan", "inf", "-inf", "-3", "2"};
    NYdb::TValueBuilder leftRows, rightRows;
    leftRows.BeginList();
    rightRows.BeginList();
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto id = static_cast<i32>(i + 1);
        leftRows.AddListItem().BeginStruct()
            .AddMember("KR").Decimal(NYdb::TDecimalValue(keys[i], 10, 0))
            .AddMember("Id").Int32(id).AddMember("Marker").OptionalInt32(id).EndStruct();
        rightRows.AddListItem().BeginStruct()
            .AddMember("Id").Int32(id).AddMember("N").Int32(id)
            .AddMember("Marker").OptionalInt32(id + 100).EndStruct();
    }
    const auto leftSeed = tableClient.BulkUpsert("/Root/DecimalIdentityLeft", leftRows.EndList().Build()).GetValueSync();
    const auto rightSeed = tableClient.BulkUpsert("/Root/DecimalIdentityRight", rightRows.EndList().Build()).GetValueSync();
    UNIT_ASSERT_C(leftSeed.IsSuccess(), leftSeed.GetIssues().ToString());
    UNIT_ASSERT_C(rightSeed.IsSuccess(), rightSeed.GetIssues().ToString());
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
    const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
    const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
    const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
    const auto beforeSuccess = successes->Val(), beforeFailure = failures->Val();
    const auto scalar = session.ExecuteQuery(
        "PRAGMA YqlSelect='force'; SELECT Id AS id, KR=KR AS eq FROM `/Root/DecimalIdentityLeft` ORDER BY Id;",
        NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
    UNIT_ASSERT_C(scalar.IsSuccess(), scalar.GetIssues().ToString());
    UNIT_ASSERT(successes->Val() > beforeSuccess);
    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
    UNIT_ASSERT_VALUES_EQUAL(scalar.GetResultSets().size(), 1);
    const auto& scalarSet = scalar.GetResultSet(0);
    UNIT_ASSERT(!scalarSet.Truncated());
    UNIT_ASSERT_VALUES_EQUAL(scalarSet.GetColumnsMeta().size(), 2);
    UNIT_ASSERT_VALUES_EQUAL(scalarSet.GetColumnsMeta()[0].Type.ToString(), "Int32");
    UNIT_ASSERT_VALUES_EQUAL(scalarSet.GetColumnsMeta()[1].Type.ToString(), "Bool");
    NYdb::TResultSetParser scalarParser(scalarSet);
    size_t scalarCount = 0;
    while (scalarParser.TryNextRow()) {
        UNIT_ASSERT(scalarCount < keys.size());
        UNIT_ASSERT_VALUES_EQUAL(scalarParser.ColumnParser(0).GetInt32(), static_cast<i32>(scalarCount + 1));
        UNIT_ASSERT_VALUES_EQUAL(scalarParser.ColumnParser(1).GetBool(), keys[scalarCount] != "nan");
        ++scalarCount;
    }
    UNIT_ASSERT_VALUES_EQUAL(scalarCount, keys.size());

    const std::string expectedCore = blockJoin ? "BlockHashJoinCore" : mode == "map" ? "MapJoinCore" : "GraceJoinCore";
    for (const bool window : {true, false}) {
        const TString body = window
            ? "SELECT Id AS id, KR AS original_key, COUNT(*) OVER (PARTITION BY KR) AS value FROM `/Root/DecimalIdentityLeft`;"
            : "SELECT l.Id AS id, l.KR AS original_key, r.Marker AS value "
              "FROM `/Root/DecimalIdentityLeft` AS l LEFT JOIN `/Root/DecimalIdentityRight` AS r "
              "ON l.Id=r.N AND l.Marker<r.Marker AND l.Id!=4;";
        const TString query = TStringBuilder() << "PRAGMA YqlSelect='force'; "
            << "PRAGMA ydb.WindowFunctionsV2='true'; "
            << "PRAGMA ydb.CostBasedOptimizationLevel='0'; PRAGMA ydb.HashJoinMode='" << mode << "'; " << body;
        const auto compiledBefore = successes->Val();
        const auto explain = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NYdb::NQuery::EExecMode::Explain)).GetValueSync();
        UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
        UNIT_ASSERT(successes->Val() > compiledBefore);
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
        UNIT_ASSERT(explain.GetStats() && explain.GetStats()->GetAst());
        const auto ast = *explain.GetStats()->GetAst();
        const auto parsed = NYql::ParseAst(TStringBuf(ast));
        UNIT_ASSERT_C(parsed.IsOk(), ast);
        NYql::TExprContext exprCtx;
        NYql::TExprNode::TPtr exprRoot;
        UNIT_ASSERT_C(NYql::CompileExpr(*parsed.Root, exprRoot, exprCtx, nullptr, nullptr), ast);
        TExprNode::TListType cores;
        CollectNodes(exprRoot, TStringBuf(expectedCore), cores);
        const TStringBuf expectedKind = window ? "Inner" : "Left";
        UNIT_ASSERT_C(std::any_of(cores.begin(), cores.end(), [&](const auto& core) {
            return core->ChildrenSize() > 2 && core->Child(2)->IsAtom() && core->Child(2)->Content() == expectedKind;
        }), query << "\n" << ast);
        UNIT_ASSERT_C(ast.find("StablePickle") != std::string::npos || ast.find("EqualNulls") != std::string::npos,
            query << "\n" << ast);
        if (window) {
            UNIT_ASSERT_C(ast.find("WideChopper") == std::string::npos && ast.find("Fold1") == std::string::npos, ast);
        } else {
            // The non-key RHS equality plus residual must become an inner match and a
            // restoration join on the real composite primary key (KR, Id).
            UNIT_ASSERT_C(cores.size() >= 2, ast);
            UNIT_ASSERT_C(std::any_of(cores.begin(), cores.end(), [&](const auto& core) {
                return core->ChildrenSize() > 2 && core->Child(2)->IsAtom() && core->Child(2)->Content() == "Inner";
            }), ast);
        }
        const auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
        const auto& rs = result.GetResultSet(0);
        UNIT_ASSERT(!rs.Truncated());
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Name, "id");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[1].Name, "original_key");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[2].Name, "value");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Type.ToString(), "Int32");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[1].Type.ToString(), "Decimal(10,0)");
        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[2].Type.ToString(), window ? "Uint64" : "Int32?");
        NYdb::TResultSetParser parser(rs);
        std::vector<std::tuple<i32, std::string, std::optional<i64>>> actual, expected;
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto id = static_cast<i32>(i + 1);
            const std::optional<i64> value = window ? std::optional<i64>(i < 4 ? 2 : 1)
                : id == 4 ? std::nullopt : std::optional<i64>(id + 100);
            expected.emplace_back(id, keys[i], value);
        }
        while (parser.TryNextRow()) {
            std::optional<i64> value;
            if (window) {
                const auto count = parser.ColumnParser(2).GetUint64();
                UNIT_ASSERT(count <= keys.size());
                value = static_cast<i64>(count);
            } else {
                const auto marker = parser.ColumnParser(2).GetOptionalInt32();
                if (marker) {
                    value = *marker;
                }
            }
            actual.emplace_back(parser.ColumnParser(0).GetInt32(), parser.ColumnParser(1).GetDecimal().ToString(), value);
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        UNIT_ASSERT_C(actual == expected, query << "\n" << ast << "\n" << FormatResultSetYson(rs));
    }
}

void CollectNodes(const TExprNode::TPtr& node, TStringBuf name, TExprNode::TListType& result) {
    if (node->IsCallable(name)) {
        result.push_back(node);
    }
    for (const auto& child : node->ChildrenList()) {
        CollectNodes(child, name, result);
    }
}

} // namespace

Y_UNIT_TEST_SUITE(KqpRboDecimalJoin) {
    Y_UNIT_TEST(MixedDecimalCorrelationPreservesDomainValues) {
        for (const std::string mode : {"grace", "block", "map"}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(true);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetDefaultLangVer(202501);
            service->SetDefaultCostBasedOptimizationLevel(0);
            service->SetUseBlockHashJoin(mode == "block");
            TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
            auto tableClient = kikimr.GetTableClient();
            auto tableSession = tableClient.CreateSession().GetValueSync().GetSession();
            const auto created = tableSession.ExecuteSchemeQuery(R"(
                CREATE TABLE `/Root/DecimalDomainLow` (Id Int32 NOT NULL, K Decimal(10,0) NOT NULL, PRIMARY KEY(Id));
                CREATE TABLE `/Root/DecimalDomainHigh` (Id Int32 NOT NULL, K Decimal(12,1) NOT NULL, PRIMARY KEY(Id));
            )").GetValueSync();
            UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
            const std::vector<std::string> low{"7", "7", "nan", "inf", "-inf", "2"};
            const std::vector<std::string> high{"7.0", "7.0", "7.5", "nan", "inf", "-inf", "3.0"};
            const auto seed = [&](const TString& table, const std::vector<std::string>& keys, ui8 precision, ui8 scale) {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                for (size_t i = 0; i < keys.size(); ++i) {
                    rows.AddListItem().BeginStruct().AddMember("Id").Int32(static_cast<i32>(i + 1))
                        .AddMember("K").Decimal(NYdb::TDecimalValue(keys[i], precision, scale)).EndStruct();
                }
                const auto result = tableClient.BulkUpsert(table, rows.EndList().Build()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            };
            seed("/Root/DecimalDomainLow", low, 10, 0);
            seed("/Root/DecimalDomainHigh", high, 12, 1);
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
            const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
            const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
            const auto failureCount = failures->Val();
            for (const bool outerHigh : {false, true}) {
                const TString outer = outerHigh ? "/Root/DecimalDomainHigh" : "/Root/DecimalDomainLow";
                const TString inner = outerHigh ? "/Root/DecimalDomainLow" : "/Root/DecimalDomainHigh";
                const auto& keys = outerHigh ? high : low;
                const auto compiledBeforeScalar = successes->Val();
                const auto scalar = session.ExecuteQuery(TStringBuilder()
                    << "PRAGMA YqlSelect='force'; SELECT Id AS id, K=K AS eq FROM `" << outer << "`;",
                    NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(scalar.IsSuccess(), scalar.GetIssues().ToString());
                UNIT_ASSERT(successes->Val() > compiledBeforeScalar);
                UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failureCount);
                UNIT_ASSERT_VALUES_EQUAL(scalar.GetResultSets().size(), 1);
                const auto& scalarSet = scalar.GetResultSet(0);
                UNIT_ASSERT(!scalarSet.Truncated());
                UNIT_ASSERT_VALUES_EQUAL(scalarSet.GetColumnsMeta().size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(scalarSet.GetColumnsMeta()[0].Type.ToString(), "Int32");
                UNIT_ASSERT_VALUES_EQUAL(scalarSet.GetColumnsMeta()[1].Type.ToString(), "Bool");
                NYdb::TResultSetParser scalarParser(scalarSet);
                std::vector<bool> seen(keys.size(), false);
                size_t scalarRows = 0;
                while (scalarParser.TryNextRow()) {
                    const auto id = scalarParser.ColumnParser(0).GetInt32();
                    UNIT_ASSERT(id > 0 && static_cast<size_t>(id) <= keys.size());
                    UNIT_ASSERT(!seen[id - 1]);
                    seen[id - 1] = true;
                    UNIT_ASSERT_VALUES_EQUAL(scalarParser.ColumnParser(1).GetBool(), keys[id - 1] != "nan");
                    ++scalarRows;
                }
                UNIT_ASSERT_VALUES_EQUAL(scalarRows, keys.size());

                for (const bool negated : {false, true}) {
                    const TString query = TStringBuilder()
                        << "PRAGMA YqlSelect='force'; PRAGMA ydb.CostBasedOptimizationLevel='0'; "
                        << "PRAGMA ydb.HashJoinMode='" << (mode == "map" ? "map" : "grace") << "'; "
                        << "SELECT l.Id AS id, l.K AS original_key FROM `" << outer << "` AS l WHERE "
                        << (negated ? "NOT " : "") << "EXISTS (SELECT 1 FROM `" << inner << "` AS r WHERE r.K=l.K);";
                    const auto compiledBefore = successes->Val();
                    const auto explain = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(),
                        NYdb::NQuery::TExecuteQuerySettings().ExecMode(NYdb::NQuery::EExecMode::Explain)).GetValueSync();
                    UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
                    UNIT_ASSERT(successes->Val() > compiledBefore);
                    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failureCount);
                    UNIT_ASSERT(explain.GetStats() && explain.GetStats()->GetAst());
                    const auto ast = *explain.GetStats()->GetAst();
                    const std::string core = mode == "block" ? "BlockHashJoinCore" : mode == "map" ? "MapJoinCore" : "GraceJoinCore";
                    UNIT_ASSERT_C(ast.find(core) != std::string::npos, query << "\n" << ast);
                    const auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
                    UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString());
                    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failureCount);
                    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                    const auto& rs = result.GetResultSet(0);
                    UNIT_ASSERT(!rs.Truncated());
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 2);
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Name, "id");
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[1].Name, "original_key");
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Type.ToString(), "Int32");
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[1].Type.ToString(), outerHigh ? "Decimal(12,1)" : "Decimal(10,0)");
                    std::vector<std::pair<i32, std::string>> expected, actual;
                    for (size_t i = 0; i < keys.size(); ++i) {
                        const bool matches = keys[i] == "7" || keys[i] == "7.0" || keys[i] == "inf" || keys[i] == "-inf";
                        if (matches != negated) {
                            expected.emplace_back(static_cast<i32>(i + 1),
                                NYdb::TDecimalValue(keys[i], outerHigh ? 12 : 10, outerHigh ? 1 : 0).ToString());
                        }
                    }
                    NYdb::TResultSetParser parser(rs);
                    while (parser.TryNextRow()) {
                        actual.emplace_back(parser.ColumnParser(0).GetInt32(), parser.ColumnParser(1).GetDecimal().ToString());
                    }
                    std::sort(actual.begin(), actual.end());
                    std::sort(expected.begin(), expected.end());
                    UNIT_ASSERT_C(actual == expected, query << "\n" << ast << "\n" << FormatResultSetYson(rs));
                }
            }
        }
    }


    Y_UNIT_TEST_TWIN(RequiredDecimalPartitionAndLeftRowIdentities, BlockJoin) {
        CheckDecimalIdentityJoins(BlockJoin, "grace");
    }

    Y_UNIT_TEST(RequiredDecimalMapPartitionAndLeftRowIdentities) {
        CheckDecimalIdentityJoins(false, "map");
    }

    Y_UNIT_TEST_TWIN(DirectDecimalKeysRespectScalarEquality, BlockJoin) {
        CheckDecimalJoins(BlockJoin, "grace");
    }

    Y_UNIT_TEST(DirectDecimalMapKeysRespectScalarEquality) {
        CheckDecimalJoins(false, "map");
    }

    Y_UNIT_TEST(OrdinaryDecimalKeysRemainSeparateFromNullSafeOccurrences) {
        // Structural lowering controls complement SQL runtime bags.
        // Decimal null-safe runtime admission is not inferred from this fixture.
        for (const bool nullable : {false, true}) {
            for (const bool nullSafeFirst : {false, true}) {
                NTests::TIdTestContext f;
                const auto a = f.Id("a"), b = f.Id("b"), c = f.Id("c");
                auto left = f.Read({a}), right = f.Read({b, c});
                const TTypeAnnotationNode* item = f.ExprCtx.MakeType<TDataExprParamsType>(EDataSlot::Decimal, "10", "0");
                if (nullable) {
                    item = f.ExprCtx.MakeType<TOptionalExprType>(item);
                }
                const auto setType = [&](IOperator& op) {
                    TVector<const TItemExprType*> fields;
                    for (const auto id : op.GetOutputIUs()) {
                        fields.push_back(f.ExprCtx.MakeType<TItemExprType>(f.ExprCtx.GetIndexAsString(id), item));
                    }
                    op.Type = f.ExprCtx.MakeType<TListExprType>(f.ExprCtx.MakeType<TStructExprType>(
                        fields));
                };
                setType(*left);
                setType(*right);
                TJoinIUs keys;
                keys.Add({a, b, nullSafeFirst});
                keys.Add({a, c, !nullSafeFirst});
                auto join = MakeIntrusive<TOpJoin>(left, right, f.Pos, "Left", std::move(keys));
                auto* joined = join.Get();
                joined->Props.JoinAlgo = EJoinAlgoType::GraceJoin;
                auto root = f.Root(join, {{a, "a"}, {b, "b"}, {c, "c"}});
                ComputePlanLiveness(*root);
                const TPhysicalNames names(root->PlanProps.InfoUnitRegistry);
                const auto physical = TPhysicalJoinBuilder(*joined, f.ExprCtx, f.Pos, names).BuildPhysicalOp(
                    f.ExprCtx.NewArgument(f.Pos, "left"), f.ExprCtx.NewArgument(f.Pos, "right"), true, f.TypeCtx);
                TExprNode::TListType cores, guards;
                CollectNodes(physical, "BlockHashJoinCore", cores);
                CollectNodes(physical, "IfPresent", guards);
                UNIT_ASSERT_VALUES_EQUAL(cores.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(guards.size(), 2); // One ordinary key per side only.
                for (const auto& guard : guards) {
                    UNIT_ASSERT(guard->Head().IsCallable("StrictCast"));
                    UNIT_ASSERT(guard->Child(1)->Tail().IsCallable("If"));
                    UNIT_ASSERT(guard->Child(1)->Tail().Head().IsCallable("=="));
                    UNIT_ASSERT(guard->Tail().IsCallable("Nothing"));
                }
                const auto core = NNodes::TDqBlockHashJoinCore(cores.front());
                UNIT_ASSERT_VALUES_EQUAL(core.LeftKeyColumns().Size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(core.RightKeyColumns().Size(), 2);
                UNIT_ASSERT(core.LeftKeyColumns().Item(0).Value() != core.LeftKeyColumns().Item(1).Value());
                UNIT_ASSERT(core.RightKeyColumns().Item(0).Value() != core.RightKeyColumns().Item(1).Value());
                UNIT_ASSERT_VALUES_EQUAL(core.Settings().Size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(core.Settings().Item(0).Name().Value(), "EqualNulls");
                UNIT_ASSERT_VALUES_EQUAL(core.Settings().Item(0).Value().Cast<NNodes::TCoUint32>().Literal().Value(),
                    nullSafeFirst ? "0" : "1");
            }
        }
    }
}

} // namespace NKikimr::NKqp
