#include "kqp_compile_trace_transformer.h"

namespace NKikimr::NKqp {

using namespace NYql;

namespace {

class TCompileTraceTransformer : public TGraphTransformerBase {
public:
    TCompileTraceTransformer(TAutoPtr<IGraphTransformer>&& transformer, TIntrusivePtr<TKikimrQueryContext> queryCtx,
            ui8 verbosity, TString name, const char* component)
        : Transformer(std::move(transformer))
        , QueryCtx(std::move(queryCtx))
        , Verbosity(verbosity)
        , Name(std::move(name))
        , Component(component)
    {}

    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        auto* trace = QueryCtx->CompileTrace.Get();
        if (!Span && trace) {
            Span = trace->StartSpan(Verbosity, Name, Component);
        }
        return Traced(trace, [&] {
            return Transformer->Transform(input, output, ctx);
        });
    }

    NThreading::TFuture<void> DoGetAsyncFuture(const TExprNode& input) final {
        return Transformer->GetAsyncFuture(input);
    }

    TStatus DoApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        return Traced(QueryCtx->CompileTrace.Get(), [&] {
            return Transformer->ApplyAsyncChanges(input, output, ctx);
        });
    }

    void Rewind() final {
        Span.EndError("Transformer rewound");
        Transformer->Rewind();
    }

    TStatistics GetStatistics() const final {
        return Transformer->GetStatistics();
    }

private:
    template <class TFunc>
    TStatus Traced(TCompileTrace* trace, TFunc&& func) {
        if (trace) {
            trace->Push(Span);
        }
        TStatus status = TStatus::Error;
        try {
            status = func();
        } catch (...) {
            if (trace) {
                trace->Pop(Span);
            }
            throw;
        }
        if (trace) {
            trace->Pop(Span);
        }
        if (status == TStatus::Ok || status.HasRestart) {
            Span.EndOk();
        } else if (status == TStatus::Error) {
            Span.EndError("Transformer failed");
        }
        return status;
    }

private:
    TAutoPtr<IGraphTransformer> Transformer;
    const TIntrusivePtr<TKikimrQueryContext> QueryCtx;
    const ui8 Verbosity;
    const TString Name;
    const char* const Component;
    NWilson::TSpan Span;
};

class TCompileTraceMarker : public TSyncTransformerBase {
public:
    TCompileTraceMarker(TIntrusivePtr<TKikimrQueryContext> queryCtx, bool begin, ui8 verbosity, TString name,
            const char* component, const void* group)
        : QueryCtx(std::move(queryCtx))
        , Begin(begin)
        , Verbosity(verbosity)
        , Name(std::move(name))
        , Component(component)
        , Group(group)
    {}

    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext&) final {
        output = input;
        if (auto* trace = QueryCtx->CompileTrace.Get()) {
            if (Begin) {
                trace->BeginRegion(Verbosity, Name, Component, Group);
            } else {
                trace->EndRegion(Name);
            }
        }
        return TStatus::Ok;
    }

    void Rewind() final {
    }

private:
    const TIntrusivePtr<TKikimrQueryContext> QueryCtx;
    const bool Begin;
    const ui8 Verbosity;
    const TString Name;
    const char* const Component;
    const void* const Group;
};

} // namespace

TAutoPtr<IGraphTransformer> CreateCompileTraceTransformer(TAutoPtr<IGraphTransformer>&& transformer,
        TIntrusivePtr<TKikimrQueryContext> queryCtx, ui8 verbosity, TString name, const char* component) {
    return new TCompileTraceTransformer(std::move(transformer), std::move(queryCtx), verbosity, std::move(name), component);
}

TAutoPtr<IGraphTransformer> CreateCompileTraceBegin(TIntrusivePtr<TKikimrQueryContext> queryCtx,
        ui8 verbosity, TString name, const char* component, const void* group) {
    return new TCompileTraceMarker(std::move(queryCtx), true, verbosity, std::move(name), component, group);
}

TAutoPtr<IGraphTransformer> CreateCompileTraceEnd(TIntrusivePtr<TKikimrQueryContext> queryCtx, TString name) {
    return new TCompileTraceMarker(std::move(queryCtx), false, 0, std::move(name), nullptr, nullptr);
}

} // namespace NKikimr::NKqp
