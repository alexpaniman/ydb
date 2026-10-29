"""Render the Q01 general-frame MIN/MAX amendment without changing a checkout.

This is production-patch generation, not generated test source. No compiler or
native process is launched. The amendment remains uncompiled and unverified.
"""
import difflib
import subprocess

SOURCE_COMMIT = 'c4a3cd163ec32e76c2557469378f89f22da3b5fc'
PREFIX = 'ydb/core/kqp/'
CPP = PREFIX + 'opt/rbo/physical_conversion/kqp_rbo_physical_window_builder.cpp'
HEADER = CPP.removesuffix('.cpp') + '.h'
COMMON = PREFIX + 'common/kqp_window_minmax.h'
RUNTIME = PREFIX + 'runtime/kqp_range_minmax.cpp'
RUNTIME_HEADER = PREFIX + 'runtime/kqp_range_minmax.h'

SLOT_HEADER = r'''#pragma once

#include <yql/essentials/public/udf/udf_data_type.h>

namespace NKikimr::NKqp {

// These scalar selectors have the same total order in AggrMin/AggrMax and
// NUdf::CompareValues. Floating, timezone and composite values are excluded.
inline bool IsIncrementalWindowMinMaxSlot(NYql::NUdf::EDataSlot slot) {
    using NYql::NUdf::EDataSlot;
    switch (slot) {
        case EDataSlot::Bool:
        case EDataSlot::Int8: case EDataSlot::Uint8:
        case EDataSlot::Int16: case EDataSlot::Uint16:
        case EDataSlot::Int32: case EDataSlot::Uint32:
        case EDataSlot::Int64: case EDataSlot::Uint64:
        case EDataSlot::Decimal:
        case EDataSlot::Interval: case EDataSlot::Interval64:
        case EDataSlot::Date: case EDataSlot::Datetime: case EDataSlot::Timestamp:
        case EDataSlot::Date32: case EDataSlot::Datetime64: case EDataSlot::Timestamp64:
            return true;
        default:
            return false;
    }
}

} // namespace NKikimr::NKqp
'''

NODE_HEADER = r'''#pragma once

#include <yql/essentials/minikql/computation/mkql_computation_node.h>

namespace NKikimr::NMiniKQL {

IComputationNode* WrapKqpRangeMinMax(TCallable& callable, const TComputationNodeFactoryContext& ctx);

} // namespace NKikimr::NMiniKQL
'''

