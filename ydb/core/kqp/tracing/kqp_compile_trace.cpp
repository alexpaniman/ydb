#include "kqp_compile_trace.h"

#include <ydb/library/actors/core/actorsystem.h>

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
    return NWilson::TTraceId(Parents.empty() ? Root : Parents.back());
}

NWilson::TSpan TCompileTrace::StartSpan(ui8 verbosity, TString name, const char* component) const {
    if (!IsEnabled(verbosity)) {
        return {};
    }
    NWilson::TSpan span(verbosity, Parent(), std::move(name), NWilson::EFlags::NONE, ActorSystem);
    if (span && component) {
        span.Attribute("ydb.code.component", TString(component));
    }
    return span;
}

void TCompileTrace::Push(const NWilson::TSpan& span) {
    if (span) {
        Parents.emplace_back(span.GetTraceId());
    }
}

void TCompileTrace::Pop(const NWilson::TSpan& span) {
    if (!span) {
        return;
    }
    // Asynchronous continuations may interleave sibling spans, so the span is
    // not necessarily the innermost one.
    const auto id = span.GetTraceId();
    const auto it = std::find(Parents.rbegin(), Parents.rend(), id);
    if (it != Parents.rend()) {
        Parents.erase(std::next(it).base());
    }
}

void TCompileTrace::AddFinishedSpan(ui8 verbosity, const TString& name, TInstant start, TInstant end,
        std::initializer_list<std::pair<const char*, i64>> attributes) const {
    if (!IsEnabled(verbosity)) {
        return;
    }
    const auto parent = Parent();
    auto span = NWilson::TSpan::ConstructTerminated(parent, parent.Span(verbosity), start, end,
        NWilson::NTraceProto::Status::STATUS_CODE_OK, name);
    if (ActorSystem) {
        span.Attribute("node_id", ActorSystem->NodeId);
    }
    for (const auto& [key, value] : attributes) {
        span.Attribute(key, value);
    }
    span.End();
}

void TCompileTrace::BeginRegion(ui8 verbosity, const TString& name, const char* component, const void* group) {
    for (size_t i = 0; i < Regions.size(); ++i) {
        if (Regions[i].Group != group) {
            continue;
        }
        if (Regions[i].Name == name) {
            return;
        }
        EndRegions(Regions.size() - i, true);
        break;
    }
    auto span = StartSpan(verbosity, name, component);
    if (!span) {
        return;
    }
    Push(span);
    Regions.push_back({name, group, std::move(span)});
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
        auto& span = Regions.back().Span;
        Pop(span);
        if (success) {
            span.EndOk();
        } else {
            span.EndError("Compilation failed");
        }
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
    }
}

TCompileTraceScope::~TCompileTraceScope() {
    if (Trace) {
        Trace->Pop(Span_);
    }
    // On stack unwinding the span ends itself with an error.
    if (std::uncaught_exceptions() == UncaughtExceptions) {
        Span_.EndOk();
    }
}

} // namespace NKikimr::NKqp
