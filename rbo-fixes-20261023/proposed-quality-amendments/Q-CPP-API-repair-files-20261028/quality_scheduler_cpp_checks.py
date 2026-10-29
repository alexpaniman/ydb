"""Generate anonymous production-rule Q28 semantic and work-counter contracts."""
import hashlib
import json


def specs():
    result = []
    for mode in ('through', 'into'):
        for branches in (8, 16, 32):
            for depth in (2, 4):
                result.append({'mode': mode, 'branches': branches, 'depth': depth, 'unknown_control': False})
    result.append({'mode': 'through', 'branches': 32, 'depth': 4, 'unknown_control': True})
    for mode in ('kept_map', 'removed_map', 'join_children', 'mixed_chain',
                 'canonical_replicate', 'raw_shared_slots', 'subplan_root',
                 'cbo_exclusion', 'physical_exclusion', 'parents_exclusion',
                 'metadata_exclusion', 'types_exclusion', 'liveness_exclusion'):
        result.append({'mode': mode, 'branches': 32, 'depth': 3, 'unknown_control': False,
                       'scenario': True})
    return result


def cases():
    return ['q28_' + item['mode'] + '_' + str(item['branches']) + '_' + str(item['depth'])
            + ('_unknown_control' if item['unknown_control'] else '') for item in specs()]