NODE = r'''#include "kqp_range_minmax.h"

#include <ydb/core/kqp/common/kqp_window_minmax.h>

#include <yql/essentials/minikql/comp_nodes/mkql_window_frames_collector_params_deserializer.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_core_win_frames_collector.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/public/udf/udf_type_ops.h>

#include <util/generic/hash_set.h>

#include <deque>
#include <limits>

namespace NKikimr::NMiniKQL {
namespace {

struct TColumn {
    ui32 Input;
    ui32 Output;
    NUdf::EDataSlot Slot;
    bool Minimum;
    bool OptionalOutput;
};

using TCollector = TCoreWinFramesCollector<NUdf::TUnboxedValue, TComputationContext, true>;
using TFactory = decltype(TCollector::CreateFactory(TCollector::TComparatorBounds{}));

class TRangeMinMaxWrapper: public TMutableComputationNode<TRangeMinMaxWrapper> {
    using TBase = TMutableComputationNode<TRangeMinMaxWrapper>;

    struct TEntry {
        ui64 Ordinal;
        NUdf::TUnboxedValue Value;
    };
    using TDeque = std::deque<TEntry, TMKQLAllocator<TEntry>>;

    class TStreamValue: public TComputationValue<TStreamValue> {
    public:
        TStreamValue(TMemoryUsageInfo* memInfo, TComputationContext& ctx,
                     NUdf::TUnboxedValue input, const TRangeMinMaxWrapper* owner)
            : TComputationValue(memInfo)
            , Ctx(ctx)
            , Input(std::move(input))
            , Owner(owner)
            , Alloc(TlsAllocState)
            , Buffer(Nothing(), NUdf::TUnboxedValue())
            , Collector(Owner->Factory(Buffer,
                [this](NUdf::TUnboxedValue& row) { return Consume(row); }, Bounds, Ctx))
            , Candidates(Owner->Columns.size())
        {
        }

        ~TStreamValue() override {
            Buffer.Clear();
            Alloc->OffloadFree(BufferBytes);
        }

    private:
        void ReserveOne() {
            if (Buffer.Size() < Buffer.Capacity()) {
                return;
            }
            const auto capacity = Buffer.Capacity();
            MKQL_ENSURE(capacity <= std::numeric_limits<size_t>::max() / (2 * sizeof(NUdf::TUnboxedValue)),
                        "Window buffer is too large");
            const auto next = std::max<size_t>(2, capacity * 2);
            const auto bytes = next * sizeof(NUdf::TUnboxedValue);
            // The public circular buffer uses std::vector. Charge both old and
            // new storage before growth, including its transient allocation.
            Alloc->OffloadAlloc(bytes);
            try {
                Buffer.Reserve(next);
            } catch (...) {
                Alloc->OffloadFree(bytes);
                throw;
            }
            Alloc->OffloadFree(BufferBytes);
            BufferBytes = bytes;
        }

        EConsumeStatus Consume(NUdf::TUnboxedValue& row) {
            switch (Input.Fetch(row)) {
                case NUdf::EFetchStatus::Ok:
                    ReserveOne();
                    MKQL_ENSURE(Fetched < static_cast<ui64>(std::numeric_limits<TRow>::max()),
                                "Window partition is too large");
                    ++Fetched;
                    return EConsumeStatus::Ok;
                case NUdf::EFetchStatus::Yield:
                    return EConsumeStatus::Wait;
                case NUdf::EFetchStatus::Finish:
                    return EConsumeStatus::End;
            }
            Y_UNREACHABLE();
        }

        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& result) override {
            if (Finished) {
                return NUdf::EFetchStatus::Finish;
            }
            switch (Collector.Next()) {
                case EConsumeStatus::Wait:
                    return NUdf::EFetchStatus::Yield;
                case EConsumeStatus::End:
                    Collector.Clean();
                    Candidates.clear();
                    Buffer.Clear();
                    Alloc->OffloadFree(BufferBytes);
                    BufferBytes = 0;
                    Finished = true;
                    return NUdf::EFetchStatus::Finish;
                case EConsumeStatus::Ok:
                    break;
            }

            // The collector keeps a contiguous tail of successfully fetched
            // rows. Its public indices are rebased whenever that tail shrinks.
            const auto frame = Bounds.GetIntervalInQueueByRange(0);
            MKQL_ENSURE(Fetched >= Buffer.Size() && frame.Min() >= 0 && frame.Max() >= 0,
                        "Invalid window bounds");
            MKQL_ENSURE(static_cast<ui64>(frame.Min()) <= Buffer.Size() &&
                        static_cast<ui64>(frame.Max()) <= Buffer.Size(), "Invalid window bounds");
            const ui64 base = Fetched - Buffer.Size();
            const ui64 begin = base + frame.Min();
            const ui64 end = base + frame.Max();
            const ui64 entering = std::max(SeenEnd, begin);
            auto row = Collector.GetCurrentElement();
            NUdf::TUnboxedValue* items = nullptr;
            result = Ctx.HolderFactory.CreateDirectArrayHolder(Owner->Width, items);
            for (ui32 index = 0; index < Owner->Width; ++index) {
                items[index] = row.GetElement(index);
            }

            for (size_t index = 0; index < Owner->Columns.size(); ++index) {
                const auto& column = Owner->Columns[index];
                auto& candidates = Candidates[index];
                while (!candidates.empty() && candidates.front().Ordinal < begin) {
                    candidates.pop_front();
                }
                if (begin >= end) {
                    candidates.clear();
                } else {
                    for (ui64 ordinal = entering; ordinal < end; ++ordinal) {
                        auto value = Buffer.Get(ordinal - base).GetElement(column.Input);
                        if (!value) {
                            continue;
                        }
                        while (!candidates.empty()) {
                            const int cmp = NUdf::CompareValues(column.Slot, candidates.back().Value, value);
                            if (column.Minimum ? cmp < 0 : cmp > 0) {
                                break;
                            }
                            candidates.pop_back();
                        }
                        candidates.push_back({ordinal, NUdf::TUnboxedValue(value)});
                    }
                }
                MKQL_ENSURE(column.OptionalOutput || !candidates.empty(), "Nonoptional MIN/MAX has an empty frame");
                items[column.Output] = candidates.empty() ? NUdf::TUnboxedValue() : candidates.front().Value;
            }
            // An empty gap may retire never-entered rows. They cannot re-enter:
            // both absolute interval boundaries move only forwards.
            SeenEnd = std::max(SeenEnd, std::max(begin, end));
            return NUdf::EFetchStatus::Ok;
        }

        TComputationContext& Ctx;
        const NUdf::TUnboxedValue Input;
        const TRangeMinMaxWrapper* const Owner;
        TAllocState* const Alloc;
        TCollector::TQueue Buffer;
        TFrameBoundsIndices Bounds;
        TCollector Collector;
        TMKQLVector<TDeque> Candidates;
        ui64 Fetched = 0;
        ui64 SeenEnd = 0;
        size_t BufferBytes = 0;
        bool Finished = false;
    };

public:
    TRangeMinMaxWrapper(TComputationMutables& mutables, IComputationNode* input,
                       TFactory factory, std::vector<IComputationNode*> dependencies,
                       TVector<TColumn> columns, ui32 width)
        : TBase(mutables)
        , Input(input)
        , Factory(std::move(factory))
        , Dependencies(std::move(dependencies))
        , Columns(std::move(columns))
        , Width(width)
    {
    }

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<TStreamValue>(ctx, Input->GetValue(ctx), this);
    }

private:
    void RegisterDependencies() const final {
        DependsOn(Input);
        for (auto* dependency : Dependencies) {
            DependsOn(dependency);
        }
    }

    IComputationNode* const Input;
    const TFactory Factory;
    const std::vector<IComputationNode*> Dependencies;
    const TVector<TColumn> Columns;
    const ui32 Width;
};

} // namespace

IComputationNode* WrapKqpRangeMinMax(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 3, "KqpRangeMinMax expects three arguments");
    auto* input = LocateNode(ctx.NodeLocator, callable, 0);
    const auto* streamType = AS_TYPE(TStreamType, callable.GetInput(0).GetStaticType());
    const auto* rowType = AS_TYPE(TStructType, streamType->GetItemType());
    const auto* mappings = AS_VALUE(TTupleLiteral, callable.GetInput(2));
    MKQL_ENSURE(mappings->GetValuesCount(), "KqpRangeMinMax needs at least one column");
    TVector<TColumn> columns;
    THashSet<ui32> outputs;
    for (ui32 index = 0; index < mappings->GetValuesCount(); ++index) {
        const auto* mapping = AS_VALUE(TTupleLiteral, mappings->GetValue(index));
        MKQL_ENSURE(mapping->GetValuesCount() == 3, "Invalid MIN/MAX column mapping");
        const ui32 source = AS_VALUE(TDataLiteral, mapping->GetValue(0))->AsValue().Get<ui32>();
        const ui32 destination = AS_VALUE(TDataLiteral, mapping->GetValue(1))->AsValue().Get<ui32>();
        const bool minimum = AS_VALUE(TDataLiteral, mapping->GetValue(2))->AsValue().Get<bool>();
        MKQL_ENSURE(source < rowType->GetMembersCount() && destination < rowType->GetMembersCount() &&
                    outputs.insert(destination).second, "Invalid MIN/MAX column mapping");
        auto* sourceType = rowType->GetMemberType(source);
        auto* resultType = rowType->GetMemberType(destination);
        const bool optionalOutput = resultType->IsOptional();
        if (sourceType->IsOptional()) {
            sourceType = AS_TYPE(TOptionalType, sourceType)->GetItemType();
        }
        if (optionalOutput) {
            resultType = AS_TYPE(TOptionalType, resultType)->GetItemType();
        }
        MKQL_ENSURE(sourceType->IsData() && sourceType->IsSameType(*resultType), "MIN/MAX column types differ");
        const auto slot = AS_TYPE(TDataType, sourceType)->GetDataSlot();
        MKQL_ENSURE(slot && NKqp::IsIncrementalWindowMinMaxSlot(*slot), "Unsupported incremental MIN/MAX type");
        columns.push_back({source, destination, *slot, minimum, optionalOutput});
    }

    const auto params = callable.GetInput(1);
    const auto order = DeserializeSortOrder(params);
    MKQL_ENSURE(order == ESortOrder::Asc || order == ESortOrder::Desc, "RANGE requires a sort order");
    auto [variant, dependencies] = DeserializeBoundsAsVariant(params, rowType,
        [&ctx](const TRuntimeNode& node) { return LocateNode(ctx.NodeLocator, *node.GetNode()); }, ctx.Mutables.CurValueIndex);
    MKQL_ENSURE(variant.RangeIntervals().size() == 1 && variant.RangeIncrementals().empty() &&
                variant.RowIntervals().empty() && variant.RowIncrementals().empty(), "Expected one RANGE frame");
    MKQL_ENSURE(!variant.RangeIntervals().front().Min().IsInf(), "Incremental MIN/MAX requires a finite lower bound");
    MKQL_ENSURE(!variant.RangeIntervals().front().Max().IsInf() ||
                variant.RangeIntervals().front().Max().GetDirection() == EDirection::Following,
                "Invalid unbounded RANGE endpoint");
    TDeserializerContext deserialize(
        [](const NUdf::TUnboxedValue& row, ui32 index) { return row.GetElement(index); },
        [](const NUdf::TUnboxedValue& value) { return !static_cast<bool>(value); },
        []<typename T>(const NUdf::TUnboxedValue& value) -> T { return value.Get<T>(); });
    auto bounds = ConvertBoundsToComparators<NUdf::TUnboxedValue, NUdf::TUnboxedValue, TComputationContext>(variant, order, deserialize);
    return new TRangeMinMaxWrapper(ctx.Mutables, input, TCollector::CreateFactory(std::move(bounds)),
                                  std::move(dependencies), std::move(columns), rowType->GetMembersCount());
}

} // namespace NKikimr::NMiniKQL
'''

