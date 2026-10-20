#include "kqp_compile_trace.h"

#include <ydb/library/actors/core/actorsystem.h>

#include <util/datetime/cputimer.h>
#include <util/system/datetime.h>
#include <util/system/guard.h>
#include <util/system/thread.h>

#include <algorithm>

namespace NKikimr::NKqp {

TCompileTrace::TCompileTrace(NWilson::TTraceId root, NActors::TActorSystem* actorSystem)
    : Root(std::move(root))
    , ActorSystem(actorSystem)
{}

TCompileTrace::~TCompileTrace() {
    Finish(true);
}

NWilson::TTraceId TCompileTrace::Parent() const {
    return NWilson::TTraceId(Parents.empty() ? Root : Parents.back().Id);
}

NWilson::TTraceId TCompileTrace::GetCurrentParent() const {
    auto guard = Guard(ParentsLock);
    return Parent();
}

NWilson::TSpan TCompileTrace::StartSpan(ui8 verbosity, TString name, const char* component) const {
    if (!IsEnabled(verbosity)) {
        return {};
    }
    NWilson::TSpan span(verbosity, Parent(), std::move(name), NWilson::EFlags::NONE, ActorSystem);
    if (span) {
        if (component) {
            span.Attribute("ydb.code.component", TString(component));
        }
        span.Attribute("thread.id", static_cast<i64>(TThread::CurrentThreadNumericId()));
    }
    return span;
}

void TCompileTrace::EndSpan(NWilson::TSpan& span, ui64 cpuStart, bool success) const {
    if (!span) {
        return;
    }
    if (CpuCounted) {
        span.Attribute("ydb.cpu_us", static_cast<i64>(GetCpuTime() - cpuStart));
    }
    if (success) {
        span.EndOk();
    } else {
        span.EndError("Compilation failed");
    }
}

void TCompileTrace::Push(const NWilson::TSpan& span) {
    if (span) {
        TParent parent{span.GetTraceId(), TInstant::Now(), GetCycleCount()};
        auto guard = Guard(ParentsLock);
        Parents.push_back(std::move(parent));
    }
}

void TCompileTrace::Pop(const NWilson::TSpan& span) {
    if (!span) {
        return;
    }
    // Asynchronous continuations may interleave sibling spans, so the span is
    // not necessarily the innermost one.
    const auto id = span.GetTraceId();
    auto guard = Guard(ParentsLock);
    const auto it = std::find_if(Parents.rbegin(), Parents.rend(), [&](const TParent& parent) {
        return parent.Id == id;
    });
    if (it != Parents.rend()) {
        Parents.erase(std::next(it).base());
    }
}

TInstant TCompileTrace::Now() const {
    if (Parents.empty()) {
        return TInstant::Now();
    }
    const auto& parent = Parents.back();
    return parent.Start + CyclesToDuration(GetCycleCount() - parent.StartCycles);
}

NWilson::TSpan TCompileTrace::StartFinishedSpan(ui8 verbosity, const TString& name, TInstant start, TInstant end) const {
    if (!IsEnabled(verbosity)) {
        return {};
    }
    const auto parent = Parent();
    auto span = NWilson::TSpan::ConstructTerminated(parent, parent.Span(verbosity), start, end,
        NWilson::NTraceProto::Status::STATUS_CODE_OK, name);
    if (ActorSystem) {
        span.Attribute("node_id", ActorSystem->NodeId);
    }
    span.Attribute("thread.id", static_cast<i64>(TThread::CurrentThreadNumericId()));
    return span;
}

ui32 TCompileTrace::NextIteration(const TString& name) {
    return ++Iterations[name];
}

ui64 TCompileTrace::GetCpuTime() const {
    return Running ? Cpu + ThreadCPUTime() - RunningSince : Cpu;
}

void TCompileTrace::Resume() {
    if (Running++ == 0) {
        RunningSince = ThreadCPUTime();
        CpuCounted = true;
    }
}

void TCompileTrace::Suspend() {
    if (Running > 0 && --Running == 0) {
        Cpu += ThreadCPUTime() - RunningSince;
    }
}

NWilson::TSpan* TCompileTrace::BeginRegion(ui8 verbosity, const TString& name, const char* component, const void* group) {
    // Opening a region the group has already opened means the pipeline
    // restarted inside the region that is open now.
    const bool restart = GroupRegions[group].contains(name);
    TString restartedIn;
    for (size_t i = 0; i < Regions.size(); ++i) {
        if (Regions[i].Group != group) {
            continue;
        }
        if (Regions[i].Name == name) {
            return nullptr;
        }
        if (restart) {
            restartedIn = Regions[i].Name;
            Regions[i].Span.Attribute("ydb.compile.restarted", true);
        }
        EndRegions(Regions.size() - i, true);
        break;
    }
    GroupRegions[group].insert(name);
    const ui32 iteration = NextIteration(name);
    auto span = StartSpan(verbosity, name, component);
    if (!span) {
        return nullptr;
    }
    span.Attribute("ydb.compile.iteration", static_cast<i64>(iteration));
    if (!restartedIn.empty()) {
        span.Attribute("ydb.compile.restart_from", restartedIn);
    }
    Push(span);
    Regions.push_back({name, group, std::move(span), GetCpuTime()});
    return &Regions.back().Span;
}

void TCompileTrace::EndRegion(TStringBuf name) {
    for (size_t i = Regions.size(); i > 0; --i) {
        if (Regions[i - 1].Name == name) {
            EndRegions(Regions.size() - i + 1, true);
            return;
        }
    }
}

void TCompileTrace::EndRegions(size_t count, bool success) {
    for (; count > 0; --count) {
        auto& region = Regions.back();
        Pop(region.Span);
        EndSpan(region.Span, region.CpuStart, success);
        Regions.pop_back();
    }
}

void TCompileTrace::Finish(bool success) {
    EndRegions(Regions.size(), success);
}

TCompileTraceScope::TCompileTraceScope(TCompileTrace* trace, ui8 verbosity, TString name, const char* component)
    : Trace(trace && trace->IsEnabled(verbosity) ? trace : nullptr)
    , UncaughtExceptions(std::uncaught_exceptions())
{
    if (Trace) {
        Span_ = Trace->StartSpan(verbosity, std::move(name), component);
        Trace->Push(Span_);
        CpuStart = Trace->GetCpuTime();
    }
}

TCompileTraceScope::~TCompileTraceScope() {
    if (!Trace) {
        return;
    }
    Trace->Pop(Span_);
    Trace->EndSpan(Span_, CpuStart, Success && std::uncaught_exceptions() == UncaughtExceptions);
}

TCompileTraceRunScope::TCompileTraceRunScope(TCompileTrace* trace)
    : Trace(trace)
{
    if (Trace) {
        Trace->Resume();
    }
}

TCompileTraceRunScope::~TCompileTraceRunScope() {
    if (Trace) {
        Trace->Suspend();
    }
}

} // namespace NKikimr::NKqp
