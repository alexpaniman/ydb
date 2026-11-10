#pragma once

#include <ydb/library/actors/wilson/wilson_span.h>
#include <ydb/library/wilson_ids/wilson.h>

#include <memory>
#include <utility>

namespace NKikimr::NKqp {

// One compilation owns this context. Synchronous scopes nest within each
// actor invocation; no open scope crosses an asynchronous suspension.
struct TCompileTrace {
    static constexpr ui8 Level = TComponentTracingLevels::TQueryProcessor::Detailed;
    NWilson::TTraceId Current;

    static std::shared_ptr<TCompileTrace> Create(const NWilson::TTraceId& parent) {
        if (!parent || parent.GetVerbosity() < Level) return {};
        return std::make_shared<TCompileTrace>(TCompileTrace{parent.Clone()});
    }
};

class TCompileTraceScope {
public:
    template <typename TName>
    TCompileTraceScope(const std::shared_ptr<TCompileTrace>& trace, TName&& name)
        : Trace(trace.get())
    {
        if (Trace) {
            Parent = Trace->Current.Clone();
            Span = NWilson::TSpan(TCompileTrace::Level, Parent.Clone(), std::forward<TName>(name), NWilson::EFlags::AUTO_END);
            Trace->Current = Span.GetTraceId().Clone();
        }
    }
    ~TCompileTraceScope() {
        if (Trace) Trace->Current = std::move(Parent);
    }
    TCompileTraceScope(const TCompileTraceScope&) = delete;
    TCompileTraceScope& operator=(const TCompileTraceScope&) = delete;
private:
    TCompileTrace* Trace;
    NWilson::TTraceId Parent;
    NWilson::TSpan Span;
};

} // namespace NKikimr::NKqp
