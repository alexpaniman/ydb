#pragma once

#include <ydb/library/actors/wilson/wilson_span.h>
#include <ydb/library/wilson_ids/wilson.h>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/system/spinlock.h>

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
//
// Spans ended by the trace carry the CPU time the compilation spent while they
// were open (ydb.cpu_us). It is counted only while the compilation runs, see
// Resume and Suspend, since after an asynchronous wait it may continue on
// another thread.
class TCompileTrace : public TThrRefBase {
public:
    using TPtr = TIntrusivePtr<TCompileTrace>;

    TCompileTrace(NWilson::TTraceId root, NActors::TActorSystem* actorSystem);
    ~TCompileTrace();

    bool IsEnabled(ui8 verbosity) const {
        return Root && verbosity <= Root.GetVerbosity();
    }

    NWilson::TSpan StartSpan(ui8 verbosity, TString name, const char* component) const;
    // Ends the span, recording the CPU time of the compilation since cpuStart (see GetCpuTime).
    void EndSpan(NWilson::TSpan& span, ui64 cpuStart, bool success = true) const;

    // Makes the span the parent of the spans started until it is popped.
    void Push(const NWilson::TSpan& span);
    void Pop(const NWilson::TSpan& span);

    // Parent for spans that other components start during the compilation,
    // e.g. metadata requests of the gateway. Safe to call from any thread.
    NWilson::TTraceId GetCurrentParent() const;

    // Time on the clock of the innermost span. Wilson ends a span at its start
    // time plus the CPU cycles elapsed since, so spans recorded with these times
    // nest within it.
    TInstant Now() const;

    // Records an already finished span under the current parent. The caller
    // adds attributes and ends it.
    NWilson::TSpan StartFinishedSpan(ui8 verbosity, const TString& name, TInstant start, TInstant end) const;

    // Number of the next run of a span that the compilation may repeat, e.g.
    // after a restart of the pipeline, starting from 1.
    ui32 NextIteration(const TString& name);

    // CPU time of the compilation in microseconds, counted while it runs.
    ui64 GetCpuTime() const;
    // Called when the compilation starts and stops running on the current thread.
    void Resume();
    void Suspend();

    // A region is a span opened and closed by separate calls, e.g. by marker
    // stages placed around groups of transformers of a pipeline. Regions of one
    // group follow each other: opening a region ends the open regions of its
    // group, unless it is open itself (after a restart of the pipeline).
    // Returns the opened region span to add attributes to, if any.
    NWilson::TSpan* BeginRegion(ui8 verbosity, const TString& name, const char* component, const void* group);
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

    struct TParent {
        NWilson::TTraceId Id;
        TInstant Start;
        ui64 StartCycles;
    };
    TVector<TParent> Parents;
    // Guards Parents for GetCurrentParent.
    TAdaptiveLock ParentsLock;

    struct TRegion {
        TString Name;
        const void* Group;
        NWilson::TSpan Span;
        ui64 CpuStart;
    };
    TVector<TRegion> Regions;
    THashMap<TString, ui32> Iterations;
    // Names of the regions each group has opened.
    THashMap<const void*, THashSet<TString>> GroupRegions;

    ui64 Cpu = 0;
    ui64 RunningSince = 0;
    ui32 Running = 0;
    bool CpuCounted = false;
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
    ui64 CpuStart = 0;
};

// The compilation runs on the current thread for the lifetime of the scope.
class TCompileTraceRunScope {
public:
    explicit TCompileTraceRunScope(TCompileTrace* trace);
    ~TCompileTraceRunScope();

    TCompileTraceRunScope(const TCompileTraceRunScope&) = delete;
    TCompileTraceRunScope& operator=(const TCompileTraceRunScope&) = delete;

private:
    TCompileTrace* const Trace;
};

} // namespace NKikimr::NKqp
