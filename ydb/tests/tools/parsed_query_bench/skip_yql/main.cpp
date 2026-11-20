#include "runner.h"

#include "../measurement.h"
#include "../ready_client.h"

#include <ydb/tests/tools/parsed_query_bench/proto/execute_query.pb.h>
#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/string/cast.h>

#include <array>
#include <iostream>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using TRequest = Ydb::Query::Benchmark::ExecuteQueryRequest;
enum class EStage { Text, LogicalPlan };
constexpr std::array<TStringBuf, 2> Names = {"text", "logical_plan"};

struct TInput {
    EStage Stage;
    TString Bytes;
};

TRequest Decode(const TInput& input) {
    TRequest request;
    DecodeRequest(input.Bytes, request);
    Y_ENSURE(request.exec_mode() == Ydb::Query::EXEC_MODE_EXECUTE, "Expected EXEC_MODE_EXECUTE");
    if (input.Stage == EStage::Text) {
        Y_ENSURE(request.has_query_content() && request.query_content().syntax() == Ydb::Query::SYNTAX_YQL_V1,
            "Expected YQL v1 text");
    } else {
        Y_ENSURE(request.has_parsed_query() && request.parsed_query().has_logical_plan(), "Expected logical_plan");
    }
    return request;
}

TInput Read(EStage stage, const char* path) {
    TFile file(path, OpenExisting | RdOnly);
    const auto size = file.GetLength();
    Y_ENSURE(size >= 0 && static_cast<ui64>(size) <= MaxRequestBytes, "Input exceeds request size limit");
    return {stage, TFileInput(file).ReadAll()};
}

void Verify(TRunner& runner, const std::array<TInput, 2>& inputs) {
    auto sql = Decode(inputs[0]);
    auto plan = Decode(inputs[1]);
    auto sqlEnvelope = sql;
    auto planEnvelope = plan;
    sqlEnvelope.clear_query();
    planEnvelope.clear_query();
    Y_ENSURE(google::protobuf::util::MessageDifferencer::Equals(sqlEnvelope, planEnvelope), "Request envelopes differ");

    TCapture textRoots, planRoots;
    runner.WithSession([&] { textRoots = runner.Run(sql, true); });
    runner.WithSession([&] { planRoots = runner.Run(plan, true); });
    Y_ENSURE(!textRoots.Signature.empty() && textRoots.Signature == planRoots.Signature,
        "Native logical roots differ between sql_text and logical_plan");
}

void Bench(TRunner& runner, const std::array<TInput, 2>& inputs, size_t warmup, size_t repeats) {
    std::cout << "stage,iteration,bytes,total_wall_ns,total_cpu_ns\n";
    for (size_t round = 0; round < warmup + repeats; ++round) {
        for (size_t slot = 0; slot < inputs.size(); ++slot) {
            const auto index = (round + slot) % inputs.size();
            const auto& input = inputs[index];
            TMeasurementTime elapsed;
            runner.WithSession([&] {
                const auto start = TMeasurementTime::Now();
                const auto request = Decode(input);
                const auto done = runner.Run(request, false).Done;
                elapsed = {done.Wall - start.Wall, done.Cpu - start.Cpu};
            });
            if (round >= warmup) {
                std::cout << Names[index] << ',' << round - warmup << ',' << input.Bytes.size()
                          << ',' << elapsed.Wall << ',' << elapsed.Cpu << '\n';
            }
        }
    }
}

int Run(int argc, const char** argv) {
    Y_ENSURE(argc >= 4 && TStringBuf(argv[1]) == "--catalog",
        "Usage: --catalog CATALOG {--verify | --bench WARMUP REPEATS | --client-ready WARMUP REPEATS} TEXT.pb LOGICAL_PLAN.pb");
    const TStringBuf mode(argv[3]);
    const bool verify = mode == "--verify";
    Y_ENSURE((verify && argc == 6) || ((mode == "--bench" || mode == "--client-ready") && argc == 8),
        "Invalid mode or number of arguments");
    const auto warmup = verify ? 0 : FromString<size_t>(argv[4]);
    const auto repeats = verify ? 1 : FromString<size_t>(argv[5]);
    Y_ENSURE(warmup <= 100000 && repeats && repeats <= 100000, "Invalid iteration count");
    const std::array<TInput, 2> inputs = {Read(EStage::Text, argv[argc - 2]), Read(EStage::LogicalPlan, argv[argc - 1])};
    if (mode == "--client-ready") {
        Decode(inputs[0]);
        Decode(inputs[1]);
        return RunReadyClient(warmup, repeats, inputs, Names);
    }
    TRunner runner(argv[2]);
    Verify(runner, inputs);
    if (verify) {
        std::cout << "sql_text and logical_plan matched at the native RBO boundary\n";
    } else {
        Bench(runner, inputs, warmup, repeats);
    }
    return 0;
}

} // namespace
} // namespace NKikimr::NKqp::NParsedQueryBench

int main(int argc, const char** argv) {
    try {
        return NKikimr::NKqp::NParsedQueryBench::Run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