ANNOTATION = r'''TStatus AnnotateKqpRangeMinMax(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (!EnsureArgsCount(*node, 3, ctx) || !EnsureStreamType(*node->Child(0), ctx)) {
        return TStatus::Error;
    }
    auto fail = [&]() {
        ctx.AddError(TIssue(node->Pos(ctx), "KqpRangeMinMax requires typed scalar MIN/MAX mappings and literal frame settings"));
        return TStatus::Error;
    };
    const auto* row = node->Child(0)->GetTypeAnn()->Cast<TStreamExprType>()->GetItemType();
    if (row->GetKind() != ETypeAnnotationKind::Struct || !node->Child(1)->IsCallable("AsStruct") ||
        !node->Child(2)->IsList() || !node->Child(2)->ChildrenSize()) {
        return fail();
    }
    const auto* rowType = row->Cast<TStructExprType>();
    THashSet<TStringBuf> outputs;
    for (const auto& mapping : node->Child(2)->Children()) {
        if (!mapping->IsList() || mapping->ChildrenSize() != 3 ||
            !mapping->Child(0)->IsAtom() || !mapping->Child(1)->IsAtom() || !mapping->Child(2)->IsAtom()) {
            return fail();
        }
        const auto input = rowType->FindItem(mapping->Child(0)->Content());
        const auto output = rowType->FindItem(mapping->Child(1)->Content());
        if (!input || !output || !outputs.insert(mapping->Child(1)->Content()).second ||
            (mapping->Child(2)->Content() != "min" && mapping->Child(2)->Content() != "max")) {
            return fail();
        }
        auto* inputType = rowType->GetItems()[*input]->GetItemType();
        auto* outputType = rowType->GetItems()[*output]->GetItemType();
        if (inputType->GetKind() == ETypeAnnotationKind::Optional) {
            inputType = inputType->Cast<TOptionalExprType>()->GetItemType();
        }
        if (outputType->GetKind() == ETypeAnnotationKind::Optional) {
            outputType = outputType->Cast<TOptionalExprType>()->GetItemType();
        }
        if (inputType->GetKind() != ETypeAnnotationKind::Data || !IsSameAnnotation(*inputType, *outputType) ||
            !IsIncrementalWindowMinMaxSlot(inputType->Cast<TDataExprType>()->GetSlot())) {
            return fail();
        }
    }
    node->SetTypeAnn(node->Child(0)->GetTypeAnn());
    return TStatus::Ok;
}

'''

