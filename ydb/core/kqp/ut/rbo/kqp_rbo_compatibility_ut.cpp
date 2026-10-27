#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <library/cpp/json/json_reader.h>
#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace NKikimr::NKqp {
namespace {

using namespace NYdb;

Y_UNIT_TEST_SUITE(KqpRboCompatibility) {
    // NUL is an admitted one-byte ASCII SQL ESCAPE. Literal and parameter
    // patterns must preserve the same complete bag, including binary subjects.
    Y_UNIT_TEST_TWIN(LikeNulEscapeLiteralAndParameter, Lang202603) {
        NKikimrConfig::TAppConfig config;
        auto* service = config.MutableTableServiceConfig();
        service->SetEnableNewRBO(true);
        service->SetEnableFallbackToYqlOptimizer(false);
        service->SetEnableFallbackOnDML(false);
        service->SetEnableFallbackOnMultipleStatements(false);
        service->SetDefaultLangVer(Lang202603 ? 202603 : 202501);
        TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
        auto tableClient = kikimr.GetTableClient();
        auto tableSession = tableClient.CreateSession().GetValueSync().GetSession();
        const auto created = tableSession.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/LikeNulEscapeRows` (
                Id Int32 NOT NULL, Subject String NOT NULL, PRIMARY KEY (Id)
            );
        )").GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
        struct TRow { i32 Id; std::string Subject; };
        const std::vector<TRow> rows{
            {1, "%"}, {2, "_"}, {3, "x"}, {4, ""}, {5, "%%"}, {6, "__"},
            {7, std::string("\0%", 2)}, {8, std::string("\0_", 2)},
            {9, std::string("\0x", 2)}, {10, std::string("\0", 1)},
        };
        TValueBuilder values;
        values.BeginList();
        for (const auto& row : rows) {
            values.AddListItem().BeginStruct().AddMember("Id").Int32(row.Id)
                .AddMember("Subject").String(row.Subject).EndStruct();
        }
        const auto seeded = tableClient.BulkUpsert(
            "/Root/LikeNulEscapeRows", values.EndList().Build()).GetValueSync();
        UNIT_ASSERT_C(seeded.IsSuccess(), seeded.GetIssues().ToString());
        const TString query = R"SQL(
PRAGMA YqlSelect='force';
PRAGMA AnsiLike;
DECLARE $p AS String;
DECLARE $one AS String;
SELECT Id, Subject,
    Subject LIKE '\x00%'s ESCAPE '\x00's AS LiteralPercent,
    Subject LIKE $p ESCAPE '\x00's AS ParameterPercent,
    Subject LIKE '\x00_'s ESCAPE '\x00's AS LiteralUnderscore,
    Subject LIKE $one ESCAPE '\x00's AS ParameterUnderscore
FROM `/Root/LikeNulEscapeRows`;
        )SQL";
        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        bool first = true;
        TStringBuilder mismatches;
        size_t mismatchedBindings = 0;
        for (const bool escapedPatterns : {true, false}) {
            const auto params = TParamsBuilder()
                .AddParam("$p").String(escapedPatterns ? std::string("\0%", 2) : "x").Build()
                .AddParam("$one").String(escapedPatterns ? std::string("\0_", 2) : "x").Build().Build();
            const auto beforeSuccess = successes->Val(), beforeFailure = failures->Val();
            const auto explain = session.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params,
                NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
            UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
            if (first) { UNIT_ASSERT(successes->Val() > beforeSuccess); first = false; }
            UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
            UNIT_ASSERT(explain.GetStats() && explain.GetStats()->GetAst());
            const TString ast{*explain.GetStats()->GetAst()};
            UNIT_ASSERT_C(ast.Contains("Re2.PatternFromLike") && ast.Contains("Parameter"), ast);
            Cerr << "[LIKE-NUL-ESCAPE] lang=" << (Lang202603 ? 202603 : 202501)
                 << " pattern_bytes=" << (escapedPatterns ? "00 25 / 00 5f" : "78 / 78")
                 << "\n" << query << "\n" << ast << Endl;
            const auto result = session.ExecuteQuery(query,
                NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
            const auto& rs = result.GetResultSet(0);
            UNIT_ASSERT(!rs.Truncated());
            const std::vector<std::string> names{
                "Id", "Subject", "LiteralPercent", "ParameterPercent", "LiteralUnderscore", "ParameterUnderscore"};
            UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[i].Name, names[i]);
                UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[i].Type.ToString(), i == 0 ? "Int32" : i == 1 ? "String" : "Bool");
            }
            std::vector<std::vector<i32>> actual, expected;
            TResultSetParser parser(rs);
            while (parser.TryNextRow()) {
                const auto id = parser.ColumnParser(0).GetInt32();
                const auto found = std::find_if(rows.begin(), rows.end(), [&](const TRow& row) { return row.Id == id; });
                UNIT_ASSERT(found != rows.end());
                UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser(1).GetString(), found->Subject);
                actual.push_back({id, parser.ColumnParser(2).GetBool(), parser.ColumnParser(3).GetBool(),
                    parser.ColumnParser(4).GetBool(), parser.ColumnParser(5).GetBool()});
            }
            for (const auto& row : rows) {
                expected.push_back({row.Id, row.Subject == "%", row.Subject == (escapedPatterns ? "%" : "x"),
                    row.Subject == "_", row.Subject == (escapedPatterns ? "_" : "x")});
            }
            std::sort(actual.begin(), actual.end());
            std::sort(expected.begin(), expected.end());
            if (actual != expected) {
                ++mismatchedBindings;
                mismatches << "pattern_bytes=" << (escapedPatterns ? "00 25 / 00 5f" : "78 / 78")
                    << "\n" << query << "\n" << ast << "\n" << FormatResultSetYson(rs) << "\n";
            }
        }
        UNIT_ASSERT_VALUES_EQUAL_C(mismatchedBindings, 0, mismatches);
    }

    // SQL literal and parameter paths must implement the same escape state.
    // Stored subjects prevent folding the entire match into a literal result.
    Y_UNIT_TEST_TWIN(LikeEscapedBackslashLiteralAndParameter, Lang202603) {
        NKikimrConfig::TAppConfig appConfig;
        auto* service = appConfig.MutableTableServiceConfig();
        service->SetEnableNewRBO(true);
        service->SetEnableFallbackToYqlOptimizer(false);
        service->SetEnableFallbackOnDML(false);
        service->SetEnableFallbackOnMultipleStatements(false);
        service->SetDefaultLangVer(Lang202603 ? 202603 : 202501);
        auto settings = TKikimrSettings(appConfig);
        settings.SetWithSampleTables(false);
        TKikimrRunner kikimr(settings);
        auto tableClient = kikimr.GetTableClient();
        auto tableSession = tableClient.CreateSession().GetValueSync().GetSession();
        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto created = tableSession.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/LikeEscapeRows` (
                Id Int32 NOT NULL, Subject String NOT NULL, PRIMARY KEY (Id)
            );
        )").GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());

        struct TRow { i32 Id; std::string Subject; bool SlashPrefix; bool HashPrefix; };
        const std::vector<TRow> rows{
            {1, "\\x", true, false}, {2, "\\%", true, false},
            {3, "\\", true, false}, {4, "x", false, false},
            {5, "\\xx", true, false}, {6, "\\_", true, false},
            {7, "#x", false, true}, {8, "", false, false},
        };
        TValueBuilder values;
        values.BeginList();
        for (const auto& row : rows) {
            values.AddListItem().BeginStruct()
                .AddMember("Id").Int32(row.Id)
                .AddMember("Subject").String(row.Subject).EndStruct();
        }
        const auto seeded = tableClient.BulkUpsert(
            "/Root/LikeEscapeRows", values.EndList().Build()).GetValueSync();
        UNIT_ASSERT_C(seeded.IsSuccess(), seeded.GetIssues().ToString());

        const TString query = R"SQL(
PRAGMA YqlSelect = 'force';
PRAGMA AnsiLike;
DECLARE $p AS String;
DECLARE $control AS String;
DECLARE $one AS String;
SELECT Id, Subject,
    Subject LIKE @@#\%@@ ESCAPE '#' AS LiteralMatch,
    Subject LIKE $p ESCAPE '#' AS ParameterMatch,
    Subject LIKE @@##%@@ ESCAPE '#' AS LiteralControl,
    Subject LIKE $control ESCAPE '#' AS ParameterControl,
    Subject LIKE @@#\_@@ ESCAPE '#' AS LiteralOne,
    Subject LIKE $one ESCAPE '#' AS ParameterOne
FROM `/Root/LikeEscapeRows`;
        )SQL";
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        bool first = true;
        // A second binding also checks that the pattern remains a runtime input.
        for (const bool slashPattern : {true, false}) {
            const auto params = TParamsBuilder()
                .AddParam("$p").String(slashPattern ? "#\\%" : "##%").Build()
                .AddParam("$control").String("##%").Build()
                .AddParam("$one").String("#\\_").Build().Build();
            const auto beforeSuccess = successes->Val();
            const auto beforeFailure = failures->Val();
            const auto explained = session.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params,
                NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
            UNIT_ASSERT_C(explained.IsSuccess(), explained.GetIssues().ToString());
            if (first) { UNIT_ASSERT(successes->Val() > beforeSuccess); }
            UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
            first = false;
            UNIT_ASSERT(explained.GetStats() && explained.GetStats()->GetAst());
            const TString ast{*explained.GetStats()->GetAst()};
            Cerr << "[LIKE-ESCAPE-AST-BEGIN] lang=" << (Lang202603 ? 202603 : 202501)
                 << " pattern=" << (slashPattern ? "23 5c 25" : "23 23 25")
                 << "\n" << query << "\n" << ast << "\n[LIKE-ESCAPE-AST-END]" << Endl;
            // If either path vanishes, report a fixture coverage failure.
            UNIT_ASSERT_C(ast.Contains("Parameter") && ast.Contains("Re2.PatternFromLike"), ast);

            const auto result = session.ExecuteQuery(query,
                NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString() << "\n" << ast);
            UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
            const auto& rs = result.GetResultSet(0);
            UNIT_ASSERT(!rs.Truncated());
            const std::vector<std::string> names{
                "Id", "Subject", "LiteralMatch", "ParameterMatch", "LiteralControl", "ParameterControl",
                "LiteralOne", "ParameterOne"};
            const auto& columns = rs.GetColumnsMeta();
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL(columns[i].Name, names[i]);
                UNIT_ASSERT_VALUES_EQUAL(columns[i].Type.ToString(), i == 0 ? "Int32" : (i == 1 ? "String" : "Bool"));
            }
            std::vector<std::vector<i32>> actual, expected;
            TResultSetParser parser(rs);
            while (parser.TryNextRow()) {
                const auto id = parser.ColumnParser(0).GetInt32();
                const auto found = std::find_if(rows.begin(), rows.end(),
                    [&](const TRow& row) { return row.Id == id; });
                UNIT_ASSERT(found != rows.end());
                UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser(1).GetString(), found->Subject);
                actual.push_back({id, parser.ColumnParser(2).GetBool(), parser.ColumnParser(3).GetBool(),
                    parser.ColumnParser(4).GetBool(), parser.ColumnParser(5).GetBool(),
                    parser.ColumnParser(6).GetBool(), parser.ColumnParser(7).GetBool()});
            }
            for (const auto& row : rows) {
                expected.push_back({row.Id, row.SlashPrefix, slashPattern ? row.SlashPrefix : row.HashPrefix,
                    row.HashPrefix, row.HashPrefix, row.SlashPrefix && row.Subject.size() == 2,
                    row.SlashPrefix && row.Subject.size() == 2});
            }
            std::sort(actual.begin(), actual.end());
            std::sort(expected.begin(), expected.end());
            Cerr << "[LIKE-ESCAPE-RESULT] " << FormatResultSetYson(rs) << Endl;
            UNIT_ASSERT_VALUES_EQUAL_C(actual, expected, query << "\n" << ast << "\n" << FormatResultSetYson(rs));
        }
    }

    Y_UNIT_TEST(SelectedUniqueIndexRetainsDuplicateNullBusinessKeys) {
        NKikimrConfig::TAppConfig appConfig;
        auto* service = appConfig.MutableTableServiceConfig();
        service->SetEnableNewRBO(true);
        service->SetEnableFallbackToYqlOptimizer(false);
        service->SetEnableFallbackOnDML(false);
        service->SetEnableFallbackOnMultipleStatements(false);
        service->SetDefaultLangVer(202501);
        service->SetDefaultCostBasedOptimizationLevel(0);
        service->SetEnableAutoIndexSelectionForIndexLookupJoin(false);
        service->SetEnableIndexStreamWrite(true);
        auto settings = TKikimrSettings(appConfig);
        settings.SetWithSampleTables(false);
        settings.FeatureFlags.SetEnableAddUniqueIndex(true);
        settings.FeatureFlags.SetEnableAccessToIndexImplTables(true);
        TKikimrRunner kikimr(settings);
        auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto querySession = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto created = tableSession.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/SelectedUniqueRows` (
                Id Int32 NOT NULL, Flag Bool NOT NULL, U String,
                CoverValue String, Payload Int32, PRIMARY KEY (Id),
                INDEX by_fu GLOBAL UNIQUE SYNC ON (Flag, U) COVER (CoverValue)
            );
        )").GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());

        struct TRow {
            i32 Id;
            bool Flag;
            std::optional<std::string> U;
            std::optional<std::string> Cover;
            std::optional<i32> Payload;
        };
        std::vector<TRow> expected{
            {1, true, std::nullopt, "c1", 11},
            {2, true, std::nullopt, "c2", 22},
            {3, true, "a", "c3", 33},
            {4, false, "a", "c4", 44},
        };
        // INSERT SELECT supplies the subquery required by new RBO's DML path.
        // These writes use the real uniqueness checker; BulkUpsert is not used.
        const auto insert = [&](const std::vector<TRow>& input) {
            TValueBuilder rows;
            rows.BeginList();
            for (const auto& row : input) {
                rows.AddListItem().BeginStruct()
                    .AddMember("Id").Int32(row.Id)
                    .AddMember("Flag").Bool(row.Flag)
                    .AddMember("U").OptionalString(row.U)
                    .AddMember("CoverValue").OptionalString(row.Cover)
                    .AddMember("Payload").OptionalInt32(row.Payload)
                    .EndStruct();
            }
            const auto params = TParamsBuilder().AddParam("$rows", rows.EndList().Build()).Build();
            return querySession.ExecuteQuery(R"(
                PRAGMA YqlSelect = 'force';
                DECLARE $rows AS List<Struct<Id:Int32,Flag:Bool,U:String?,CoverValue:String?,Payload:Int32?>>;
                INSERT INTO `/Root/SelectedUniqueRows` (Id, Flag, U, CoverValue, Payload)
                SELECT Id, Flag, U, CoverValue, Payload FROM AS_TABLE($rows);
            )", NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
        };
        const auto seeded = insert(expected);
        UNIT_ASSERT_C(seeded.IsSuccess(), seeded.GetIssues().ToString());

        const TString indexPath = "/Root/SelectedUniqueRows/by_fu/indexImplTable";
        const auto described = tableSession.DescribeTable(indexPath).GetValueSync();
        UNIT_ASSERT_C(described.IsSuccess(), described.GetIssues().ToString());
        const auto& description = described.GetTableDescription();
        const std::vector<std::string> physicalKey{"Flag", "U", "Id"};
        UNIT_ASSERT_VALUES_EQUAL(description.GetPrimaryKeyColumns(), physicalKey);
        const auto storedColumns = description.GetTableColumns();
        UNIT_ASSERT_VALUES_EQUAL(storedColumns.size(), 4);
        for (const auto& column : storedColumns) {
            if (column.Name == "Id") {
                UNIT_ASSERT_VALUES_EQUAL(column.Type.ToString(), "Int32");
            } else if (column.Name == "Flag") {
                UNIT_ASSERT_VALUES_EQUAL(column.Type.ToString(), "Bool");
            } else {
                UNIT_ASSERT_C(column.Name == "U" || column.Name == "CoverValue", column.Name);
                UNIT_ASSERT_VALUES_EQUAL(column.Type.ToString(), "String?");
            }
        }
        const auto expectedRow = [&](i32 id) -> const TRow& {
            const auto found = std::find_if(expected.begin(), expected.end(),
                [&](const TRow& row) { return row.Id == id; });
            UNIT_ASSERT_C(found != expected.end(), "Unexpected row Id=" << id);
            return *found;
        };
        const auto checkStoredRows = [&]() {
            // Read both physical tables directly and consume every part. This
            // checks NULL retention and rollback of a rejected unique insert.
            // SQL planning cannot replace the index assertion with a base scan.
            for (const bool indexRead : {false, true}) {
                const TString readPath = indexRead ? indexPath : TString{"/Root/SelectedUniqueRows"};
                auto stream = tableSession.ReadTable(readPath, NYdb::NTable::TReadTableSettings().Ordered(true)).GetValueSync();
                UNIT_ASSERT_C(stream.IsSuccess(), stream.GetIssues().ToString());
                std::vector<i32> actualIds;
                for (;;) {
                    auto part = stream.ReadNext().GetValueSync();
                    if (!part.IsSuccess()) {
                        UNIT_ASSERT_C(part.EOS(), part.GetIssues().ToString());
                        break;
                    }
                    const auto resultSet = part.ExtractPart();
                    // ReadTable marks each streaming fragment as truncated. Completeness
                    // is checked by consuming EOS and comparing the entire row bag below.
                    TResultSetParser parser(resultSet);
                    while (parser.TryNextRow()) {
                        // ReadTable may wrap required storage columns in Optional.
                        auto& idCell = parser.ColumnParser("Id");
                        const auto id = idCell.GetKind() == TTypeParser::ETypeKind::Optional
                            ? idCell.GetOptionalInt32() : std::optional<i32>{idCell.GetInt32()};
                        UNIT_ASSERT(id);
                        const auto& row = expectedRow(*id);
                        auto& flagCell = parser.ColumnParser("Flag");
                        const auto flag = flagCell.GetKind() == TTypeParser::ETypeKind::Optional
                            ? flagCell.GetOptionalBool() : std::optional<bool>{flagCell.GetBool()};
                        UNIT_ASSERT(flag);
                        UNIT_ASSERT_VALUES_EQUAL(*flag, row.Flag);
                        UNIT_ASSERT(parser.ColumnParser("U").GetOptionalString() == row.U);
                        UNIT_ASSERT(parser.ColumnParser("CoverValue").GetOptionalString() == row.Cover);
                        if (!indexRead) {
                            UNIT_ASSERT(parser.ColumnParser("Payload").GetOptionalInt32() == row.Payload);
                        }
                        actualIds.push_back(*id);
                    }
                }
                std::vector<i32> expectedIds;
                for (const auto& row : expected) { expectedIds.push_back(row.Id); }
                std::sort(actualIds.begin(), actualIds.end());
                std::sort(expectedIds.begin(), expectedIds.end());
                UNIT_ASSERT_VALUES_EQUAL(actualIds, expectedIds);
            }
        };

        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        const auto checkReads = [&](bool firstCompilation) {
            // Automatic selection must choose the covered Bool-prefix scan and
            // then the noncovering prefix scan plus primary-key lookup.
            for (const bool fetch : {false, true}) {
                const TString query = TStringBuilder()
                    << "PRAGMA YqlSelect = 'force'; SELECT Id, U, "
                    << (fetch ? "Payload" : "CoverValue") << " AS Value "
                    << "FROM `/Root/SelectedUniqueRows` WHERE Flag = true;";
                const auto successesBefore = successes->Val();
                const auto failuresBefore = failures->Val();
                const auto explain = querySession.ExecuteQuery(query, NQuery::TTxControl::NoTx(),
                    NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
                UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
                if (firstCompilation) { UNIT_ASSERT_C(successes->Val() > successesBefore, query); }
                UNIT_ASSERT_VALUES_EQUAL_C(failures->Val(), failuresBefore, query);
                UNIT_ASSERT_C(explain.GetStats() && explain.GetStats()->GetPlan(), query);
                const TString plan{*explain.GetStats()->GetPlan()};
                NJson::TJsonValue json;
                UNIT_ASSERT_C(NJson::ReadJsonTree(plan, &json, true), plan);
                const auto& simplified = json["SimplifiedPlan"];
                UNIT_ASSERT_C(simplified.IsMap(), plan);
                const auto index = FindPlanNodeByKv(simplified, "Table", "SelectedUniqueRows/by_fu/indexImplTable");
                UNIT_ASSERT_C(index.IsDefined(), plan);
                UNIT_ASSERT_VALUES_EQUAL_C(index["Index"].GetStringSafe(), "by_fu", plan);
                UNIT_ASSERT_VALUES_EQUAL_C(index["Name"].GetStringSafe(), "TableRangeScan", plan);
                const auto& rangeKeys = index["ReadRangesKeys"].GetArraySafe();
                UNIT_ASSERT_VALUES_EQUAL_C(rangeKeys.size(), 1, plan);
                UNIT_ASSERT_VALUES_EQUAL_C(rangeKeys[0].GetStringSafe(), "Flag", plan);
                const auto lookup = FindPlanNodeByKv(simplified, "Name", "TableLookup");
                UNIT_ASSERT_VALUES_EQUAL_C(lookup.IsDefined(), fetch, plan);
                if (fetch) {
                    UNIT_ASSERT_VALUES_EQUAL_C(lookup["Table"].GetStringSafe(), "/Root/SelectedUniqueRows", plan);
                }

                const auto result = querySession.ExecuteQuery(query, NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString() << "\n" << plan);
                UNIT_ASSERT_VALUES_EQUAL_C(failures->Val(), failuresBefore, query);
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                const auto& resultSet = result.GetResultSet(0);
                UNIT_ASSERT(!resultSet.Truncated());
                const auto& columns = resultSet.GetColumnsMeta();
                UNIT_ASSERT_VALUES_EQUAL(columns.size(), 3);
                UNIT_ASSERT_VALUES_EQUAL(columns[0].Name, "Id");
                UNIT_ASSERT_VALUES_EQUAL(columns[0].Type.ToString(), "Int32");
                UNIT_ASSERT_VALUES_EQUAL(columns[1].Name, "U");
                UNIT_ASSERT_VALUES_EQUAL(columns[1].Type.ToString(), "String?");
                UNIT_ASSERT_VALUES_EQUAL(columns[2].Name, "Value");
                UNIT_ASSERT_VALUES_EQUAL(columns[2].Type.ToString(), fetch ? "Int32?" : "String?");
                std::vector<i32> actualIds, expectedIds;
                TResultSetParser parser(resultSet);
                while (parser.TryNextRow()) {
                    const auto id = parser.ColumnParser(0).GetInt32();
                    const auto& row = expectedRow(id);
                    UNIT_ASSERT(parser.ColumnParser(1).GetOptionalString() == row.U);
                    if (fetch) {
                        UNIT_ASSERT(parser.ColumnParser(2).GetOptionalInt32() == row.Payload);
                    } else {
                        UNIT_ASSERT(parser.ColumnParser(2).GetOptionalString() == row.Cover);
                    }
                    actualIds.push_back(id);
                }
                for (const auto& row : expected) {
                    if (row.Flag) { expectedIds.push_back(row.Id); }
                }
                // SQL has no ORDER BY: normalize only for exact bag comparison.
                std::sort(actualIds.begin(), actualIds.end());
                std::sort(expectedIds.begin(), expectedIds.end());
                UNIT_ASSERT_VALUES_EQUAL_C(actualIds, expectedIds, query << "\n" << FormatResultSetYson(resultSet));
            }
        };
        checkStoredRows();
        checkReads(true);

        const auto rejected = insert({{5, true, "a", "conflict", 55}});
        UNIT_ASSERT_VALUES_EQUAL_C(rejected.GetStatus(), EStatus::PRECONDITION_FAILED, rejected.GetIssues().ToString());
        checkStoredRows();
        checkReads(false);

        const TRow anotherNull{6, true, std::nullopt, "c6", 66};
        const auto accepted = insert({anotherNull});
        UNIT_ASSERT_C(accepted.IsSuccess(), accepted.GetIssues().ToString());
        expected.push_back(anotherNull);
        checkStoredRows();
        checkReads(false);
    }

    Y_UNIT_TEST(ComputedDecimalIndexPrefixPreservesFullBags) {
        NKikimrConfig::TAppConfig appConfig;
        auto* service = appConfig.MutableTableServiceConfig();
        service->SetEnableNewRBO(true);
        service->SetEnableFallbackToYqlOptimizer(false);
        service->SetEnableFallbackOnDML(false);
        service->SetEnableFallbackOnMultipleStatements(false);
        service->SetDefaultLangVer(202501);
        service->SetDefaultCostBasedOptimizationLevel(0);
        service->SetEnableAutoIndexSelectionForIndexLookupJoin(false);
        service->SetEnableIndexStreamWrite(true);
        auto settings = TKikimrSettings(appConfig);
        settings.SetWithSampleTables(false);
        settings.FeatureFlags.SetEnableAddUniqueIndex(true);
        settings.FeatureFlags.SetEnableAccessToIndexImplTables(true);
        TKikimrRunner kikimr(settings);
        auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        auto querySession = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");

        for (const bool unique : {false, true}) {
            const TString name = unique ? "ComputedDecimalUnique" : "ComputedDecimalOrdinary";
            const TString table = TStringBuilder() << "/Root/" << name;
            const TString indexPath = TStringBuilder() << table << "/by_dt/indexImplTable";
            const TString schema = TStringBuilder()
                << "CREATE TABLE `" << table << "` ("
                << "Id Utf8 NOT NULL, D Decimal(7,0) NOT NULL, Tail String NOT NULL, "
                << "Payload Int32 NOT NULL, PRIMARY KEY (Id), INDEX by_dt GLOBAL "
                << (unique ? "UNIQUE " : "") << "SYNC ON (D, Tail));";
            const auto created = tableSession.ExecuteSchemeQuery(schema).GetValueSync();
            UNIT_ASSERT_C(created.IsSuccess(), schema << "\n" << created.GetIssues().ToString());

            struct TRow { std::string Id; const char* D; std::string Tail; i32 Payload; };
            const std::vector<TRow> rows{
                {"mi1", "-inf", "a", -10}, {"mi2", "-inf", "b", -11},
                {"zero", "0", "a", 0},
                {"f1", "7", "a", 70},
                // Ordinary business-key duplicates must survive as two identical
                // output rows. UNIQUE permits the same D with distinct Tail.
                {"f2", "7", unique ? "b" : "a", 70},
                {"f3", "7", unique ? "c" : "b", 71},
                {"pi1", "inf", "a", 100}, {"pi2", "inf", "b", 101},
                {"nan", "nan", "a", 999},
            };
            TValueBuilder values;
            values.BeginList();
            for (const auto& row : rows) {
                values.AddListItem().BeginStruct()
                    .AddMember("Id").Utf8(row.Id)
                    .AddMember("D").Decimal(TDecimalValue(row.D, 7, 0))
                    .AddMember("Tail").String(row.Tail)
                    .AddMember("Payload").Int32(row.Payload).EndStruct();
            }
            const auto insertParams = TParamsBuilder().AddParam("$rows", values.EndList().Build()).Build();
            const TString insertSql = TStringBuilder()
                << "PRAGMA YqlSelect = 'force'; "
                << "DECLARE $rows AS List<Struct<Id:Utf8,D:Decimal(7,0),Tail:String,Payload:Int32>>; "
                << "INSERT INTO `" << table << "` (Id, D, Tail, Payload) "
                << "SELECT Id, D, Tail, Payload FROM AS_TABLE($rows);";
            const auto inserted = querySession.ExecuteQuery(insertSql,
                NQuery::TTxControl::BeginTx().CommitTx(), insertParams).GetValueSync();
            UNIT_ASSERT_C(inserted.IsSuccess(), insertSql << "\n" << inserted.GetIssues().ToString());

            // Establish the exact stored base rows, including NaN. Otherwise an
            // empty NaN equality result could pass after a broken fixture write.
            const TString scanSql = TStringBuilder() << "PRAGMA YqlSelect = 'force'; "
                << "SELECT Id, D, Tail, Payload FROM `" << table << "`;";
            const auto stored = querySession.ExecuteQuery(scanSql,
                NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(stored.IsSuccess(), stored.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(stored.GetResultSets().size(), 1);
            const auto& storedSet = stored.GetResultSet(0);
            UNIT_ASSERT(!storedSet.Truncated());
            const std::vector<std::string> storedNames{"Id", "D", "Tail", "Payload"};
            const std::vector<std::string> storedTypes{"Utf8", "Decimal(7,0)", "String", "Int32"};
            UNIT_ASSERT_VALUES_EQUAL(storedSet.GetColumnsMeta().size(), storedNames.size());
            for (size_t i = 0; i < storedNames.size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL(storedSet.GetColumnsMeta()[i].Name, storedNames[i]);
                UNIT_ASSERT_VALUES_EQUAL(storedSet.GetColumnsMeta()[i].Type.ToString(), storedTypes[i]);
            }
            std::vector<std::string> storedIds, expectedIds;
            TResultSetParser storedParser(storedSet);
            while (storedParser.TryNextRow()) {
                const auto id = storedParser.ColumnParser(0).GetUtf8();
                const auto row = std::find_if(rows.begin(), rows.end(),
                    [&](const TRow& value) { return value.Id == id; });
                UNIT_ASSERT(row != rows.end());
                const auto decimal = storedParser.ColumnParser(1).GetDecimal();
                const TDecimalValue expectedDecimal(row->D, 7, 0);
                UNIT_ASSERT_VALUES_EQUAL(decimal.Low_, expectedDecimal.Low_);
                UNIT_ASSERT_VALUES_EQUAL(decimal.Hi_, expectedDecimal.Hi_);
                UNIT_ASSERT_VALUES_EQUAL(storedParser.ColumnParser(2).GetString(), row->Tail);
                UNIT_ASSERT_VALUES_EQUAL(storedParser.ColumnParser(3).GetInt32(), row->Payload);
                storedIds.push_back(id);
            }
            for (const auto& row : rows) { expectedIds.push_back(row.Id); }
            std::sort(storedIds.begin(), storedIds.end());
            std::sort(expectedIds.begin(), expectedIds.end());
            UNIT_ASSERT_VALUES_EQUAL(storedIds, expectedIds);

            // The complete physical key, including the base PK, is part of the
            // admitted range corridor. Payload must require a base-table lookup.
            const auto described = tableSession.DescribeTable(indexPath).GetValueSync();
            UNIT_ASSERT_C(described.IsSuccess(), described.GetIssues().ToString());
            const auto& description = described.GetTableDescription();
            const std::vector<std::string> physicalKey{"D", "Tail", "Id"};
            UNIT_ASSERT_VALUES_EQUAL(description.GetPrimaryKeyColumns(), physicalKey);
            const auto columns = description.GetTableColumns();
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 3);
            for (const auto& column : columns) {
                UNIT_ASSERT_C(column.Name == "D" || column.Name == "Tail" || column.Name == "Id", column.Name);
                UNIT_ASSERT_VALUES_EQUAL(column.Type.ToString(),
                    column.Name == "D" ? "Decimal(7,0)" : (column.Name == "Tail" ? "String" : "Utf8"));
            }

            const TString query = TStringBuilder()
                << "PRAGMA YqlSelect = 'force'; DECLARE $p AS Decimal(7,0); "
                << "SELECT Tail, Payload FROM `" << table << "` WHERE D = $p;";
            struct TCase { const char* Bound; std::vector<std::string> Ids; };
            const std::vector<TCase> cases{
                {"7", {"f1", "f2", "f3"}}, {"nan", {}},
                {"-inf", {"mi1", "mi2"}}, {"inf", {"pi1", "pi2"}}, {"8", {}},
            };
            bool first = true;
            for (const auto& testCase : cases) {
                const auto params = TParamsBuilder().AddParam("$p")
                    .Decimal(TDecimalValue(testCase.Bound, 7, 0)).Build().Build();
                const auto beforeSuccess = successes->Val();
                const auto beforeFailure = failures->Val();
                const auto explained = querySession.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params,
                    NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
                UNIT_ASSERT_C(explained.IsSuccess(), query << "\n" << explained.GetIssues().ToString());
                if (first) { UNIT_ASSERT_C(successes->Val() > beforeSuccess, query); }
                first = false;
                UNIT_ASSERT_VALUES_EQUAL_C(failures->Val(), beforeFailure, query);
                UNIT_ASSERT_C(explained.GetStats() && explained.GetStats()->GetPlan() && explained.GetStats()->GetAst(), query);
                const TString plan{*explained.GetStats()->GetPlan()};
                const TString ast{*explained.GetStats()->GetAst()};
                Cerr << "[COMPUTED-DECIMAL-INDEX-BEGIN] unique=" << unique << " bound=" << testCase.Bound
                     << "\n" << query << "\n" << plan << "\n" << ast << "\n[COMPUTED-DECIMAL-INDEX-END]" << Endl;
                NJson::TJsonValue json;
                UNIT_ASSERT_C(NJson::ReadJsonTree(plan, &json, true), plan);
                const auto& simplified = json["SimplifiedPlan"];
                UNIT_ASSERT_C(simplified.IsMap(), plan);
                const auto index = FindPlanNodeByKv(simplified, "Table", name + "/by_dt/indexImplTable");
                UNIT_ASSERT_C(index.IsDefined(), plan);
                UNIT_ASSERT_VALUES_EQUAL_C(index["Index"].GetStringSafe(), "by_dt", plan);
                UNIT_ASSERT_VALUES_EQUAL_C(index["Name"].GetStringSafe(), "TableRangeScan", plan);
                const auto& rangeKeys = index["ReadRangesKeys"].GetArraySafe();
                UNIT_ASSERT_VALUES_EQUAL_C(rangeKeys.size(), 1, plan);
                UNIT_ASSERT_VALUES_EQUAL_C(rangeKeys[0].GetStringSafe(), "D", plan);
                const auto lookup = FindPlanNodeByKv(simplified, "Name", "TableLookup");
                UNIT_ASSERT_C(lookup.IsDefined(), plan);
                UNIT_ASSERT_VALUES_EQUAL_C(lookup["Table"].GetStringSafe(), table, plan);

                // Require a computed range transaction feeding the actual row
                // source. A literal point, full scan or folded parameter is a
                // fixture failure, even if the result happens to be correct.
                UNIT_ASSERT_C(ast.Contains("(declare $p (DataType 'Decimal '7 '0))")
                    && ast.Contains("(Just $p)"), ast);
                UNIT_ASSERT_C(ast.Contains("RangeFinalize") && ast.Contains("RangeMultiply")
                    && ast.Contains("DqCnValue") && ast.Contains("'\"compute\""), ast);
                UNIT_ASSERT_C(!ast.Contains("KqlKeyInc") && !ast.Contains("KqlKeyExc"), ast);
                const auto sourceBegin = ast.find("(KqpRowsSourceSettings ");
                UNIT_ASSERT_C(sourceBegin != TString::npos, ast);
                UNIT_ASSERT_C(ast.find("(KqpRowsSourceSettings ", sourceBegin + 1) == TString::npos, ast);
                const TString source = ast.substr(sourceBegin, ast.find('\n', sourceBegin) - sourceBegin);
                UNIT_ASSERT_C(source.Contains("%kqp_physical_tx_param_binding_"), ast);

                const auto result = querySession.ExecuteQuery(query,
                    NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString() << "\n" << ast);
                UNIT_ASSERT_VALUES_EQUAL_C(failures->Val(), beforeFailure, query);
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                const auto& rs = result.GetResultSet(0);
                UNIT_ASSERT(!rs.Truncated());
                const auto& resultColumns = rs.GetColumnsMeta();
                UNIT_ASSERT_VALUES_EQUAL(resultColumns.size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(resultColumns[0].Name, "Tail");
                UNIT_ASSERT_VALUES_EQUAL(resultColumns[0].Type.ToString(), "String");
                UNIT_ASSERT_VALUES_EQUAL(resultColumns[1].Name, "Payload");
                UNIT_ASSERT_VALUES_EQUAL(resultColumns[1].Type.ToString(), "Int32");
                std::vector<std::vector<std::string>> actual, expected;
                TResultSetParser parser(rs);
                while (parser.TryNextRow()) {
                    actual.push_back({parser.ColumnParser(0).GetString(),
                        std::to_string(parser.ColumnParser(1).GetInt32())});
                }
                for (const auto& id : testCase.Ids) {
                    const auto row = std::find_if(rows.begin(), rows.end(),
                        [&](const TRow& value) { return value.Id == id; });
                    UNIT_ASSERT(row != rows.end());
                    expected.push_back({row->Tail, std::to_string(row->Payload)});
                }
                std::sort(actual.begin(), actual.end());
                std::sort(expected.begin(), expected.end());
                UNIT_ASSERT_VALUES_EQUAL_C(actual, expected, "unique=" << unique << " bound=" << testCase.Bound
                    << "\n" << query << "\n" << FormatResultSetYson(rs) << "\n" << ast);
            }
        }
    }

    Y_UNIT_TEST(RequiredDecimalSetOperationsUseValueIdentity) {
        struct TCase {
            ui8 LeftPrecision, LeftScale, RightPrecision, RightScale;
            std::vector<std::string> Left, Right, Intersection, Difference;
        };
        // Exercise both input orientations after common-type alignment.
        // Set identity compares those converted values, including NaN peers;
        // the final public cast is an exact widening for these fixture values.
        const std::vector<TCase> cases{
            {7, 0, 7, 0,
                {"nan", "nan", "7", "7", "8", "inf", "-inf"},
                {"nan", "7", "inf", "-inf", "9"},
                {"nan", "7", "inf", "-inf"}, {"8"}},
            {7, 0, 7, 0,
                {"nan", "nan", "7", "7", "8", "inf", "-inf"},
                {"7", "inf", "-inf", "9"},
                {"7", "inf", "-inf"}, {"nan", "8"}},
            {7, 0, 10, 0,
                {"nan", "nan", "7", "7", "8", "inf", "-inf"},
                {"nan", "7", "-inf", "12345678"},
                {"nan", "7", "-inf"}, {"8", "inf"}},
            {10, 0, 7, 0,
                {"nan", "nan", "7", "7", "12345678", "inf", "-inf"},
                {"nan", "7", "inf", "-inf", "8"},
                {"nan", "7", "inf", "-inf"}, {"12345678"}},
            {7, 0, 10, 1,
                {"nan", "nan", "7", "7", "8", "1", "10", "inf", "-inf"},
                {"nan", "7.0", "7.5", "1.0", "10.5", "inf", "-inf", "9.0"},
                {"nan", "7", "1", "inf", "-inf"}, {"8", "10"}},
            {10, 1, 7, 0,
                {"nan", "nan", "7.0", "7.0", "7.5", "1.0", "10.5", "inf", "-inf", "9.0"},
                {"nan", "7", "8", "1", "10", "inf", "-inf"},
                {"nan", "7.0", "1.0", "inf", "-inf"}, {"7.5", "10.5", "9.0"}},
        };
        for (const std::string mode : {"grace", "block", "map"}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(true);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetEnableFallbackOnDML(false);
            service->SetEnableFallbackOnMultipleStatements(false);
            service->SetDefaultLangVer(202501);
            service->SetDefaultCostBasedOptimizationLevel(0);
            service->SetUseBlockHashJoin(mode == "block");
            TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
            const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
            const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
            const auto values = [](const std::vector<std::string>& input, ui8 precision, ui8 scale) {
                TValueBuilder result;
                result.BeginList();
                for (const auto& value : input) {
                    result.AddListItem().BeginStruct().AddMember("D")
                        .Decimal(TDecimalValue(value, precision, scale)).EndStruct();
                }
                return result.EndList().Build();
            };
            bool observedNewRbo = false;
            for (const auto& item : cases) {
                const auto params = TParamsBuilder()
                    .AddParam("$lhs", values(item.Left, item.LeftPrecision, item.LeftScale))
                    .AddParam("$rhs", values(item.Right, item.RightPrecision, item.RightScale)).Build();
                for (const bool difference : {false, true}) {
                    const TString query = TStringBuilder()
                        << "PRAGMA YqlSelect = 'force'; PRAGMA ydb.CostBasedOptimizationLevel = '0'; "
                        << "PRAGMA ydb.HashJoinMode = '" << (mode == "map" ? "map" : "grace") << "'; "
                        << "DECLARE $lhs AS List<Struct<D:Decimal(" << ui32(item.LeftPrecision) << ',' << ui32(item.LeftScale) << ")>>; "
                        << "DECLARE $rhs AS List<Struct<D:Decimal(" << ui32(item.RightPrecision) << ',' << ui32(item.RightScale) << ")>>; "
                        << "$set = (SELECT D FROM AS_TABLE($lhs) " << (difference ? "EXCEPT" : "INTERSECT")
                        << " SELECT D FROM AS_TABLE($rhs)); "
                        << "SELECT CAST(D AS Decimal(12,1)) AS D FROM $set;";
                    const auto beforeSuccess = successes->Val();
                    const auto beforeFailure = failures->Val();
                    const auto explained = session.ExecuteQuery(query, NQuery::TTxControl::NoTx(), params,
                        NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
                    UNIT_ASSERT_C(explained.IsSuccess(), query << "\n" << explained.GetIssues().ToString());
                    if (!observedNewRbo) {
                        UNIT_ASSERT(successes->Val() > beforeSuccess);
                        observedNewRbo = true;
                    }
                    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
                    UNIT_ASSERT(explained.GetStats() && explained.GetStats()->GetAst());
                    const TString ast{*explained.GetStats()->GetAst()};
                    const TString core = mode == "block" ? "BlockHashJoinCore" : mode == "map" ? "MapJoinCore" : "GraceJoinCore";
                    Cerr << "[DECIMAL-SET-IDENTITY] mode=" << mode << "\n" << query << "\n" << ast << Endl;
                    UNIT_ASSERT_C(ast.Contains(core) && ast.Contains("StablePickle"), ast);
                    UNIT_ASSERT_C(ast.Contains("(declare $lhs ") && ast.Contains("(declare $rhs "), ast);
                    const auto result = session.ExecuteQuery(query,
                        NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
                    UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString());
                    UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
                    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                    const auto& rs = result.GetResultSet(0);
                    UNIT_ASSERT(!rs.Truncated());
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 1);
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Name, "D");
                    UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Type.ToString(), "Decimal(12,1)");
                    std::vector<std::string> actual, expected;
                    TResultSetParser parser(rs);
                    while (parser.TryNextRow()) { actual.push_back(parser.ColumnParser(0).GetDecimal().ToString()); }
                    for (const auto& value : difference ? item.Difference : item.Intersection) {
                        expected.push_back(TDecimalValue(value, 12, 1).ToString());
                    }
                    std::sort(actual.begin(), actual.end());
                    std::sort(expected.begin(), expected.end());
                    UNIT_ASSERT_VALUES_EQUAL_C(actual, expected, query << "\n" << FormatResultSetYson(rs) << "\n" << ast);
                }
            }
        }
    }

    Y_UNIT_TEST_TWIN(SafeCastOptionalResults, PhysicalStagePeephole) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        appConfig.MutableTableServiceConfig()->SetEnableNewRBOPhysicalStagePeephole(PhysicalStagePeephole);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));

        const auto params = TParamsBuilder()
            .AddParam("$rows")
                .BeginList()
                    .AddListItem().BeginStruct()
                        .AddMember("value").Decimal(TDecimalValue("42", 7, 0))
                        .AddMember("number").Int32(42)
                        .AddMember("text").String("42")
                        .AddMember("nullable").OptionalInt32(42)
                    .EndStruct()
                    .AddListItem().BeginStruct()
                        .AddMember("value").Decimal(TDecimalValue("42", 7, 0))
                        .AddMember("number").Int32(-7)
                        .AddMember("text").String("invalid")
                        .AddMember("nullable").OptionalInt32(std::nullopt)
                    .EndStruct()
                .EndList()
            .Build()
            .Build();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(R"(
            DECLARE $rows AS List<Struct<value:Decimal(7,0),number:Int32,text:String,nullable:Int32?>>;
            SELECT
                CAST(value AS Decimal(7,0)?) AS value,
                CAST(number AS Decimal(7,0)?) AS numeric_decimal,
                CAST(number AS String?) AS numeric_string,
                CAST(text AS Decimal(7,0)?) AS parsed_decimal,
                CAST(value AS Decimal(7,0)??) AS nested_decimal,
                CAST(nullable AS String??) AS nested_string
            FROM AS_TABLE($rows) ORDER BY number DESC;
        )", NQuery::TTxControl::NoTx(), params).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        const auto& resultSet = result.GetResultSet(0);
        UNIT_ASSERT_VALUES_EQUAL(resultSet.RowsCount(), 2);
        const TVector<TString> types = {"Decimal(7,0)?", "Decimal(7,0)?", "String?",
            "Decimal(7,0)?", "Decimal(7,0)??", "String??"};
        for (size_t column = 0; column < types.size(); ++column) {
            UNIT_ASSERT_VALUES_EQUAL(resultSet.GetColumnsMeta()[column].Type.ToString(), types[column]);
        }
        TResultSetParser parser(resultSet);
        for (const bool present : {true, false}) {
            UNIT_ASSERT(parser.TryNextRow());
            const auto value = parser.ColumnParser(0).GetOptionalDecimal();
            UNIT_ASSERT(value);
            UNIT_ASSERT_VALUES_EQUAL(value->ToString(), "42");
            const auto numeric = parser.ColumnParser(1).GetOptionalDecimal();
            UNIT_ASSERT(numeric);
            UNIT_ASSERT_VALUES_EQUAL(numeric->ToString(), present ? "42" : "-7");
            const auto text = parser.ColumnParser(2).GetOptionalString();
            UNIT_ASSERT(text);
            UNIT_ASSERT_VALUES_EQUAL(*text, present ? "42" : "-7");
            const auto parsed = parser.ColumnParser(3).GetOptionalDecimal();
            UNIT_ASSERT_VALUES_EQUAL(parsed.has_value(), present);
            if (parsed) {
                UNIT_ASSERT_VALUES_EQUAL(parsed->ToString(), "42");
            }
            auto& nestedDecimal = parser.ColumnParser(4);
            nestedDecimal.OpenOptional();
            UNIT_ASSERT(!nestedDecimal.IsNull());
            const auto decimal = nestedDecimal.GetOptionalDecimal();
            UNIT_ASSERT(decimal);
            UNIT_ASSERT_VALUES_EQUAL(decimal->ToString(), "42");
            auto& nestedString = parser.ColumnParser(5);
            nestedString.OpenOptional();
            UNIT_ASSERT(!nestedString.IsNull());
            const auto string = nestedString.GetOptionalString();
            UNIT_ASSERT_VALUES_EQUAL(string.has_value(), present);
            if (string) {
                UNIT_ASSERT_VALUES_EQUAL(*string, "42");
            }
        }
    }

    Y_UNIT_TEST(ManyJoinsWithDecimalRanges) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        appConfig.MutableTableServiceConfig()->SetEnableNewRBOPhysicalStagePeephole(false);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false));

        // Filtering each joined table by a non-nullable decimal key produces
        // optional casts in the read ranges.
        constexpr ui32 JoinCount = 90;
        TStringBuilder schema;
        for (ui32 i = 0; i <= JoinCount; ++i) {
            schema << "CREATE TABLE `/Root/t" << i << "` ("
                << "scope Decimal(7,0) NOT NULL, key Uint64 NOT NULL, value String,"
                << "PRIMARY KEY (scope, key));\n";
        }
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto created = session.ExecuteSchemeQuery(schema).GetValueSync();
        UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());

        TStringBuilder query;
        query << "SELECT t0.key";
        for (ui32 i = 1; i <= JoinCount; ++i) {
            query << ", t" << i << ".value AS value" << i;
        }
        query << " FROM `/Root/t0` AS t0\n";
        for (ui32 i = 1; i <= JoinCount; ++i) {
            query << "LEFT JOIN (SELECT key, value FROM `/Root/t" << i
                << "` WHERE scope = CAST('0' AS Decimal(7,0))) AS t" << i
                << " ON t0.key = t" << i << ".key\n";
        }
        const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
        const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
        const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
        const auto successesBefore = successes->Val();
        const auto failuresBefore = failures->Val();
        const auto result = kikimr.GetQueryClient().ExecuteQuery(query, NQuery::TTxControl::NoTx(),
            NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT(result.GetStats() && result.GetStats()->GetPlan());
        UNIT_ASSERT(!result.GetStats()->GetPlan()->empty());
        UNIT_ASSERT_VALUES_EQUAL(successes->Val(), successesBefore + 1);
        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), failuresBefore);
    }
}

} // namespace
} // namespace NKikimr::NKqp
