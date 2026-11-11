// Performance contracts from PR #58587, fbeb81657ef. Preserve its SQL,
// storage choices, CBO override, warmups and query lists. This fixture measures
// the native pipeline; tools/bench_compilation compares immutable revisions.
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/library/actors/wilson/test_util/fake_wilson_uploader.h>
#include <yql/essentials/public/langver/yql_langver.h>
#include <library/cpp/json/json_reader.h>
#include <util/stream/file.h>
#include <util/string/hex.h>
#include <util/string/split.h>
#include <util/system/datetime.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <regex>
#include <set>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;

TString Data(const TString& path) {
    return TIFStream(SRC_("data/" + path)).ReadAll();
}

TString QueryText(const TString& benchmark, ui32 id) {
    return TString("PRAGMA ydb.CostBasedOptimizationLevel = '0';\n")
        + "$round = ($x,$y) -> {return $x;};\n"
        + "$to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };\n"
        + "$to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };\n"
        + Data("yql-" + benchmark + "/q" + ToString(id) + ".yql");
}

std::pair<ui64, ui64> CompileCounters(TKikimrRunner& runner) {
    TKqpCounters counters(runner.GetTestServer().GetRuntime()->GetAppData().Counters);
    return {counters.GetKqpCounters()->GetCounter("Compilation/NewRBO/Success")->Val(),
        counters.GetKqpCounters()->GetCounter("Compilation/NewRBO/Failed")->Val()};
}

void CheckCboDisabled(const TString& plan, const TString& traceId) {
    NJson::TJsonValue root;
    NJson::ReadJsonTree(plan, &root, true);
    const auto& stats = root.GetMapSafe().at("SimplifiedPlan").GetMapSafe().at("OptimizerStats").GetMapSafe();
    for (const TString name : {"CBOTreesTotal", "CBOTreesOptimized"}) {
        const auto it = stats.find(name);
        UNIT_ASSERT_C(it != stats.end(), "Missing " << name << " for " << traceId);
        UNIT_ASSERT_VALUES_EQUAL_C(it->second.GetUIntegerSafe(), 0, name << " for " << traceId);
    }
}

void SaveTrace(TTestActorRuntime& runtime, const NWilson::TTraceId& parent, const TString& name) {
    const TString traceId(static_cast<const char*>(parent.GetTraceIdPtr()), parent.GetTraceIdSize());
    const auto deadline = TInstant::Now() + TDuration::Seconds(30);
    while (TInstant::Now() < deadline) {
        auto promise = NThreading::NewPromise<std::vector<NWilson::TTraceSnapshot::TOtelSpan>>();
        runtime.GetActorSystem(0)->Send(NWilson::MakeWilsonUploaderId(),
            new NWilson::TFakeWilsonUploader::TEvGetSnapshot(promise));
        UNIT_ASSERT_C(promise.GetFuture().Wait(deadline - TInstant::Now()), name);
        NJson::TJsonValue spans(NJson::JSON_ARRAY);
        THashMap<TString, ui32> counts;
        for (const auto& span : promise.GetFuture().GetValueSync()) {
            if (span.trace_id() != traceId) continue;
            ++counts[span.name()];
            UNIT_ASSERT_C(span.end_time_unix_nano() >= span.start_time_unix_nano(), name << ": " << span.name());
            NJson::TJsonValue item;
            item["name"] = span.name();
            item["span_id"] = HexEncode(span.span_id());
            item["parent_span_id"] = HexEncode(span.parent_span_id());
            item["start_ns"] = span.start_time_unix_nano();
            item["end_ns"] = span.end_time_unix_nano();
            item["status"] = int(span.status().code());
            for (const auto& attribute : span.attributes())
                item["attributes"][attribute.key()] = attribute.value().DebugString();
            spans.AppendValue(std::move(item));
        }
        if (counts["Compile query"] && counts["Emit native physical plan"]) {
            for (const TString phase : {"Compile query", "Native parse", "Native table discovery",
                    "Native binding and scalar typing", "Native operator translation"})
                UNIT_ASSERT_VALUES_EQUAL_C(counts[phase], 1, name << ": " << phase);
            // Multi-result queries can have several RBO roots and emissions.
            UNIT_ASSERT_C(counts["New RBO optimization"] > 0, name);
            UNIT_ASSERT_VALUES_EQUAL_C(counts["Native frontend fallback"], 0, name);
            const auto path = GetOutputPath() / (name + ".trace.json");
            TFileOutput(path).Write(spans.GetStringRobust());
            Cout << "PERF_COMPILATION_TRACE," << name << ',' << path << Endl;
            return;
        }
        Sleep(TDuration::MilliSeconds(10));
    }
    UNIT_FAIL("Incomplete compilation trace: " << name);
}