COMPILER = r'''    compiler->AddCallable("KqpRangeMinMax",
        [&ctx](const TExprNode& node, TMkqlBuildContext& buildCtx) {
            auto input = MkqlBuildExpr(*node.Child(0), buildCtx);
            auto params = MkqlBuildExpr(*node.Child(1), buildCtx);
            const auto* rowType = AS_TYPE(TStructType, AS_TYPE(TStreamType, input.GetStaticType())->GetItemType());
            TVector<TRuntimeNode> mappings;
            for (const auto& mapping : node.Child(2)->Children()) {
                mappings.push_back(ctx.PgmBuilder().NewTuple({
                    ctx.PgmBuilder().NewDataLiteral<ui32>(rowType->GetMemberIndex(mapping->Child(0)->Content())),
                    ctx.PgmBuilder().NewDataLiteral<ui32>(rowType->GetMemberIndex(mapping->Child(1)->Content())),
                    ctx.PgmBuilder().NewDataLiteral<bool>(mapping->Child(2)->Content() == "min")}));
            }
            return ctx.PgmBuilder().KqpRangeMinMax(input, params, ctx.PgmBuilder().NewTuple(mappings));
        });

'''

BUILDER = r'''TRuntimeNode TKqpProgramBuilder::KqpRangeMinMax(TRuntimeNode input, TRuntimeNode params, TRuntimeNode columns) {
    MKQL_ENSURE(input.GetStaticType()->IsStream(), "KqpRangeMinMax expects a stream");
    TCallableBuilder builder(Env, __func__, input.GetStaticType());
    builder.Add(input);
    builder.Add(params);
    builder.Add(columns);
    return TRuntimeNode(builder.Build(), false);
}

'''

