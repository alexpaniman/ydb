"""Generate fixed-only production-runtime contracts into anonymous buffers.

This module stores a generator, never a rendered translation unit. The actual
KqpRangeMinMax wrapper is linked from the receipt-bound production build.
"""
import hashlib
from pathlib import Path

SOURCE_COMMIT = '3e2a57e559557a716bba6f42eccff44bbc2ed8d7'
PROFILE = 'kqp_runtime'
METRIC_FIELDS = ('input_rows', 'input_fetches', 'input_yields', 'output_rows',
                 'output_yields', 'peak_live_rows', 'allocator_allocations',
                 'allocator_offloads', 'allocator_offloaded_bytes',
                 'peak_used_delta', 'peak_allocated_delta', 'sampled_used_delta', 'cleanup_live_rows',
                 'limit_rejections', 'operator_instances', 'cycles',
                 'allocator_used_before', 'allocator_used_after_release', 'allocator_peak_used_before', 'allocator_peak_used_after', 'allocator_peak_allocated_before', 'allocator_peak_allocated_after', 'allocator_allocations_before', 'allocator_allocations_after', 'allocator_offloads_before', 'allocator_offloads_after', 'allocator_offloaded_bytes_before', 'allocator_offloaded_bytes_after',
                 'allocator_used_after_consumption', 'allocator_peak_used_after_consumption', 'allocator_peak_allocated_after_consumption')


def specs():
    result = []
    for mixed in (False, True):
        for frame in ('overlap', 'empty_gap', 'suffix', 'peers'):
            for lifecycle in ('complete', 'yield', 'cancel_output'):
                result.append(dict(mixed=mixed, frame=frame, lifecycle=lifecycle,
                                   rows=257, descending=len(result) % 2 == 1,
                                   null_keys=frame == 'suffix', nulls_first=len(result) % 3 == 0))
        for lifecycle in ('cancel_yield', 'limit', 'reset'):
            result.append(dict(mixed=mixed, frame='suffix' if lifecycle == 'reset' else 'overlap',
                               lifecycle=lifecycle, rows=257, descending=False,
                               null_keys=False, nulls_first=False))
        for rows in (65, 1025):
            result.append(dict(mixed=mixed, frame='overlap', lifecycle='complete', rows=rows,
                               descending=False, null_keys=False, nulls_first=False))
        result.append(dict(mixed=mixed, frame='overlap', lifecycle='complete', rows=0,
                           descending=False, null_keys=False, nulls_first=False))
        result.append(dict(mixed=mixed, frame='suffix', lifecycle='yield', rows=257,
                           descending=True, null_keys=True, nulls_first=True, all_null=True))
        for limit_at in (3, 17):
            result.append(dict(mixed=mixed, frame='peers', lifecycle='limit', rows=257,
                               descending=False, null_keys=False, nulls_first=False, limit_at=limit_at))
    return result


def cases():
    return ['q01_runtime_' + str(index) for index in range(len(specs()))]