void Measure(const TString& benchmark, ui32 id, ui32 iterations) {
    const bool tracing = !GetTestParam("PERF_COMPILATION_TRACE").empty();
    constexpr ui8 traceLevel = 15;
    NKikimrConfig::TAppConfig appConfig;
    if (tracing) {
        auto* external = appConfig.MutableTracingConfig()->AddExternalThrottling();
        external->SetLevel(traceLevel);
        external->SetMaxTracesPerMinute(1'000'000);
        external->SetMaxTracesBurst(1'000'000);
    }
    auto* config = appConfig.MutableTableServiceConfig();
    config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
    config->SetEnableNewRBO(true);
    config->SetEnableNewRBOPhysicalStagePeephole(false);
    config->SetEnableFallbackToYqlOptimizer(false);
    config->SetEnableFallbackOnMultipleStatements(false);
    config->SetAllowOlapDataQuery(true);
    config->SetDefaultCostBasedOptimizationLevel(4);
    config->SetDefaultLangVer(NYql::GetMaxLangVersion());
    config->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
    config->SetEnableAstCache(false);
    config->SetCompileQueryCacheSize(0);
    auto settings = TKikimrSettings(appConfig).SetWithSampleTables(false);
    settings.LogSettings = TTestLogSettings();
    settings.LogSettings->DefaultLogPriority = NActors::NLog::PRI_CRIT;
    TKikimrRunner runner(settings);
    auto& runtime = *runner.GetTestServer().GetRuntime();
    if (tracing) {
        const auto uploader = runtime.Register(new NWilson::TFakeWilsonUploader(), 0);
        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node)
            runtime.RegisterService(NWilson::MakeWilsonUploaderId(), uploader, node);
    }
    auto tableSession = runner.GetTableClient().CreateSession().GetValueSync().GetSession();
    std::string schema = std::string(Data("schema/" + benchmark + ".sql"));
    if (benchmark == "clickbench")
        schema = std::regex_replace(schema, std::regex(R"((CREATE TABLE [^\(]+\([^;]*\))\s*;)"),
            "$1 WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 16);");
    const auto created = tableSession.ExecuteSchemeQuery(TString(schema)).GetValueSync();
    UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());
    auto session = runner.GetQueryClient().GetSession().GetValueSync().GetSession();
    const auto query = QueryText(benchmark, id);
    constexpr ui32 warmups = 5;
    for (ui32 iteration = 0; iteration < warmups + iterations; ++iteration) {
        const bool warmup = iteration < warmups;
        const TString traceId = TStringBuilder() << "compilation-time-" << benchmark << '-' << id << "-native-"
            << (warmup ? TString("warmup-") + ToString(iteration) : ToString(iteration - warmups + 1));
        const auto before = CompileCounters(runner);
        NWilson::TTraceId traceParent;
        auto request = NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain).TraceId(std::string(traceId));
        if (tracing && !warmup) {
            traceParent = NWilson::TTraceId::NewTraceId(traceLevel, 4095).Span(traceLevel);
            request.TraceParent(traceParent.ToTraceresponseHeader());
        }
        const auto cpuStart = std::clock();
        const auto start = std::chrono::steady_clock::now();
        const auto result = session.ExecuteQuery(query, NQuery::TTxControl::NoTx(), request).ExtractValueSync();
        const auto wallUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        const double cpuUs = double(std::clock() - cpuStart) * 1'000'000 / CLOCKS_PER_SEC;
        UNIT_ASSERT_C(result.IsSuccess(), traceId << ": " << result.GetIssues().ToString());
        UNIT_ASSERT_C(result.GetStats() && result.GetStats()->GetPlan() && !result.GetStats()->GetPlan()->empty(), traceId);
        UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, traceId);
        CheckCboDisabled(TString(*result.GetStats()->GetPlan()), traceId);
        const auto after = CompileCounters(runner);
        UNIT_ASSERT_VALUES_EQUAL_C(after.first - before.first, 1, traceId);
        UNIT_ASSERT_VALUES_EQUAL_C(after.second, before.second, traceId);
        if (warmup) continue;
        if (tracing) SaveTrace(runtime, traceParent, traceId);
        Cout << "PERF_COMPILATION," << benchmark << ',' << id << ",native," << iteration - warmups + 1
            << ',' << wallUs << ',' << cpuUs << Endl;
    }
}

