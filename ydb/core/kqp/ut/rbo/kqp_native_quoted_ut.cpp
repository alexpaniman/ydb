#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/opt/rbo/kqp_expression.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;

TKikimrSettings Settings(bool native) {
    NKikimrConfig::TAppConfig app;
    auto* config = app.MutableTableServiceConfig();
    config->SetEnableNewRBO(native);
    config->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                                        : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    config->SetEnableFallbackToYqlOptimizer(false);
    auto settings = TKikimrSettings(app).SetWithSampleTables(false);
    settings.LogSettings = TTestLogSettings();
    settings.LogSettings->DefaultLogPriority = NActors::NLog::PRI_CRIT;
    return settings;
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeQuotedRuntime) {
    Y_UNIT_TEST(NativeDiagnosticStringsAndPathsNeedNoBackendContext) {
        using namespace NFrontend;
        auto arena = std::make_shared<TExpressionArena>();
        TInfoUnitRegistry names;
        const auto check = [&](TExprId root, TStringBuf expected) {
            const auto size = arena->Size();
            const auto expression = TExpression::FromNative(arena, root);
            UNIT_ASSERT_VALUES_EQUAL(expression.ToString(), expected);
            UNIT_ASSERT_VALUES_EQUAL(expression.ToExplainString(names), expected);
            UNIT_ASSERT_VALUES_EQUAL(arena->Size(), size);
        };
        check(arena->Literal({ETypeKind::String}, std::string("text")), "\"text\"");
        check(arena->Literal({ETypeKind::String, true}, {}), "NULL");
        check(arena->Literal({ETypeKind::String}, std::string("\0\xff", 2)), "\"\\x00\\xFF\"");
        check(arena->Literal({ETypeKind::Utf8}, std::string("é")), "\"\\xC3\\xA9\"");
        check(arena->JsonValue(arena->Parameter("json", {ETypeKind::Json}), "$.key"), "JsonValue($json, \"$.key\")");
        check(arena->Like(arena->Parameter("text", {ETypeKind::String}),
            arena->Literal({ETypeKind::String}, std::string("a#%")), "#"), "Like($text, \"a#%\", \"#\")");
    }

    Y_UNIT_TEST_TWIN(AnsiQuotedNamesBindAndExecuteWithExactCase, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto schema = session.ExecuteSchemeQuery(
            "CREATE TABLE `/Root/Mixed` (Id Int64 NOT NULL, PRIMARY KEY(Id));").GetValueSync();
        UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
        const auto write = session.ExecuteDataQuery("UPSERT INTO `/Root/Mixed` (Id) VALUES (7);",
            NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(write.IsSuccess(), write.GetIssues().ToString());
        const auto result = session.ExecuteDataQuery(
            "--!ansi_lexer\nPRAGMA YqlSelect = 'force'; WITH \"c\" AS (SELECT \"Id\" FROM \"/Root/Mixed\") "
            "SELECT \"a\"\"b\".\"Id\" AS \"Result\", 'it''s' AS \"Text\" FROM \"c\" AS \"a\"\"b\";",
            NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
        const auto& columns = result.GetResultSet(0).GetColumnsMeta();
        UNIT_ASSERT_VALUES_EQUAL(columns.at(0).Name, "Result");
        UNIT_ASSERT_VALUES_EQUAL(columns.at(1).Name, "Text");
        TResultSetParser rows(result.GetResultSet(0));
        UNIT_ASSERT(rows.TryNextRow());
        UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser(0).GetInt64(), 7);
        UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser(1).GetString(), "it's");
        UNIT_ASSERT(!rows.TryNextRow());
        const auto invalid = session.ExecuteDataQuery("--!ansi_lexer\nSELECT \"id\" FROM \"/Root/Mixed\";",
            NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT(!invalid.IsSuccess());
        UNIT_ASSERT(invalid.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
    }

    Y_UNIT_TEST_TWIN(QuotedBytesAndUnicodeMatchLegacy, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        TString sql = "SELECT ";
        TVector<TString> expected;
        const auto add = [&](const TString& literal, const TString& value) {
            if (!expected.empty()) sql += ", ";
            sql += TStringBuilder() << literal << " AS v" << expected.size();
            expected.push_back(value);
        };
        constexpr TStringBuf hex = "0123456789abcdef";
        for (unsigned value = 0; value < 256; ++value) {
            add(TStringBuilder() << "'\\x" << hex[value >> 4] << hex[value & 15] << "'", TString(1, static_cast<char>(value)));
            add(TStringBuilder() << "'\\" << static_cast<char>('0' + (value >> 6)) <<
                static_cast<char>('0' + ((value >> 3) & 7)) << static_cast<char>('0' + (value & 7)) << "'", TString(1, static_cast<char>(value)));
        }
        add(R"('\a\b\f\n\r\t\v')", "\a\b\f\n\r\t\v");
        add(R"('\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff')", "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf");
        add(R"('\U00010000\U0010ffff')", "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf");
        add(R"('\q\4\?')", "q4?");
        const auto result = session.ExecuteDataQuery(sql, NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).ColumnsCount(), expected.size());
        TResultSetParser rows(result.GetResultSet(0));
        UNIT_ASSERT(rows.TryNextRow());
        for (size_t i = 0; i < expected.size(); ++i)
            UNIT_ASSERT_VALUES_EQUAL_C(rows.ColumnParser(i).GetString(), expected[i], i);
    }

    Y_UNIT_TEST_TWIN(MultilineAndAnsiQuotingMatchLegacy, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        for (const auto& [sql, expected] : TVector<std::pair<TString, TVector<TString>>>{
                {R"(SELECT 'a\n\x00b' AS a, @@one@@@@two@@ AS b, @@\x30@@ AS c)", {TString("a\n\0b", 4), "one@@two", R"(\x30)"}},
                {"--!ansi_lexer\nSELECT 'it''s' AS a, '\\x30' AS b", {"it's", R"(\x30)"}}}) {
            const auto result = session.ExecuteDataQuery(sql, NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
            TResultSetParser rows(result.GetResultSet(0));
            UNIT_ASSERT(rows.TryNextRow());
            for (size_t i = 0; i < expected.size(); ++i)
                UNIT_ASSERT_VALUES_EQUAL_C(rows.ColumnParser(i).GetString(), expected[i], sql);
        }
    }
}
} // namespace NKikimr::NKqp