PRELUDE = r'''
#include <ydb/core/kqp/runtime/kqp_range_minmax.h>
#include <yql/essentials/minikql/comp_nodes/ut/mkql_computation_node_ut.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_impl.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_node_builder.h>
#include <util/stream/output.h>
#include <algorithm>
#include <optional>
#include <vector>

using namespace NKikimr::NMiniKQL;
namespace NUdf = NYql::NUdf;
namespace {
struct TSpec {
    bool Mixed;
    ui32 Frame;
    ui32 Lifecycle;
    ui64 Rows;
    bool Descending;
    bool NullKeys;
    bool NullsFirst;
    bool AllNull;
    ui64 LimitAt;
};
struct TStats {
    ui64 Checks = 0;
    bool Passed = true;
    ui64 InputRows = 0, InputFetches = 0, InputYields = 0;
    ui64 OutputRows = 0, OutputYields = 0, LiveRows = 0, PeakLiveRows = 0;
    ui64 LiveSources = 0, Instances = 0, Cycles = 0;
    ui64 Allocations = 0, Offloads = 0, OffloadedBytes = 0;
    ui64 PeakUsed = 0, PeakAllocated = 0, LimitRejections = 0;
    ui64 BaselineUsed = 0, SampledUsed = 0;
    ui64 LimitAttempts = 0, LimitRequiredDelta = 0;
    ui64 LimitOffloads = 0;
    ui64 UsedBefore = 0, UsedAfterRelease = 0, PeakUsedBefore = 0, PeakUsedAfter = 0, PeakAllocatedBefore = 0, PeakAllocatedAfter = 0, AllocationsBefore = 0, AllocationsAfter = 0, OffloadsBefore = 0, OffloadsAfter = 0, OffloadedBytesBefore = 0, OffloadedBytesAfter = 0;
    ui64 UsedAfterConsumption = 0, PeakUsedAfterConsumption = 0, PeakAllocatedAfterConsumption = 0;
    void Check(bool value) { ++Checks; Passed = Passed && value; }
    void Sample() {
        const ui64 used = TlsAllocState->GetUsed();
        SampledUsed = std::max(SampledUsed, used > BaselineUsed ? used - BaselineUsed : 0);
    }
};
struct TRow {
    ui64 Id;
    std::optional<i64> Key, Value;
};
struct TIndices {
    ui32 Id, Key, Value, Minimum, Maximum, Witness;
};

std::vector<TRow> MakeRows(const TSpec& spec) {
    std::vector<TRow> rows;
    for (ui64 index = 0; index < spec.Rows; ++index) {
        std::optional<i64> key = spec.Frame == 3 ? 0 : spec.Frame == 1 ? i64(index * 17) : i64(index / 4);
        if (spec.NullKeys && index % 11 == 0) key.reset();
        std::optional<i64> value = i64((index * 31) % 71) - 35;
        if (spec.AllNull || index % 7 < 3) value.reset();
        rows.push_back({index, key, value});
    }
    std::stable_sort(rows.begin(), rows.end(), [&](const TRow& left, const TRow& right) {
        if (left.Key.has_value() != right.Key.has_value())
            return spec.NullsFirst ? !left.Key : bool(left.Key);
        if (!left.Key) return false;
        return spec.Descending ? *left.Key > *right.Key : *left.Key < *right.Key;
    });
    return rows;
}

class TWitness final: public TComputationValue<TWitness> {
public:
    TWitness(TMemoryUsageInfo* info, TStats& stats): TComputationValue(info), Stats(stats) {
        ++Stats.LiveRows;
        Stats.PeakLiveRows = std::max(Stats.PeakLiveRows, Stats.LiveRows);
    }
    ~TWitness() override { --Stats.LiveRows; }
private:
    NUdf::TStringRef GetResourceTag() const override { return NUdf::TStringRef("RuntimeContractWitness"); }
    void* GetResource() override { return this; }
    TStats& Stats;
};

class TInputNode final: public TMutableComputationNode<TInputNode> {
    using TBase = TMutableComputationNode<TInputNode>;
public:
    class TValue final: public TComputationValue<TValue> {
    public:
        TValue(TMemoryUsageInfo* info, TComputationContext& ctx, const TInputNode* owner)
            : TComputationValue(info), Ctx(ctx), Owner(owner) { ++Owner->Stats.LiveSources; }
        ~TValue() override { --Owner->Stats.LiveSources; }
    private:
        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& output) override {
            auto& stats = Owner->Stats;
            const auto& spec = Owner->Spec;
            ++stats.InputFetches;
            if (Index == Owner->Rows.size()) return NUdf::EFetchStatus::Finish;
            const bool yielding = spec.Lifecycle == 1 || spec.Lifecycle == 3;
            // Two consecutive yields at each boundary also exercise a wait
            // after some rows have already entered the production collector.
            if (yielding && Index % 3 == 1 && YieldCount < 2) {
                ++YieldCount;
                ++stats.InputYields;
                return NUdf::EFetchStatus::Yield;
            }
            YieldCount = 0;
            const auto& row = Owner->Rows[Index++];
            NUdf::TUnboxedValue* fields = nullptr;
            output = Ctx.HolderFactory.CreateDirectArrayHolder(Owner->Width, fields);
            fields[Owner->Indices.Id] = NUdf::TUnboxedValuePod(row.Id);
            if (row.Key) fields[Owner->Indices.Key] = NUdf::TUnboxedValuePod(*row.Key);
            if (row.Value) fields[Owner->Indices.Value] = NUdf::TUnboxedValuePod(*row.Value);
            fields[Owner->Indices.Witness] = Ctx.HolderFactory.Create<TWitness>(stats);
            ++stats.InputRows;
            stats.Sample();
            if (spec.Lifecycle == 4 && Index == spec.LimitAt) {
                // Input construction has completed. The next allocation is
                // the real node's selected public-buffer reserve charge.
                auto* alloc = TlsAllocState;
                stats.LimitOffloads = alloc->GetOffloadedAllocCount();
                alloc->SetIncreaseMemoryLimitCallback([&stats](ui64 current, ui64 required) {
                    ++stats.LimitAttempts;
                    stats.LimitRequiredDelta = required - current;
                });
                alloc->SetLimit(alloc->GetAllocated());
            }
            return NUdf::EFetchStatus::Ok;
        }
        TComputationContext& Ctx;
        const TInputNode* Owner;
        size_t Index = 0;
        ui32 YieldCount = 0;
    };
    TInputNode(TComputationMutables& mutables, const TSpec& spec, const std::vector<TRow>& rows,
               TStats& stats, TIndices indices, ui32 width)
        : TBase(mutables), Spec(spec), Rows(rows), Stats(stats), Indices(indices), Width(width) {}
    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<TValue>(ctx, this);
    }
private:
    void RegisterDependencies() const final {}
    const TSpec& Spec;
    const std::vector<TRow>& Rows;
    TStats& Stats;
    TIndices Indices;
    ui32 Width;
};

TRuntimeNode Bound(TProgramBuilder& pb, i64 offset, bool infinite) {
    TRuntimeNode number;
    if (infinite || offset == 0) {
        const auto value = pb.NewVoid();
        number = pb.Nop(value, pb.NewTaggedType(value.GetStaticType(), infinite ? "inf" : "zero"));
    } else {
        number = pb.NewStruct({{"FiniteValue", pb.NewDataLiteral<i32>(i32(offset < 0 ? -offset : offset))}});
    }
    return pb.NewStruct({
        {"Direction", pb.NewDataLiteral<NUdf::EDataSlot::String>(offset < 0 ? "Preceding" : "Following")},
        {"Number", number}, {"SortedColumn", pb.NewDataLiteral<NUdf::EDataSlot::String>("key")}});
}

TRuntimeNode Params(TProgramBuilder& pb, const TSpec& spec) {
    const auto interval = pb.NewStruct({{"Min", Bound(pb, spec.Frame == 1 ? 2 : -3, false)},
                                       {"Max", Bound(pb, 4, spec.Frame == 2)}});
    const auto bounds = pb.NewStruct({{"RangeIntervals", pb.NewTuple({interval})},
        {"RangeIncrementals", pb.NewEmptyTuple()}, {"RowIntervals", pb.NewEmptyTuple()},
        {"RowIncrementals", pb.NewEmptyTuple()}});
    return pb.NewStruct({{"Bounds", bounds},
        {"SortOrder", pb.NewDataLiteral<NUdf::EDataSlot::String>(spec.Descending ? "Desc" : "Asc")}});
}

bool Member(const TRow& current, const TRow& candidate, const TSpec& spec) {
    if (!current.Key) return !candidate.Key || (spec.Frame == 2 && spec.NullsFirst);
    if (!candidate.Key) return spec.Frame == 2 && !spec.NullsFirst;
    const i64 delta = (*candidate.Key - *current.Key) * (spec.Descending ? -1 : 1);
    return delta >= (spec.Frame == 1 ? 2 : -3) && (spec.Frame == 2 || delta <= 4);
}

void CheckRow(const NUdf::TUnboxedValue& value, ui64 position, const std::vector<TRow>& rows,
              const TSpec& spec, TStructType* resultType, TStats& stats) {
    stats.Check(position < rows.size());
    if (position >= rows.size()) return;
    const auto& current = rows[position];
    std::optional<i64> minimum, maximum;
    ui64 members = 0;
    for (const auto& candidate : rows) {
        if (!Member(current, candidate, spec)) continue;
        ++members;
        if (!candidate.Value) continue;
        minimum = minimum ? std::min(*minimum, *candidate.Value) : candidate.Value;
        maximum = maximum ? std::max(*maximum, *candidate.Value) : candidate.Value;
    }
    stats.Check(value.GetElement(resultType->GetMemberIndex("id")).Get<ui64>() == current.Id);
    const auto low = value.GetElement(resultType->GetMemberIndex("minimum"));
    const auto high = value.GetElement(resultType->GetMemberIndex("maximum"));
    stats.Check(bool(low) == minimum.has_value());
    stats.Check(bool(high) == maximum.has_value());
    if (minimum && low) stats.Check(low.Get<i64>() == *minimum);
    if (maximum && high) stats.Check(high.Get<i64>() == *maximum);
    if (spec.Mixed) stats.Check(value.GetElement(resultType->GetMemberIndex("count")).Get<ui64>() == members);
}

void Run(const TSpec& spec, TStats& stats) {
    const auto rows = MakeRows(spec);
    TIndices indices{};
    ui32 width = 0;
    auto factory = [&](TCallable& callable, const TComputationNodeFactoryContext& ctx) -> IComputationNode* {
        if (callable.GetType()->GetName() == "RuntimeContractInput")
            return new TInputNode(ctx.Mutables, spec, rows, stats, indices, width);
        if (callable.GetType()->GetName() == "KqpRangeMinMax") {
            ++stats.Instances;
            return WrapKqpRangeMinMax(callable, ctx);
        }
        return GetBuiltinFactory()(callable, ctx);
    };
    TSetup<false> setup(factory);
    auto& pb = *setup.PgmBuilder;
    auto* optional = pb.NewOptionalType(pb.NewDataType(NUdf::TDataType<i64>::Id));
    auto* rowType = AS_TYPE(TStructType, pb.NewStructType({
        {"id", pb.NewDataType(NUdf::TDataType<ui64>::Id)}, {"key", optional},
        {"value", optional}, {"minimum", optional}, {"maximum", optional},
        {"witness", pb.NewResourceType("RuntimeContractWitness")}}));
    indices = {rowType->GetMemberIndex("id"), rowType->GetMemberIndex("key"), rowType->GetMemberIndex("value"),
               rowType->GetMemberIndex("minimum"), rowType->GetMemberIndex("maximum"), rowType->GetMemberIndex("witness")};
    width = rowType->GetMembersCount();
    TCallableBuilder input(*setup.Env, "RuntimeContractInput", pb.NewStreamType(rowType), true);
    const auto source = TRuntimeNode(input.Build(), false);
    const auto params = Params(pb, spec);
    const auto mappings = pb.NewTuple({
        pb.NewTuple({pb.NewDataLiteral<ui32>(indices.Value), pb.NewDataLiteral<ui32>(indices.Minimum), pb.NewDataLiteral<bool>(true)}),
        pb.NewTuple({pb.NewDataLiteral<ui32>(indices.Value), pb.NewDataLiteral<ui32>(indices.Maximum), pb.NewDataLiteral<bool>(false)})});
    TCallableBuilder node(*setup.Env, "KqpRangeMinMax", source.GetStaticType());
    node.Add(source); node.Add(params); node.Add(mappings);
    auto program = TRuntimeNode(node.Build(), false);
    if (spec.Mixed) {
        const auto queue = pb.QueueCreate(pb.NewVoid(), pb.NewDataLiteral<ui64>(0), {source},
                                         pb.NewResourceType("TResourceQueue:RuntimeContract"));
        const auto collected = pb.WinFramesCollector(program, queue, params);
        program = pb.Map(collected, [&](TRuntimeNode row) {
            const auto frame = pb.WinFrame(queue, pb.NewDataLiteral<ui64>(0), pb.NewDataLiteral<bool>(false),
                pb.NewDataLiteral<bool>(true), pb.NewDataLiteral<bool>(false), {row}, pb.NewListType(rowType));
            return pb.AddMember(row, "count", pb.Length(frame));
        });
    }
    auto* resultType = AS_TYPE(TStructType, AS_TYPE(TStreamType, program.GetStaticType())->GetItemType());
    auto graph = setup.BuildGraph(program);
    graph->GetContext(); // Exclude lazy graph/context preparation from runtime deltas.
    auto& alloc = setup.Alloc.Ref();
    const ui64 initialPeakUsed = alloc.GetPeakUsed(), initialPeakAllocated = alloc.GetPeakAllocated();
    stats.BaselineUsed = alloc.GetUsed();
    stats.UsedBefore = stats.BaselineUsed;
    stats.PeakUsedBefore = initialPeakUsed;
    stats.PeakAllocatedBefore = initialPeakAllocated;
    const ui64 initialAllocations = alloc.GetAllocCount(), initialOffloads = alloc.GetOffloadedAllocCount();
    const ui64 initialOffloadedBytes = alloc.GetOffloadedBytes();
    stats.AllocationsBefore = initialAllocations;
    stats.OffloadsBefore = initialOffloads;
    stats.OffloadedBytesBefore = initialOffloadedBytes;
    const ui64 cycles = spec.Lifecycle == 5 ? 4 : 1;
    stats.Check(stats.Instances == 1);
    for (ui64 cycle = 0; cycle < cycles; ++cycle) {
        ++stats.Cycles;
        const ui64 usedBefore = alloc.GetUsed();
        const ui64 rowsBefore = stats.InputRows, fetchesBefore = stats.InputFetches;
        auto stream = graph->GetValue();
        NUdf::TUnboxedValue row;
        ui64 output = 0, attempts = 0;
        bool finished = false, canceled = false, rejected = false;
        try {
            while (++attempts < spec.Rows * 10 + 20) {
                const auto status = stream.Fetch(row);
                stats.Sample();
                if (status == NUdf::EFetchStatus::Yield) {
                    ++stats.OutputYields;
                    if (spec.Lifecycle == 3) { canceled = true; break; }
                    continue;
                }
                if (status == NUdf::EFetchStatus::Finish) { finished = true; break; }
                CheckRow(row, output++, rows, spec, resultType, stats);
                ++stats.OutputRows;
                row = NUdf::TUnboxedValue();
                if (spec.Lifecycle == 2 && output == 7) { canceled = true; break; }
            }
        } catch (const TMemoryLimitExceededException&) {
            rejected = true;
            ++stats.LimitRejections;
        }
        alloc.SetLimit(0);
        alloc.SetIncreaseMemoryLimitCallback({});
        row = NUdf::TUnboxedValue();
        stats.Check(attempts < spec.Rows * 10 + 20);
        if (spec.Lifecycle == 4) {
            stats.Check(rejected && !finished && output == 0);
            const ui64 expectedCapacity = std::max<ui64>(2, 2 * (spec.LimitAt - 1));
            stats.Check(stats.LimitAttempts == 1 && stats.LimitRequiredDelta == expectedCapacity * sizeof(NUdf::TUnboxedValue));
            stats.Check(stats.InputRows - rowsBefore == spec.LimitAt);
            stats.Check(alloc.GetOffloadedAllocCount() == stats.LimitOffloads);
        } else {
            stats.Check(!rejected);
            stats.Check(canceled == (spec.Lifecycle == 2 || spec.Lifecycle == 3));
            stats.Check(finished == !canceled);
            if (finished) {
                stats.Check(output == spec.Rows && stats.InputRows - rowsBefore == spec.Rows);
                stats.Check(stats.LiveRows == 0);
                const ui64 fetches = stats.InputFetches;
                stats.Check(stream.Fetch(row) == NUdf::EFetchStatus::Finish);
                stats.Check(stream.Fetch(row) == NUdf::EFetchStatus::Finish);
                stats.Check(stats.InputFetches == fetches);
            }
            if (spec.Lifecycle == 1 || spec.Lifecycle == 3) stats.Check(stats.OutputYields > 0);
            if (spec.Lifecycle == 3) stats.Check(output == 0 && stats.LiveRows > 0);
            stats.Check(stats.InputFetches - fetchesBefore <= spec.Rows * 3 + 3);
        }
        stats.UsedAfterConsumption = alloc.GetUsed();
        stats.PeakUsedAfterConsumption = alloc.GetPeakUsed();
        stats.PeakAllocatedAfterConsumption = alloc.GetPeakAllocated();
        stats.PeakUsed = alloc.GetPeakUsed() - initialPeakUsed;
        stats.PeakAllocated = alloc.GetPeakAllocated() - initialPeakAllocated;
        stream = NUdf::TUnboxedValue();
        row = NUdf::TUnboxedValue();
        graph->Invalidate();
        stats.Check(stats.LiveRows == 0 && stats.LiveSources == 0);
        // Pooled pages may stay cached. A buffer charge smaller than one page
        // must nevertheless disappear; GetUsed excludes free pooled pages.
        stats.Check(alloc.GetUsed() % TAlignedPagePool::POOL_PAGE_SIZE == usedBefore % TAlignedPagePool::POOL_PAGE_SIZE);
        stats.Check(alloc.GetUsed() <= usedBefore + TAlignedPagePool::POOL_PAGE_SIZE);
    }
    stats.UsedAfterRelease = alloc.GetUsed();
    stats.PeakUsedAfter = alloc.GetPeakUsed();
    stats.PeakAllocatedAfter = alloc.GetPeakAllocated();
    stats.AllocationsAfter = alloc.GetAllocCount();
    stats.OffloadsAfter = alloc.GetOffloadedAllocCount();
    stats.OffloadedBytesAfter = alloc.GetOffloadedBytes();
    stats.Allocations = alloc.GetAllocCount() - initialAllocations;
    stats.Offloads = alloc.GetOffloadedAllocCount() - initialOffloads;
    stats.OffloadedBytes = alloc.GetOffloadedBytes() - initialOffloadedBytes;
    if (spec.Lifecycle != 4 && spec.Rows) stats.Check(stats.Offloads > 0 && stats.OffloadedBytes >= 2 * sizeof(NUdf::TUnboxedValue));
    if (!spec.Rows) stats.Check(stats.Offloads == 0 && stats.PeakLiveRows == 0);
    graph.Destroy();
    setup.Reset();
    stats.Check(stats.LiveRows == 0 && stats.LiveSources == 0);
}

void Emit(ui64 index, const TStats& s) {
    Cout << "{\"case_index\":" << index << ",\"checks_executed\":" << s.Checks
         << ",\"passed\":" << (s.Passed ? "true" : "false")
         << ",\"input_rows\":" << s.InputRows << ",\"input_fetches\":" << s.InputFetches
         << ",\"input_yields\":" << s.InputYields << ",\"output_rows\":" << s.OutputRows
         << ",\"output_yields\":" << s.OutputYields << ",\"peak_live_rows\":" << s.PeakLiveRows
         << ",\"allocator_allocations\":" << s.Allocations << ",\"allocator_offloads\":" << s.Offloads
         << ",\"allocator_offloaded_bytes\":" << s.OffloadedBytes << ",\"peak_used_delta\":" << s.PeakUsed
         << ",\"peak_allocated_delta\":" << s.PeakAllocated << ",\"cleanup_live_rows\":" << s.LiveRows
         << ",\"sampled_used_delta\":" << s.SampledUsed
         << ",\"limit_rejections\":" << s.LimitRejections << ",\"operator_instances\":" << s.Instances
         << ",\"cycles\":" << s.Cycles
         << ",\"allocator_used_before\":" << s.UsedBefore
         << ",\"allocator_used_after_release\":" << s.UsedAfterRelease
         << ",\"allocator_peak_used_before\":" << s.PeakUsedBefore
         << ",\"allocator_peak_used_after\":" << s.PeakUsedAfter
         << ",\"allocator_peak_allocated_before\":" << s.PeakAllocatedBefore
         << ",\"allocator_peak_allocated_after\":" << s.PeakAllocatedAfter
         << ",\"allocator_allocations_before\":" << s.AllocationsBefore
         << ",\"allocator_allocations_after\":" << s.AllocationsAfter
         << ",\"allocator_offloads_before\":" << s.OffloadsBefore
         << ",\"allocator_offloads_after\":" << s.OffloadsAfter
         << ",\"allocator_offloaded_bytes_before\":" << s.OffloadedBytesBefore
         << ",\"allocator_offloaded_bytes_after\":" << s.OffloadedBytesAfter
         << ",\"allocator_used_after_consumption\":" << s.UsedAfterConsumption
         << ",\"allocator_peak_used_after_consumption\":" << s.PeakUsedAfterConsumption
         << ",\"allocator_peak_allocated_after_consumption\":" << s.PeakAllocatedAfterConsumption
         << "}\n";
}
} // namespace
'''