void RunBenchmark(const TString& benchmark, const TVector<ui32>& queries) {
    if (GetTestParam("ENABLE_PERF_COMPILATION").empty()) return;
    const auto pipeline = GetTestParam("PERF_COMPILATION_PIPELINES");
    UNIT_ASSERT_C(pipeline.empty() || pipeline == "native",
        "This fixture requires native compilation; use tools/bench_compilation for immutable before/after comparisons");
    ui32 iterations = 2;
    const auto count = GetTestParam("PERF_COMPILATION_ITERATIONS");
    if (!count.empty()) UNIT_ASSERT_C(TryFromString(count, iterations) && iterations > 0 && iterations <= 100,
        "PERF_COMPILATION_ITERATIONS must be between 1 and 100");
    std::set<ui32> selected;
    const auto selection = GetTestParam("PERF_COMPILATION_QUERIES");
    if (!selection.empty()) {
        TVector<TString> ids;
        Split(selection, ",", ids);
        for (const auto& text : ids) {
            ui32 id = 0;
            UNIT_ASSERT_C(TryFromString(text, id) && std::find(queries.begin(), queries.end(), id) != queries.end(),
                "Invalid query ID: " << text);
            selected.insert(id);
        }
    }
    Cout << "PERF_COMPILATION_CONFIG," << benchmark << ",storage=" << (benchmark == "clickbench" ? "column" : "row")
        << ",physical_stage_peephole=false,iterations=" << iterations << ",warmup=5,pipelines=native" << Endl;
    Cout << "PERF_COMPILATION_HEADER,benchmark,query,pipeline,iteration,explain_wall_us,process_cpu_us" << Endl;
    for (const auto id : queries) {
        if (!selected.empty() && !selected.contains(id)) continue;
        Measure(benchmark, id, iterations);
    }
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeCompilationTime) {
    Y_UNIT_TEST(CompilationTimeBench_TPCH) {
        TVector<ui32> queries;
        for (ui32 id = 1; id <= 22; ++id) queries.push_back(id);
        RunBenchmark("tpch", queries);
    }
    Y_UNIT_TEST(CompilationTimeBench_TPCDS) {
        RunBenchmark("tpcds", {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20,
            21, 22, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 35, 37, 38, 39, 40,
            41, 42, 43, 45, 46, 48, 49, 50, 52, 53, 54, 55, 56, 58, 59, 60,
            61, 62, 63, 64, 65, 66, 67, 68, 69, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80,
            81, 82, 83, 84, 85, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99});
    }
    Y_UNIT_TEST(CompilationTimeBench_CLICKBENCH) {
        RunBenchmark("clickbench", {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21,
            22, 23, 24, 25, 26, 27, 28, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 41, 42});
    }
}
} // namespace NKikimr::NKqp