COMMON = r'''
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace NKikimr;
using namespace NKikimr::NKqp;
using namespace NYql;
static ui64 Checks = 0, ReadRefreshes = 0, Attempts = 0, CandidateVisits = 0;
static ui64 ReportRefreshes = 0, ReportAttempts = 0, ReportVisits = 0;
static ui64 ControlRefreshes = 0, ControlAttempts = 0, ControlVisits = 0;
static TPlanProps* EvalProps = nullptr;
static void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }
class TReadProbe: public TOpRead {
public:
    using TOpRead::TOpRead;
protected:
    void ComputeOutputIUs() override { ++ReadRefreshes; }
};
class TThroughProbe: public TPushMapElementsThroughInputRule {
public:
    bool QuickMatch(const TIntrusivePtr<IOperator>& input) const override {
        ++Attempts;
        return TPushMapElementsThroughInputRule::QuickMatch(input);
    }
};
class TIntoProbe: public TPushMapElementsIntoMapRule {
public:
    bool QuickMatch(const TIntrusivePtr<IOperator>& input) const override {
        ++Attempts;
        return TPushMapElementsIntoMapRule::QuickMatch(input);
    }
};
class TUnknownControl: public IRule {
public:
    TUnknownControl(): IRule("unknown control") {}
    bool QuickMatch(const TIntrusivePtr<IOperator>&) const override { return false; }
    bool MatchAndApply(TIntrusivePtr<IOperator>&, TRBOContext&, TPlanProps&) override { return false; }
};
class TVisitProbe: public TIntoProbe {
public:
    bool QuickMatch(const TIntrusivePtr<IOperator>&) const override { ++CandidateVisits; return false; }
};
// Inherits the production rule's local-effect declaration on the fixed revision.
// Its additional property demand must still force conservative scheduling.
class TPropertyControl: public TIntoProbe {
public:
    explicit TPropertyControl(ui32 demand) { Props |= demand; }
    bool QuickMatch(const TIntrusivePtr<IOperator>&) const override { return false; }
};
static TIntrusivePtr<IOperator> Read(NTests::TIdTestContext& f, TInfoUnitId column) {
    const auto table = NNodes::Build<NNodes::TKqpTable>(f.ExprCtx, f.Pos)
        .Path().Build("/Root/table").PathId().Build("1:1")
        .SysView().Build("").Version().Build("1").Done().Ptr();
    return MakeIntrusive<TReadProbe>("", TUnorderedIUs{column}, NYql::EStorageType::RowStorage,
        table, nullptr, nullptr, std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, f.Pos);
}
using TRow = THashMap<TInfoUnitId, ui64>;
using TRows = TVector<TRow>;
static TRows Evaluate(const TIntrusivePtr<IOperator>& node) {
    if (node->Kind == EOperator::Source) {
        TRow row;
        for (const auto column : CastOperator<TOpRead>(*node).GetColumns()) row[column] = column * 7 + 13;
        return {row};
    }
    if (node->Kind == EOperator::Map) {
        const auto& map = CastOperator<TOpMap>(*node);
        auto rows = Evaluate(map.GetInput());
        for (auto& row : rows) {
            TRow values;
            for (const auto& [output, expression] : map.GetMapElements().Items()) {
                Check(expression.IsColumnAccess());
                const auto source = expression.GetColumnAccess();
                if (row.contains(source)) {
                    values[output] = row.at(source);
                } else {
                    Check(EvalProps && EvalProps->Subplans.Contains(source));
                    const auto& subplan = EvalProps->Subplans.At(source);
                    const auto result = Evaluate(subplan.Plan);
                    Check(result.size() == 1 && subplan.ResultIU.has_value());
                    values[output] = result.front().at(*subplan.ResultIU);
                }
            }
            for (const auto& [output, value] : values) row.emplace(output, value);
        }
        return rows;
    }
    if (node->Kind == EOperator::Filter) return Evaluate(node->GetChild(0));
    if (node->Kind == EOperator::Sort) return Evaluate(node->GetChild(0));
    if (node->Kind == EOperator::Limit) {
        auto result = Evaluate(node->GetChild(0));
        if (result.size() > 1) result.resize(1);
        return result;
    }
    if (node->Kind == EOperator::Join) {
        const auto& join = CastOperator<TOpJoin>(*node);
        Check(join.JoinKind == "Cross");
        TRows result;
        for (const auto& left : Evaluate(join.GetLeftInput())) {
            for (const auto& right : Evaluate(join.GetRightInput())) {
                auto row = left;
                for (const auto& [key, value] : right) Check(row.emplace(key, value).second);
                result.push_back(std::move(row));
            }
        }
        return result;
    }
    if (node->Kind == EOperator::Replicate) {
        auto& port = CastOperator<TOpReplicate>(*node);
        const auto input = Evaluate(port.GetInput());
        if (port.IsPrimary()) return input;
        TRows result;
        for (const auto& row : input) {
            TRow output;
            for (const auto& [source, value] : row) output[port.GetRebindings().At(source)] = value;
            result.push_back(std::move(output));
        }
        return result;
    }
    if (node->Kind == EOperator::CBOTree) return Evaluate(CastOperator<TOpCBOTree>(*node).TreeRoot);
    if (node->Kind == EOperator::UnionAll) {
        const auto& merge = CastOperator<TOpUnionAll>(*node);
        TRows result;
        for (size_t index = 0; index < merge.GetChildCount(); ++index) {
            for (const auto& input : Evaluate(merge.GetChild(index))) {
                TRow row;
                for (const auto& [output, binding] : merge.GetColumns().Items()) row[output] = input.at(binding.Inputs[index]);
                result.push_back(std::move(row));
            }
        }
        return result;
    }
    throw std::runtime_error("unsupported generated node");
}
static TVector<ui64> Values(const TIntrusivePtr<IOperator>& node, TInfoUnitId column) {
    TVector<ui64> result;
    for (const auto& row : Evaluate(node)) result.push_back(row.at(column));
    std::sort(result.begin(), result.end());
    return result;
}
static void Run(ui32 branches, ui32 depth, bool through, bool unknown) {
    NTests::TIdTestContext f;
    TVector<std::pair<TIntrusivePtr<IOperator>, TInfoUnitId>> inputs;
    for (ui32 branch = 0; branch < branches; ++branch) {
        const auto base = f.Id("source"), output = f.Id("output");
        auto node = Read(f, base);
        for (ui32 level = 0; level < depth; ++level) {
            if (through) {
                auto predicate = MakeConstant("Bool", "true", f.Pos, &f.ExprCtx);
                node = MakeIntrusive<TOpFilter>(node, f.Pos, predicate);
            } else {
                TMapIUs definitions;
                definitions.Add(f.Id("inner"), f.Column(base));
                node = MakeIntrusive<TOpMap>(node, f.Pos, std::move(definitions));
            }
        }
        TMapIUs definitions;
        definitions.Add(output, f.Column(base));
        node = MakeIntrusive<TOpMap>(node, f.Pos, std::move(definitions), through);
        inputs.emplace_back(node, output);
    }
    while (inputs.size() > 1) {
        TVector<std::pair<TIntrusivePtr<IOperator>, TInfoUnitId>> next;
        for (size_t index = 0; index < inputs.size(); index += 2) {
            if (index + 1 == inputs.size()) { next.push_back(inputs[index]); continue; }
            const auto output = f.Id("merged");
            auto merged = f.Union(inputs[index].first, inputs[index + 1].first,
                {{output, inputs[index].second, inputs[index + 1].second}});
            next.emplace_back(merged, output);
        }
        inputs = std::move(next);
    }
    const auto output = inputs.front().second;
    auto root = f.Root(inputs.front().first, TOrderedIUs<TString>{{output, "value"}});
    EvalProps = &root->PlanProps;
    const auto expected = Values(root->GetInput(), output);
    TVector<std::unique_ptr<IRule>> rules;
    rules.emplace_back(std::make_unique<TVisitProbe>());
    rules.emplace_back(std::make_unique<TThroughProbe>());
    rules.emplace_back(std::make_unique<TIntoProbe>());
    if (unknown) rules.emplace_back(std::make_unique<TUnknownControl>());
    TRuleBasedStage stage("scheduler contract", std::move(rules));
    ReadRefreshes = Attempts = CandidateVisits = 0;
    stage.RunStage(*root, f.RboCtx);
    const auto refreshes = ReadRefreshes, attempts = Attempts, visits = CandidateVisits;
    Check(Values(root->GetInput(), output) == expected);
    NTests::AssertIdInvariants(*root, root->PlanProps);
    Check(expected.size() == branches);
    // Publish cost evidence only after both runs preserve semantics. A baseline
    // cost failure is expected, but a baseline semantic failure is not evidence.
    Attempts = 0;
    stage.RunStage(*root, f.RboCtx);
    Check(Values(root->GetInput(), output) == expected);
    NTests::AssertIdInvariants(*root, root->PlanProps);
    const auto finalAttempts = Attempts;
    ReportRefreshes = refreshes; ReportAttempts = attempts; ReportVisits = visits;
    // Each global output pass reaches all source probes. Local dirty-region
    // refresh may reach them only a bounded number of times per moved Map.
    const ui64 linearBound = ui64(branches) * (3 * depth + 8);
    Check(unknown ? refreshes > linearBound : refreshes <= linearBound);
    Check(attempts > 0);
    Check(visits > 0);
    if (!unknown) Check(attempts <= ui64(branches) * (40 * depth + 80));
    if (!unknown) Check(visits <= ui64(branches) * (20 * depth + 40));
    // The retained conservative pass must already have reached a fixed point.
    Check(finalAttempts <= ui64(branches) * (40 * depth + 80));
}
'''