def render_cpp_suite():
    frames = dict(overlap=0, empty_gap=1, suffix=2, peers=3)
    lifecycles = {'complete': 0, 'yield': 1, 'cancel_output': 2, 'cancel_yield': 3, 'limit': 4, 'reset': 5}
    literals = []
    for spec in specs():
        values = [spec['mixed'], frames[spec['frame']], lifecycles[spec['lifecycle']], spec['rows'],
                  spec['descending'], spec['null_keys'], spec['nulls_first'], spec.get('all_null', False), spec.get('limit_at', 1)]
        literals.append('{' + ','.join(str(value).lower() for value in values) + '}')
    return PRELUDE + '\nint main() {\n const TSpec cases[] = {' + ','.join(literals) + r'''};
    bool passed = true;
    for (ui64 index = 0; index < std::size(cases); ++index) {
        TStats stats;
        try { Run(cases[index], stats); } catch (...) { stats.Check(false); }
        Emit(index, stats);
        passed = passed && stats.Passed;
    }
    return passed ? 0 : 1;
}
'''


def execution_manifest():
    return dict(generator='quality_range_runtime_cpp_generator', source_commit=SOURCE_COMMIT,
                generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                case_count=len(specs()), fixed_only=True, required_profile=PROFILE,
                production_target='ydb/core/kqp/runtime/streaming/ut',
                sample_source='ydb/core/kqp/runtime/streaming/kqp_streaming_aggregation_ut.cpp',
                required_symbol='NKikimr::NMiniKQL::WrapKqpRangeMinMax',
                verdict_numeric_fields=list(METRIC_FIELDS), compilation_executed=False,
                native_executed=False, generated_sources_persisted=False,
                proof_scope='linked_production_runtime_lifecycle_contracts',
                limitations=['Int64 payloads only; SQL typed coverage is separate.',
                             'Source fetch/allocation counters are measured, internal deque comparisons are not instrumented.',
                             'Allocator-limit rejection covers reserve accounting before allocation; std::vector allocation-failure injection is not included.',
                             'Public allocator metrics are process-local MiniKQL accounting, not RSS.',
                             'Peak deltas measure growth above the allocator prior high-water mark; sampled_used_delta measures observed runtime usage above its prepared-graph baseline. Neither is a query RSS bound.',
                             'Absolute before snapshots follow graph/context preparation. Absolute consumption snapshots precede final-cycle stream release; after snapshots include stream release and graph invalidation, before graph/setup destruction; they retain historical allocator peaks rather than reset them.',
                             'Mixed mode uses the real collector and frame length; full RBO lowering and other native functions remain SQL acceptance responsibilities.',
                             'GetUsed modulo-page cleanup check assumes no unrelated offloaded allocations enter the fixture; native validation is still required.'])