RANGE_ELIGIBILITY = r'''bool TPhysicalWindowBuilder::CanUseRangeMinMax(TInfoUnitId column) const {
    const auto& frame = Window.GetFrame();
    auto finite = [](EWindowFrameBound bound) {
        return bound == EWindowFrameBound::Preceding || bound == EWindowFrameBound::CurrentRow || bound == EWindowFrameBound::Following;
    };
    return frame.Type == EWindowFrameType::Range && finite(frame.BeginKind) &&
        (finite(frame.EndKind) || frame.EndKind == EWindowFrameBound::UnboundedFollowing) && CanUseScalarMinMax(column);
}

'''

PREPASS = r'''    TExprNode::TListType mappings;
    bool onlyRangeMinMax = !Functions.empty();
    for (const auto output : Functions) {
        if (!CanUseRangeMinMax(output)) {
            onlyRangeMinMax = false;
            continue;
        }
        const auto& func = *Window.GetWindowFuncs().Find(output);
        mappings.push_back(Ctx.NewList(Pos, {
            Ctx.NewAtom(Pos, Names.Get(func.Arguments.Items().front())),
            Ctx.NewAtom(Pos, Names.Get(output)), Ctx.NewAtom(Pos, func.Function)}));
    }
    if (!mappings.empty()) {
        auto minmax = Ctx.NewCallable(Pos, "KqpRangeMinMax", {
            collected->ChildPtr(0), collected->ChildPtr(2), Ctx.NewList(Pos, std::move(mappings))});
        // A bypass is restricted to aggregate MIN/MAX-only windows. Native
        // functions and all other aggregates keep the existing final path.
        if (onlyRangeMinMax) {
            return BuildExpandFromStructs(minmax);
        }
        collected = Ctx.ChangeChild(*collected, 0, minmax);
    }
'''


def _replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('production_patch_anchor_is_not_unique')
    return text.replace(old, new, 1)