SCENARIOS = r'''
struct TBranch {
    TIntrusivePtr<IOperator> Node;
    TVector<TInfoUnitId> Columns;
};
static TBranch Combine(NTests::TIdTestContext& f, TVector<TBranch> inputs) {
    while (inputs.size() > 1) {
        TVector<TBranch> next;
        for (size_t index = 0; index < inputs.size(); index += 2) {
            if (index + 1 == inputs.size()) { next.push_back(inputs[index]); continue; }
            TUnionAllIUs bindings(TUnionInputPolicy{2});
            TVector<TInfoUnitId> columns;
            Check(inputs[index].Columns.size() == inputs[index + 1].Columns.size());
            for (size_t column = 0; column < inputs[index].Columns.size(); ++column) {
                const auto output = f.Id("merged");
                bindings.Add(output, TUnionInputRow{{inputs[index].Columns[column], inputs[index + 1].Columns[column]}});
                columns.push_back(output);
            }
            auto merge = MakeIntrusive<TOpUnionAll>(inputs[index].Node, inputs[index + 1].Node,
                f.Pos, std::move(bindings), false);
            next.push_back({merge, std::move(columns)});
        }
        inputs = std::move(next);
    }
    return inputs.front();
}
static TIntrusivePtr<IOperator> Chain(NTests::TIdTestContext& f, TIntrusivePtr<IOperator> input,
    TInfoUnitId column, ui32 depth, bool mixed) {
    for (ui32 level = 0; level < depth; ++level) {
        input = MakeIntrusive<TOpFilter>(input, f.Pos, MakeConstant("Bool", "true", f.Pos, &f.ExprCtx));
        if (mixed) {
            input = MakeIntrusive<TOpLimit>(input, f.Pos, f.Constant(), EOpPhase::Undefined);
            input = MakeIntrusive<TOpSort>(input, f.Pos, TSortIUs{{column, {true, false}}});
        }
    }
    return input;
}
static TIntrusivePtr<IOperator> Alias(NTests::TIdTestContext& f, TIntrusivePtr<IOperator> input,
    const TVector<std::pair<TInfoUnitId, TInfoUnitId>>& aliases, bool push) {
    TMapIUs definitions;
    for (const auto& [output, source] : aliases) definitions.Add(output, f.Column(source));
    return MakeIntrusive<TOpMap>(input, f.Pos, std::move(definitions), push);
}
static TVector<TVector<ui64>> Selected(const TBranch& input) {
    TVector<TVector<ui64>> result;
    for (const auto& row : Evaluate(input.Node)) {
        TVector<ui64> values;
        for (const auto column : input.Columns) values.push_back(row.at(column));
        result.push_back(std::move(values));
    }
    std::sort(result.begin(), result.end());
    return result;
}
struct TMeasured { ui64 Refreshes; ui64 MatchesTried; ui64 Visits; };
static TMeasured MeasureScenario(const TString& mode, ui32 branches, ui32 depth, bool forceUnknown) {
    NTests::TIdTestContext f;
    TVector<TBranch> inputs;
    TIntrusivePtr<TReplicate> hub;
    std::optional<TInfoUnitId> activeCall;
    ui64 expectedMaps = 0;
    for (ui32 branch = 0; branch < branches; ++branch) {
        const auto base = f.Id("source"), output = f.Id("output");
        auto source = Read(f, base);
        if (mode == "physical_exclusion" && branch == 0) source->Props.StageId = 1;
        auto node = Chain(f, source, base, depth, mode == "mixed_chain");
        if (mode == "kept_map" || mode == "removed_map") {
            const auto inner = f.Id("inner"), retained = f.Id("retained");
            node = Alias(f, node, {{inner, base}}, false);
            TVector<std::pair<TInfoUnitId, TInfoUnitId>> aliases{{output, base}};
            TVector<TInfoUnitId> selected{output};
            if (mode == "kept_map") { aliases.emplace_back(retained, inner); selected.push_back(retained); }
            node = Alias(f, node, aliases, false);
            expectedMaps += mode == "kept_map" ? 2 : 1;
            inputs.push_back({node, std::move(selected)});
        } else if (mode == "join_children") {
            const auto right = f.Id("right"), rightOutput = f.Id("right output");
            auto rhs = Chain(f, Read(f, right), right, depth, false);
            node = MakeIntrusive<TOpJoin>(node, rhs, f.Pos, "Cross", TJoinIUs{});
            node = Alias(f, node, {{output, base}, {rightOutput, right}}, true);
            expectedMaps += 2;
            inputs.push_back({node, {output, rightOutput}});
        } else {
            node = Alias(f, node, {{output, base}}, true);
            ++expectedMaps;
            inputs.push_back({node, {output}});
        }
    }
    auto plan = Combine(f, std::move(inputs));
    if (mode == "canonical_replicate") {
        const auto producerOutput = f.Id("producer output");
        auto producer = Chain(f, plan.Node, plan.Columns.front(), 1, false);
        producer = Alias(f, producer, {{producerOutput, plan.Columns.front()}}, true);
        ++expectedMaps;
        hub = TReplicate::Create(producer, f.Pos, f.Props.InfoUnitRegistry);
        auto first = hub->AddOutput(), second = hub->AddOutput();
        const auto secondOutput = second->GetRebindings().At(producerOutput);
        plan = Combine(f, {{first, {producerOutput}}, {second, {secondOutput}}});
    } else if (mode == "raw_shared_slots") {
        plan = Combine(f, {plan, plan});
    } else if (mode == "subplan_root") {
        const auto call = f.Id("call"), local = f.Id("local"), caller = f.Id("caller"), result = f.Id("result");
        auto limited = MakeIntrusive<TOpLimit>(plan.Node, f.Pos, f.Constant(), EOpPhase::Undefined);
        auto producer = Alias(f, limited, {{local, plan.Columns.front()}}, true);
        f.Props.Subplans.Add(call, producer, ESubplanType::EXPR, {}, local);
        activeCall = call;
        plan = {Alias(f, Read(f, caller), {{result, call}}, false), {result}};
        expectedMaps += 2;
    } else if (mode == "cbo_exclusion") {
        plan.Node = MakeIntrusive<TOpCBOTree>(plan.Node, f.Pos);
    }
    TOrderedIUs<TString> columns;
    for (const auto column : plan.Columns) columns.Append(column, "value");
    auto root = f.Root(plan.Node, std::move(columns));
    EvalProps = &root->PlanProps;
    const auto expected = Selected({root->GetInput(), plan.Columns});
    TVector<std::unique_ptr<IRule>> rules;
    rules.emplace_back(std::make_unique<TVisitProbe>());
    rules.emplace_back(std::make_unique<TThroughProbe>());
    rules.emplace_back(std::make_unique<TIntoProbe>());
    if (mode == "parents_exclusion") rules.emplace_back(std::make_unique<TPropertyControl>(ERuleProperties::RequireParents));
    if (mode == "metadata_exclusion") rules.emplace_back(std::make_unique<TPropertyControl>(ERuleProperties::RequireMetadata));
    if (mode == "types_exclusion") rules.emplace_back(std::make_unique<TPropertyControl>(ERuleProperties::RequireTypes));
    if (mode == "liveness_exclusion") rules.emplace_back(std::make_unique<TPropertyControl>(ERuleProperties::RequireLiveness));
    if (forceUnknown) rules.emplace_back(std::make_unique<TUnknownControl>());
    TRuleBasedStage stage("scheduler edge contract", std::move(rules));
    ReadRefreshes = Attempts = CandidateVisits = 0;
    stage.RunStage(*root, f.RboCtx);
    const TMeasured measurement{ReadRefreshes, Attempts, CandidateVisits};
    Check(Selected({root->GetInput(), plan.Columns}) == expected);
    if (mode != "raw_shared_slots") NTests::AssertIdInvariants(*root, root->PlanProps);
    ui64 maps = 0, joins = 0, filters = 0, sorts = 0, limits = 0;
    for (const auto& item : *root) {
        maps += item.Current->Kind == EOperator::Map;
        joins += item.Current->Kind == EOperator::Join;
        filters += item.Current->Kind == EOperator::Filter;
        sorts += item.Current->Kind == EOperator::Sort;
        limits += item.Current->Kind == EOperator::Limit;
    }
    Check(maps == expectedMaps);
    if (mode == "join_children") Check(joins == branches);
    if (mode == "mixed_chain") Check(filters == branches * depth && sorts == filters && limits == filters);
    if (hub) Check(hub->GetInput()->Kind == EOperator::Filter);
    if (activeCall) Check(root->PlanProps.Subplans.At(*activeCall).Plan->Kind == EOperator::Limit);
    // The first call includes its retained conservative pass; another call must
    // not change shape or observable results.
    stage.RunStage(*root, f.RboCtx);
    Check(Selected({root->GetInput(), plan.Columns}) == expected);
    ui64 finalMaps = 0;
    for (const auto& item : *root) finalMaps += item.Current->Kind == EOperator::Map;
    Check(finalMaps == maps);
    return measurement;
}
static void RunScenario(const TString& mode, ui32 branches, ui32 depth) {
    const auto local = MeasureScenario(mode, branches, depth, false);
    const auto conservative = MeasureScenario(mode, branches, depth, true);
    ReportRefreshes = local.Refreshes; ReportAttempts = local.MatchesTried; ReportVisits = local.Visits;
    ControlRefreshes = conservative.Refreshes; ControlAttempts = conservative.MatchesTried; ControlVisits = conservative.Visits;
    const bool excluded = mode == "raw_shared_slots" || mode == "cbo_exclusion" || mode == "physical_exclusion"
        || mode == "parents_exclusion" || mode == "metadata_exclusion" || mode == "types_exclusion" || mode == "liveness_exclusion";
    Check(local.Refreshes > 0 && local.MatchesTried > 0);
    Check(excluded ? local.Refreshes == conservative.Refreshes : local.Refreshes < conservative.Refreshes);
    Check(excluded ? local.MatchesTried == conservative.MatchesTried : local.MatchesTried < conservative.MatchesTried);
    Check(excluded ? local.Visits == conservative.Visits : local.Visits < conservative.Visits);
}
'''


