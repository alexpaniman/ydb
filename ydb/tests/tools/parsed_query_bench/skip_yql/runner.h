#pragma once

#include "../measurement.h"

#include <functional>
#include <memory>

namespace Ydb::Query::Benchmark {
class ExecuteQueryRequest;
}

namespace NKikimr::NKqp::NParsedQueryBench {

struct TCapture {
    TMeasurementTime Done;
    TString Signature;
};

class TRunner {
public:
    explicit TRunner(const TString& catalogPath);
    ~TRunner();

    void WithSession(const std::function<void()>& run);
    TCapture Run(const Ydb::Query::Benchmark::ExecuteQueryRequest& request, bool verify);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};

} // namespace NKikimr::NKqp::NParsedQueryBench
