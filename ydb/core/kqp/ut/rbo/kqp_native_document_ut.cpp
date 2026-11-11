#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/opt/rbo/frontend/document.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <yql/essentials/minikql/dom/json.h>
#include <yql/essentials/minikql/dom/yson.h>
#include <yql/essentials/public/langver/yql_langver.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;
using namespace NFrontend;

struct TResultSnapshot {
    TString Values;
    TVector<std::pair<TString, TString>> Columns;
};
TResultSnapshot Snapshot(const TResultSet& rows) {
    TResultSnapshot result{FormatResultSetYson(rows), {}};
    for (const auto& column : rows.GetColumnsMeta()) result.Columns.emplace_back(column.Name, column.Type.ToString());
    return result;
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeDocuments) {
    Y_UNIT_TEST(ValidationAgreesWithExistingLiteralContract) {
        TString mismatches;
        for (const auto kind : {ETypeKind::Json, ETypeKind::Yson}) {
            const auto check = [&](std::string_view text) {
                const TStringBuf bytes(text.data(), text.size());
                const bool expected = kind == ETypeKind::Json ? NYql::NDom::IsValidJson(bytes) : NYql::NDom::IsValidYson(bytes);
                bool accepted = true;
                try { ValidateDocumentLiteral(kind, text, {}); }
                catch (const TFrontendError&) { accepted = false; }
                if (accepted != expected) mismatches += TStringBuilder() << NFrontend::TType{kind}.ToString()
                    << " bytes=" << TString(bytes).Quote() << " accepted=" << accepted << " expected=" << expected << '\n';
            };
            for (const auto& text : std::vector<std::string>{"", "{}", "[]", "null", "#", "%nan", "%inf", "1e400", "1e-400", "-0.0",
                    "18446744073709551615", "18446744073709551616", "-9223372036854775809", "18446744073709551615u",
                    R"({"k":null,"k":1})", R"({"k"=1;"k"=2;})", R"("\ud800")", R"("\ud83d\ude00")",
                    R"({"a":[1,2,null]})", R"(<meta=1;>{a=[1;2;#;];})", "<>#", "<><>#", "{} {}", "#;#",
                    std::string("\x01\x06" "abc", 5), std::string("\x01\x08" "abc", 5)}) check(text);
            for (unsigned byte = 0; byte < 256; ++byte) {
                check(std::string(1, static_cast<char>(byte)));
                check(std::string("\"") + static_cast<char>(byte) + "\"");
                check(std::string("{\"k\":") + static_cast<char>(byte) + "}");
                check(std::string("<k=") + static_cast<char>(byte) + ">#");
            }
        }
        UNIT_ASSERT_C(mismatches.empty(), mismatches);
    }

    Y_UNIT_TEST(EmissionPreservesTypedLiteralBytes) {
        for (const auto kind : {ETypeKind::Json, ETypeKind::Yson}) {
            TExpressionArena arena;
            const auto literal = arena.Literal({kind}, std::string("{}"));
            const auto before = arena.Size();
            NYql::TExprContext ctx;
            const auto emitted = LowerNativeLambda(arena, literal, ctx, {})->TailPtr();
            UNIT_ASSERT(emitted->IsCallable(TString(NFrontend::TType{kind}.ToString())));
            UNIT_ASSERT_VALUES_EQUAL(emitted->Head().Content(), "{}");
            UNIT_ASSERT_VALUES_EQUAL(emitted->GetTypeAnn()->Cast<NYql::TDataExprType>()->GetName(), NFrontend::TType{kind}.ToString());
            UNIT_ASSERT_VALUES_EQUAL(arena.Size(), before);
        }
    }

    Y_UNIT_TEST_TWIN(RuntimeLiteralsColumnsEffectsAndScalarSubqueries, Blocks) {
        const TVector<TString> queries{
            R"(SELECT '{}'j AS j, '<a=1>{k="v"}'y AS y, Json('{"x":1}') AS jc, Yson('#') AS yc)",
            R"(SELECT JSON_VALUE('{"k":"value"}'j, '$.k') AS text, JSON_VALUE('{"k":1}'j, '$.k') AS number)",
            "SELECT id, j, y FROM `/Root/Documents` ORDER BY id",
            "SELECT COUNT(j) AS j, COUNT(y) AS y FROM `/Root/Documents`",
            "SELECT (SELECT j FROM `/Root/Documents` WHERE id=1ul) AS j, (SELECT y FROM `/Root/Documents` WHERE id=1ul) AS y",
            "SELECT COALESCE(j, '{}'j) AS j, COALESCE(y, '#'y) AS y FROM `/Root/Documents` ORDER BY id",
            "SELECT (SELECT y FROM `/Root/Documents` WHERE id=99ul) AS missing"};
        TVector<TResultSnapshot> expected;
        for (const bool native : {false, true}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(native);
            service->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetDefaultLangVer(NYql::GetMaxLangVersion());
            auto settings = TKikimrSettings(config).SetWithSampleTables(false);
            settings.LogSettings = TTestLogSettings();
            settings.LogSettings->DefaultLogPriority = NActors::NLog::PRI_CRIT;
            TKikimrRunner runner(settings);
            auto session = runner.GetTableClient().CreateSession().GetValueSync().GetSession();
            const auto schema = session.ExecuteSchemeQuery(
                "CREATE TABLE `/Root/Documents` (id Uint64 NOT NULL, j Json, y Yson, PRIMARY KEY(id));").GetValueSync();
            UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
            const auto loaded = session.ExecuteDataQuery(
                R"(UPSERT INTO `/Root/Documents` (id,j,y) VALUES (1ul,'{"a":1}'j,'<m=1>{a=2;}'y),(2ul,NULL,NULL);)",
                NYdb::NTable::TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(loaded.IsSuccess(), loaded.GetIssues().ToString());
            UNIT_ASSERT(loaded.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
            const TString header = (native ? TString("PRAGMA YqlSelect='force'; ") : TString()) +
                "PRAGMA ydb.DisableBlockExecution='" + (Blocks ? "false" : "true") + "'; ";
            for (size_t i = 0; i < queries.size(); ++i) {
                const auto result = runner.GetQueryClient().ExecuteQuery(header + queries[i], NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), queries[i] << '\n' << result.GetIssues().ToString());
                UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                auto actual = Snapshot(result.GetResultSet(0));
                if (native) {
                    UNIT_ASSERT_VALUES_EQUAL_C(actual.Values, expected[i].Values, queries[i]);
                    UNIT_ASSERT_VALUES_EQUAL_C(actual.Columns, expected[i].Columns, queries[i]);
                } else expected.push_back(std::move(actual));
            }
            for (const TString sql : {"SELECT '[1}'j", "SELECT '<a=1>'y", "SELECT '1e400'j"}) {
                const auto result = runner.GetQueryClient().ExecuteQuery(header + sql, NQuery::TTxControl::NoTx()).GetValueSync();
                UNIT_ASSERT_C(!result.IsSuccess(), sql);
                UNIT_ASSERT(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos);
            }
        }
    }
}
} // namespace NKikimr::NKqp