def render_files(repository):
    def source(path):
        return subprocess.check_output(['git', 'show', SOURCE_COMMIT + ':' + path], cwd=repository, text=True)
    files = {}
    def change(path, function):
        before = source(path)
        files[path] = (before, function(before))
    files[COMMON] = ('', SLOT_HEADER)
    files[RUNTIME_HEADER] = ('', NODE_HEADER)
    files[RUNTIME] = ('', NODE)
    change(PREFIX + 'runtime/ya.make', lambda s: _replace_once(s, '    kqp_stream_enumerate.cpp\n', '    kqp_stream_enumerate.cpp\n    kqp_range_minmax.cpp\n'))
    def compute(s):
        s = _replace_once(s, '#include "kqp_compute.h"', '#include "kqp_compute.h"\n#include "kqp_range_minmax.h"')
        return _replace_once(s, '        return baseFactory(callable, ctx);', '        if (callable.GetType()->GetName() == "KqpRangeMinMax") {\n            return WrapKqpRangeMinMax(callable, ctx);\n        }\n        return baseFactory(callable, ctx);')
    change(PREFIX + 'runtime/kqp_compute.cpp', compute)
    change(PREFIX + 'runtime/common/kqp_program_builder.h', lambda s: _replace_once(s,
        '    TRuntimeNode KqpStreamEnumerate(TRuntimeNode input);',
        '    TRuntimeNode KqpStreamEnumerate(TRuntimeNode input);\n\n    TRuntimeNode KqpRangeMinMax(TRuntimeNode input, TRuntimeNode params, TRuntimeNode columns);'))
    change(PREFIX + 'runtime/common/kqp_program_builder.cpp', lambda s: _replace_once(s, 'TRuntimeNode TKqpProgramBuilder::KqpStreamEnumerate', BUILDER + 'TRuntimeNode TKqpProgramBuilder::KqpStreamEnumerate'))
    change(PREFIX + 'query_compiler/kqp_mkql_compiler.cpp', lambda s: _replace_once(s, '    compiler->AddCallable("KqpStreamEnumerate",', COMPILER + '    compiler->AddCallable("KqpStreamEnumerate",'))
    def annotation(s):
        s = _replace_once(s, '#include "kqp_opt.h"', '#include "kqp_opt.h"\n#include <ydb/core/kqp/common/kqp_window_minmax.h>')
        s = _replace_once(s, 'TStatus AnnotateKqpStreamEnumerate(', ANNOTATION + 'TStatus AnnotateKqpStreamEnumerate(')
        return _replace_once(s, '        AddHandler({TKqpStreamEnumerate::CallableName()},', '        AddHandler({"KqpRangeMinMax"}, Hndl(&AnnotateKqpRangeMinMax));\n        AddHandler({TKqpStreamEnumerate::CallableName()},')
    change(PREFIX + 'opt/kqp_type_ann.cpp', annotation)
    def rbo(s):
        s = _replace_once(s, '#include "kqp_rbo_physical_window_builder.h"', '#include "kqp_rbo_physical_window_builder.h"\n#include <ydb/core/kqp/common/kqp_window_minmax.h>')
        s = _replace_once(s,
            '    if (frame.Type != EWindowFrameType::Rows || !bounded(frame.BeginKind) || !bounded(frame.EndKind)) {',
            '    if (frame.Type != EWindowFrameType::Rows || !bounded(frame.BeginKind) ||\n        (!bounded(frame.EndKind) && frame.EndKind != EWindowFrameBound::UnboundedFollowing)) {')
        start = s.index('    const auto& func = *Window.GetWindowFuncs().Find(column);', s.index('bool TPhysicalWindowBuilder::CanUseRowMinMax('))
        switch = s.index('    // These selectors compare a total scalar order', start)
        end = s.index('\nbool TPhysicalWindowBuilder::HasRowMinMax()', switch)
        prefix = s[start:switch]
        s = s[:start] + '    return CanUseScalarMinMax(column);\n}\n\nbool TPhysicalWindowBuilder::CanUseScalarMinMax(TInfoUnitId column) const {\n' + prefix + '    return IsIncrementalWindowMinMaxSlot(type->Cast<TDataExprType>()->GetSlot());\n}\n\n' + RANGE_ELIGIBILITY + s[end:]
        s = _replace_once(s, '(func.Function == "count" || CanUsePrefixSum(funcs[f]) || CanUseRowMinMax(funcs[f]))))', '(func.Function == "count" || CanUsePrefixSum(funcs[f]) || CanUseRowMinMax(funcs[f]) || CanUseRangeMinMax(funcs[f]))))')
        anchor = '    auto collected = BuildCollector(BuildChainOutputs(wideFlow, /*framePrefixSums=*/true), queue, bounds, ascending);\n'
        s = _replace_once(s, anchor, anchor + PREPASS)
        s = _replace_once(s,
            '        // A fixed ROWS right bound advances by at most one row. Only the first\n        // nonempty frame needs an initial scan; clipped tails add no new row.',
            '        // A finite ROWS right bound advances by at most one row. An\n        // unbounded-following bound drains the partition before the first\n        // result and then stays at its final row. In both cases only the first\n        // nonempty frame needs an initial scan; clipped tails add no new row.')
        return _replace_once(s, '        if (CanUseRowMinMax(funcs[f])) {\n            members.emplace_back(name, Member(rowArg, name));', '        if (CanUseRowMinMax(funcs[f]) || CanUseRangeMinMax(funcs[f])) {\n            members.emplace_back(name, Member(rowArg, name));')
    change(CPP, rbo)
    change(HEADER, lambda s: _replace_once(s, '    bool CanUseRowMinMax(TInfoUnitId column) const;', '    bool CanUseRowMinMax(TInfoUnitId column) const;\n    bool CanUseScalarMinMax(TInfoUnitId column) const;\n    bool CanUseRangeMinMax(TInfoUnitId column) const;'))
    return files


def render_patch(repository):
    return ''.join('diff --git a/' + path + ' b/' + path + '\n' +
        ('' if before else 'new file mode 100644\n') +
        ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                   fromfile='a/' + path if before else '/dev/null', tofile='b/' + path))
        for path, (before, after) in render_files(repository).items())
