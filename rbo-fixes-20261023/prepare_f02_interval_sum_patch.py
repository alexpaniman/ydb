"""Prepare the main-based interval SUM normalization patch, without checkout edits."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
PREFIX = 'ydb/core/kqp/opt/rbo/'

RULE = r'''#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

namespace NKikimr::NKqp {

bool TNormalizeIntervalSumRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    if (input->GetKind() != EOperator::Aggregate) {
        return false;
    }
    const auto& aggregate = CastOperator<TOpAggregate>(*input);
    if (aggregate.GetAggregationPhase() != EOpPhase::Undefined || aggregate.IsDistinctAll()) {
        return false;
    }
    const auto traits = aggregate.GetAggregationTraits().Items() | std::views::values;
    return std::any_of(traits.begin(), traits.end(), [](const auto& trait) { return trait.AggFunction == "sum"; });
}

TIntrusivePtr<IOperator> TNormalizeIntervalSumRule::SimpleMatchAndApply(
    const TIntrusivePtr<IOperator>& input, TRBOContext& rboCtx, TPlanProps& props) {
    if (!QuickMatch(input)) {
        return input;
    }
    const auto aggregate = CastOperator<TOpAggregate>(input);
    auto& ctx = rboCtx.ExprCtx;
    const auto pos = aggregate->Pos;
    const auto* intType = ctx.MakeType<TDataExprType>(EDataSlot::Int64);
    const auto* decimalType = ctx.MakeType<TDataExprParamsType>(EDataSlot::Decimal, "35", "0");

    // Match the interval SUM traits: accumulate exactly in Decimal and check
    // the interval range only after every partial state has been merged.
    const auto castThroughInt64 = [&](TInfoUnitId source, const TTypeAnnotationNode& target) {
        auto arg = ctx.NewArgument(pos, "interval_sum");
        auto value = ctx.NewCallable(pos, "Member", {arg, ctx.NewAtom(pos, ctx.GetIndexAsString(source))});
        value = ctx.NewCallable(pos, "StrictCast", {std::move(value), ExpandType(pos, *intType, ctx)});
        value = ctx.NewCallable(pos, "StrictCast", {std::move(value), ExpandType(pos, target, ctx)});
        return TExpression(ctx.NewLambda(pos, ctx.NewArguments(pos, {arg}), std::move(value)), &ctx, &props);
    };

    TMappedIUs<TInfoUnitId> convertedInputs;
    TMapIUs inputs;
    TMapIUs results;
    TAggregationIUs traits;
    for (const auto& [output, original] : aggregate->GetAggregationTraits().Items()) {
        const auto* type = aggregate->GetInput()->GetIUType(original.Input, ctx);
        const auto& valueType = RemoveOptionality(*type);
        if (original.AggFunction != "sum" || valueType.GetKind() != ETypeAnnotationKind::Data
            || !IsDataTypeInterval(valueType.Cast<TDataExprType>()->GetSlot())) {
            traits.Add(output, original);
            continue;
        }

        auto normalized = original;
        if (const auto* existing = convertedInputs.Find(original.Input)) {
            normalized.Input = *existing;
        } else {
            normalized.Input = props.InfoUnitRegistry.AddGenerated("interval_sum_input");
            inputs.Add(normalized.Input, castThroughInt64(original.Input, *decimalType));
            convertedInputs.Add(original.Input, normalized.Input);
        }
        const auto state = props.InfoUnitRegistry.AddCopy(output);
        traits.Add(state, normalized);
        // Keep the original result binding and nullable overflow behavior.
        results.Add(output, castThroughInt64(state, valueType));
    }
    if (results.Keys().Empty()) {
        return input;
    }

    TIntrusivePtr<IOperator> result = MakeIntrusive<TOpMap>(aggregate->GetInput(), pos, std::move(inputs));
    result = MakeIntrusive<TOpAggregate>(result, std::move(traits), aggregate->GetKeyColumns(),
        EOpPhase::Undefined, /*distinctAll=*/false, pos);
    return MakeIntrusive<TOpMap>(result, pos, std::move(results));
}

} // namespace NKikimr::NKqp
'''


def render_files():
    def read(path):
        return subprocess.check_output(['git', 'show', BASE + ':' + path], cwd=ROOT.parent, text=True)

    files = {}
    path = PREFIX + 'kqp_rbo_rules.h'
    before = read(path)
    anchor = '/**\n * Expand distinct aggregation.\n */\n'
    addition = '''/**
 * Normalize interval SUM to the exact Decimal accumulator used by its traits.
 */
class TNormalizeIntervalSumRule: public ISimplifiedRule {
public:
    TNormalizeIntervalSumRule()
        : ISimplifiedRule("Normalize interval SUM", ERuleProperties::RequireTypes) {
    }

    bool QuickMatch(const TIntrusivePtr<IOperator>& input) const override;
    TIntrusivePtr<IOperator> SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) override;
};

'''
    assert before.count(anchor) == 1
    files[path] = before, before.replace(anchor, addition + anchor)
    path = PREFIX + 'kqp_rbo_transformer.cpp'
    before = read(path)
    anchor = '    expandAggregationRules.emplace_back(std::make_unique<TExpandDistinctAggregationRule>());\n'
    assert before.count(anchor) == 1
    files[path] = before, before.replace(anchor, '    expandAggregationRules.emplace_back(std::make_unique<TNormalizeIntervalSumRule>());\n' + anchor)
    path = PREFIX + 'rules/ya.make'
    before = read(path)
    anchor = '    expand_whole_partition_window.cpp\n'
    assert before.count(anchor) == 1
    files[path] = before, before.replace(anchor, anchor + '    normalize_interval_sum.cpp\n')
    files[PREFIX + 'rules/normalize_interval_sum.cpp'] = '', RULE
    return files


def main():
    files = render_files()
    patch = ''.join('diff --git a/' + path + ' b/' + path + '\n'
                    + ('new file mode 100644\n' if not before else '')
                    + ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                        fromfile='a/' + path if before else '/dev/null', tofile='b/' + path))
                    for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/F02-interval-sum-normalization.patch'
    target.write_text(patch)
    with tempfile.TemporaryDirectory(prefix='f02-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env, capture_output=True, check=True)
        check = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                               cwd=ROOT.parent, env=env, capture_output=True)
    report = {'id': 'F02', 'base': BASE, 'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
              'files': list(files), 'apply_check_passed': check.returncode == 0,
              'compiled': False, 'native_verified': False, 'production_edited': False,
              'scope': 'Normalize ordinary and DISTINCT interval SUM before state splitting; preserve original result bindings and final range checks.',
              'exclusions': ['Bounded physical window SUM is separate F03.',
                             'Other existing aggregate defects retain their independent acceptance requirements.']}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': check.returncode == 0}))
    assert check.returncode == 0, 'Apply check failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
