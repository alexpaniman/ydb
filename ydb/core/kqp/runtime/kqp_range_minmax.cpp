#include "kqp_range_minmax.h"

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