def render_cpp_suite():
    body = [COMMON, SCENARIOS, 'int main() { bool all = true;']
    for index, spec in enumerate(specs()):
        arguments = ','.join((str(spec['branches']), str(spec['depth']),
                              'true' if spec['mode'] == 'through' else 'false',
                              'true' if spec['unknown_control'] else 'false'))
        invoke = ('RunScenario(' + json.dumps(spec['mode']) + ',' + str(spec['branches']) + ',' + str(spec['depth']) + ')'
                  if spec.get('scenario') else 'Run(' + arguments + ')')
        body.append('{ Checks=ReportRefreshes=ReportAttempts=ReportVisits=ControlRefreshes=ControlAttempts=ControlVisits=0; '
                    'bool passed=true; try { ' + invoke + '; } catch (...) { passed=false; }')
        prefix = '{"case_index":' + str(index) + ',"checks_executed":'
        body.append('std::cout << ' + json.dumps(prefix) + ' << Checks << ' + json.dumps(',"passed":')
                    + ' << (passed ? "true" : "false")')
        for field, variable in [('output_refreshes', 'ReportRefreshes'), ('rule_attempts', 'ReportAttempts'),
                                ('candidate_visits', 'ReportVisits'), ('control_output_refreshes', 'ControlRefreshes'),
                                ('control_rule_attempts', 'ControlAttempts'), ('control_candidate_visits', 'ControlVisits')]:
            body.append(' << ' + json.dumps(',"' + field + '":') + ' << ' + variable)
        body.append(' << "}\\n"; all = all && passed; }')
    body.append('return all ? 0 : 1; }')
    return '\n'.join(body) + '\n'


def metadata():
    return {'generator_cases': len(specs()), 'generated_source_sha256': hashlib.sha256(render_cpp_suite().encode()).hexdigest(),
            'standard_numeric_jsonl': True, 'native_runs': 0, 'generated_source_saved': False,
            'acceptance': 'Actual candidate visits, production-rule attempts and source output-refresh counts have bounded local growth; excluded controls retain conservative work.',
            'edge_contracts': ['Kept versus removed top Map', 'Both Join children acquire Maps',
                              'Filter/Limit/Sort chains', 'Canonical Replicate slot versus raw shared slots',
                              'Active subplan root replacement', 'CBO/physical/property-demand fallback'],
            'remaining_native_coverage': ['All generated native contracts await compilation and execution.',
                                         'RightSemi/RightOnly normalization remains covered by the prior Q28 profile.']}
