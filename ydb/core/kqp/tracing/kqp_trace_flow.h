#pragma once

#include <ydb/library/actors/wilson/wilson_span.h>

namespace NKikimr::NKqp {

// Spans form logical flows. A span runs within the flow of its parent and nests
// in it, unless it starts a flow of its own, e.g. a request served by another
// actor. A flow boundary does not imply that its parent makes progress in
// parallel: a parent can await the result. Links record where a flow
// consumes the outcome of another one. Viewers lay out one lane per flow.
inline constexpr TStringBuf FlowAttribute = "span.flow";
inline constexpr TStringBuf AsyncFlow = "async";
inline constexpr TStringBuf RelationAttribute = "span.link.relation";
// The span waited for the outcome of the linked span.
inline constexpr TStringBuf DependsOnRelation = "depends_on";
// The span started when the linked span completed.
inline constexpr TStringBuf FollowsFromRelation = "follows_from";

inline void MarkAsyncFlow(NWilson::TSpan& span) {
    if (span) {
        span.Attribute(TString(FlowAttribute), TString(AsyncFlow));
    }
}

inline void LinkDependsOn(NWilson::TSpan& span, const NWilson::TTraceId& dependency) {
    if (span && dependency) {
        span.Link(dependency, {{TString(RelationAttribute), TString(DependsOnRelation)}});
    }
}

inline void LinkFollowsFrom(NWilson::TSpan& span, const NWilson::TTraceId& predecessor) {
    if (span && predecessor) {
        span.Link(predecessor, {{TString(RelationAttribute), TString(FollowsFromRelation)}});
    }
}

} // namespace NKikimr::NKqp
