#pragma once

#include <ydb/library/actors/wilson/wilson_span.h>
#include <ydb/library/wilson_ids/wilson.h>

#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <utility>

namespace NKikimr::NKqp {

struct TCompileTraceLevels {
    enum : ui8 {
        // Compilation phases: parsing, annotation, optimization, physical query compilation.
        Phase = TComponentTracingLevels::TQueryProcessor::Detailed,
        // Steps of a phase: RBO stages, physical conversion steps.
        Step = TComponentTracingLevels::TQueryProcessor::Diagnostic,
        // Applied RBO rules and recomputed plan properties.
        Detail = TComponentTracingLevels::TQueryProcessor::Trace,
    };
};

// Spans of a single query compilation. Compilation runs on one thread and its
// phases nest in time, so the innermost open span is the parent of a new one.
// Spans may stay open across asynchronous waits of the compilation pipeline.
class TCompileTrace : public TThrRefBase {
public:
    using TPtr = TIntrusivePtr<TCompileTrace>;

    TCompileTrace(NWilson::TTraceId root, NActors::TActorSystem* actorSystem);
    ~TCompileTrace();

    bool IsEnabled(ui8 verbosity) const {
        return Root && verbosity <= Root.GetVerbosity();
    }

    NWilson::TSpan StartSpan(ui8 verbosity, TString name, const char* component) const;

    // Makes the span the parent of the spans started until it is popped.
    void Push(const NWilson::TSpan& span);
    void Pop(const NWilson::TSpan& span);

    // Records an already finished span under the current parent.
    void AddFinishedSpan(ui8 verbosity, const TString& name, TInstant start, TInstant end,
        std::initializer_list<std::pair<const char*, i64>> attributes = {}) const;

    // A region is a span opened and closed by separate calls, e.g. by marker
    // stages placed around groups of transformers of a pipeline. Regions of one
    // group follow each other: opening a region ends the open regions of its
    // group, unless it is open itself (after a restart of the pipeline).
    void BeginRegion(ui8 verbosity, const TString& name, const char* component, const void* group);
    // Ends the region together with the regions opened inside it.
    void EndRegion(TStringBuf name);

    // Ends the regions that remain open, e.g. after a failed transformer.
    void Finish(bool success);

private:
    NWilson::TTraceId Parent() const;
    void EndRegions(size_t count, bool success);

private:
    const NWilson::TTraceId Root;
    NActors::TActorSystem* const ActorSystem;
    TVector<NWilson::TTraceId> Parents;
    struct TRegion {
        TString Name;
        const void* Group;
        NWilson::TSpan Span;
    };
    TVector<TRegion> Regions;
};

// Span that is open for the lifetime of the scope and parents the spans started in it.
class TCompileTraceScope {
public:
    TCompileTraceScope(TCompileTrace* trace, ui8 verbosity, TString name, const char* component);
    ~TCompileTraceScope();

    TCompileTraceScope(const TCompileTraceScope&) = delete;
    TCompileTraceScope& operator=(const TCompileTraceScope&) = delete;

    NWilson::TSpan& Span() {
        return Span_;
    }

private:
    TCompileTrace* const Trace;
    const int UncaughtExceptions;
    NWilson::TSpan Span_;
};

} // namespace NKikimr::NKqp
