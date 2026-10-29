"""Build a base-relative proposed amendment; never edit production sources."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
CASE = json.loads((ROOT / 'cases' / 'Q01.json').read_text())
BASE = CASE['base']
SOURCE = 'ec5c4cdf9df226bab6e13b21da2bb25946a8966b'
CPP = 'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_window_builder.cpp'
HEADER = CPP[:-3] + 'h'


def read(revision, path):
    return subprocess.check_output(['git', 'show', revision + ':' + path], cwd=REPO, text=True)


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError('Source shape changed; review the amendment generator')
    return text.replace(old, new)


def amend_cpp(text):
    text = replace(text, 'TExprNode::TPtr TPhysicalWindowBuilder::BuildOutputRowType() const {',
                   'TExprNode::TPtr TPhysicalWindowBuilder::BuildOutputRowType(bool framePrefixSums) const {')
    text = replace(text, '''    return ExpandType(Pos, *Ctx.MakeType<TStructExprType>(fields), Ctx);
}

TString TPhysicalWindowBuilder::AccumulatorName''', '''    if (framePrefixSums) {
        for (ui32 f = 0; f < Functions.size(); ++f) {
            const auto& func = *Window.GetWindowFuncs().Find(Functions[f]);
            if (CanUsePrefixSum(Functions[f]) && InputItemType(func.Arguments.Items().front())->IsOptionalOrNull()) {
                fields.push_back(Ctx.MakeType<TItemExprType>(SumCountName(f), Ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64)));
            }
        }
    }
    return ExpandType(Pos, *Ctx.MakeType<TStructExprType>(fields), Ctx);
}

TString TPhysicalWindowBuilder::AccumulatorName''')
    text = replace(text, 'TString TPhysicalWindowBuilder::PositionName(ui32 funcIndex) const {', '''TString TPhysicalWindowBuilder::SumCountName(ui32 funcIndex) const {
    return Names.GetTemporaryName("__kqp_win_sum_count_" + ToString(funcIndex) + "_");
}

bool TPhysicalWindowBuilder::CanUsePrefixSum(TInfoUnitId column) const {
    const auto& func = *Window.GetWindowFuncs().Find(column);
    if (func.Kind != EWindowFuncKind::Aggregate || func.Function != "sum" || func.Arguments.Items().size() != 1) {
        return false;
    }
    const auto* type = InputItemType(func.Arguments.Items().front());
    if (type->GetKind() == ETypeAnnotationKind::Optional) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    if (type->GetKind() != ETypeAnnotationKind::Data) {
        return false;
    }
    // These inputs widen exactly to a modular Int64/Uint64 accumulator.
    // Floating, Decimal and Interval sums must preserve their ordered fold.
    switch (type->Cast<TDataExprType>()->GetSlot()) {
        case NUdf::EDataSlot::Int8:
        case NUdf::EDataSlot::Uint8:
        case NUdf::EDataSlot::Int16:
        case NUdf::EDataSlot::Uint16:
        case NUdf::EDataSlot::Int32:
        case NUdf::EDataSlot::Uint32:
        case NUdf::EDataSlot::Int64:
        case NUdf::EDataSlot::Uint64:
            return true;
        default:
            return false;
    }
}

TString TPhysicalWindowBuilder::PositionName(ui32 funcIndex) const {''')
    text = replace(text, 'TExprNode::TPtr TPhysicalWindowBuilder::BuildChainLambda(bool update) const {',
                   'TExprNode::TPtr TPhysicalWindowBuilder::BuildChainLambda(bool update, bool framePrefixSums) const {')
    text = replace(text, '''        outputMembers.emplace_back(Names.Get(funcs[f]), BuildResultFromAccumulator(funcs[f], accumulator));
    }

    if (NeedsPeerKey)''', '''        outputMembers.emplace_back(Names.Get(funcs[f]), BuildResultFromAccumulator(funcs[f], accumulator));
        if (framePrefixSums && CanUsePrefixSum(funcs[f]) && InputItemType(func.Arguments.Items().front())->IsOptionalOrNull()) {
            auto value = Member(itemArg, Names.Get(func.Arguments.Items().front()));
            auto count = update
                ? Ctx.NewCallable(Pos, "AggrCountUpdate", {value, Member(previousState, SumCountName(f))})
                : Ctx.NewCallable(Pos, "AggrCountInit", {value});
            stateMembers.emplace_back(SumCountName(f), count);
            outputMembers.emplace_back(SumCountName(f), count);
        }
    }

    if (NeedsPeerKey)''')
    text = text.replace('skipCount', 'skipPrefixAggregates')
    text = replace(text, '(skipPrefixAggregates && func.Function == "count")',
                   '(skipPrefixAggregates && (func.Function == "count" || CanUsePrefixSum(funcs[f])))')
    start = text.index('TExprNode::TPtr TPhysicalWindowBuilder::BuildChainOutputs(')
    end = text.index('TExprNode::TPtr TPhysicalWindowBuilder::BuildPartitionList(', start)
    part = text[start:end]
    part = replace(part, 'BuildChainOutputs(TExprNode::TPtr wideFlow) const',
                   'BuildChainOutputs(TExprNode::TPtr wideFlow, bool framePrefixSums) const')
    part = replace(part, 'BuildChainLambda(/*update=*/false)', 'BuildChainLambda(/*update=*/false, framePrefixSums)')
    part = replace(part, 'BuildChainLambda(/*update=*/true)', 'BuildChainLambda(/*update=*/true, framePrefixSums)')
    text = text[:start] + part + text[end:]
    start = text.index('TExprNode::TPtr TPhysicalWindowBuilder::BuildQueue(')
    end = text.index('TExprNode::TPtr TPhysicalWindowBuilder::BuildFrameBounds(', start)
    part = text[start:end]
    part = replace(part, 'BuildQueue(TExprNode::TPtr wideFlow) const', 'BuildQueue(TExprNode::TPtr wideFlow, bool framePrefixSums) const')
    part = replace(part, 'BuildOutputRowType()', 'BuildOutputRowType(framePrefixSums)')
    text = text[:start] + part + text[end:]
    start = text.index('TExprNode::TPtr TPhysicalWindowBuilder::BuildFrameFold(')
    end = text.index('TExprNode::TPtr TPhysicalWindowBuilder::BuildPartitionHandler(', start)
    part = text[start:end]
    part = replace(part, 'BuildQueue(wideFlow)', 'BuildQueue(wideFlow, /*framePrefixSums=*/true)')
    part = replace(part, 'BuildChainOutputs(wideFlow)', 'BuildChainOutputs(wideFlow, /*framePrefixSums=*/true)')
    part = replace(part, '''        auto stateArg = Ctx.NewArgument(Pos, "frame_state");''', '''        if (CanUsePrefixSum(funcs[f])) {
            auto first = Ctx.NewArgument(Pos, "first_row");
            auto last = Ctx.NewArgument(Pos, "last_row");
            const auto argument = func.Arguments.Items().front();
            const bool optionalInput = InputItemType(argument)->IsOptionalOrNull();
            const auto* resultType = Window.GetIUType(funcs[f], Ctx);
            const auto* dataType = resultType->GetKind() == ETypeAnnotationKind::Optional
                ? resultType->Cast<TOptionalExprType>()->GetItemType() : resultType;
            const auto sumType = ExpandType(Pos, *dataType, Ctx);
            const auto sumZero = Ctx.NewCallable(Pos, dataType->Cast<TDataExprType>()->GetName(), {Ctx.NewAtom(Pos, "0")});
            const auto empty = Ctx.NewCallable(Pos, "Nothing", {
                Ctx.NewCallable(Pos, "OptionalType", {sumType})});
            auto firstSum = Member(first, name);
            auto lastSum = Member(last, name);
            if (!IsNonOptionalAggregate(funcs[f])) {
                firstSum = Ctx.NewCallable(Pos, "Coalesce", {firstSum, sumZero});
                lastSum = Ctx.NewCallable(Pos, "Coalesce", {lastSum, sumZero});
            }
            auto firstValue = Member(first, Names.Get(argument));
            auto firstContribution = Ctx.NewCallable(Pos, "SafeCast", {firstValue, sumType});
            if (optionalInput) {
                firstContribution = Ctx.NewCallable(Pos, "Coalesce", {firstContribution, sumZero});
            }
            auto sum = Ctx.NewCallable(Pos, "+", {
                Ctx.NewCallable(Pos, "-", {lastSum, firstSum}), firstContribution});
            auto present = Ctx.NewCallable(Pos, "Just", {sum});
            if (optionalInput) {
                // The prefix sums alone cannot distinguish an all-NULL frame
                // from a zero sum. Keep its non-NULL count in the private queue.
                auto count = Ctx.NewCallable(Pos, "+", {
                    Ctx.NewCallable(Pos, "-", {Member(last, SumCountName(f)), Member(first, SumCountName(f))}),
                    Ctx.NewCallable(Pos, "AggrCountInit", {firstValue})});
                present = Ctx.NewCallable(Pos, "If", {
                    Ctx.NewCallable(Pos, ">", {count, zero}), present, empty});
            }
            auto result = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {first, last}), present);
            auto value = Ctx.NewCallable(Pos, "IfPresent", {firstRow, lastRow, result, empty});
            members.emplace_back(name, IsNonOptionalAggregate(funcs[f])
                ? Ctx.NewCallable(Pos, "Unwrap", {value}) : value);
            continue;
        }

        auto stateArg = Ctx.NewArgument(Pos, "frame_state");''')
    return text[:start] + part + text[end:]


def amend_header(text):
    for before, after in [
        ('BuildOutputRowType()', 'BuildOutputRowType(bool framePrefixSums = false)'),
        ('BuildChainLambda(bool update)', 'BuildChainLambda(bool update, bool framePrefixSums = false)'),
        ('BuildChainOutputs(TExprNode::TPtr wideFlow)', 'BuildChainOutputs(TExprNode::TPtr wideFlow, bool framePrefixSums = false)'),
        ('BuildQueue(TExprNode::TPtr wideFlow)', 'BuildQueue(TExprNode::TPtr wideFlow, bool framePrefixSums = false)'),
        ('bool skipCount = false', 'bool skipPrefixAggregates = false'),
        ('    TString PositionName(ui32 funcIndex) const;',
         '    TString SumCountName(ui32 funcIndex) const;\n    bool CanUsePrefixSum(TInfoUnitId column) const;\n    TString PositionName(ui32 funcIndex) const;')]:
        text = replace(text, before, after)
    return text


def main():
    folder = ROOT / 'proposed-quality-amendments'
    folder.mkdir(exist_ok=True)
    patch = ''.join(''.join(difflib.unified_diff(read(BASE, path).splitlines(True),
                    modifier(read(SOURCE, path)).splitlines(True), fromfile='a/' + path, tofile='b/' + path))
                    for path, modifier in ((CPP, amend_cpp), (HEADER, amend_header)))
    target = folder / 'Q01-integer-sum.patch'
    target.write_text(patch)
    check = subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(target)],
                           cwd=ROOT / 'worktrees' / 'verify', capture_output=True)
    metadata = {'id': 'Q01', 'base': BASE, 'source_count_branch': SOURCE,
        'status': 'amendment_proposed_not_applied', 'scope': 'Existing COUNT improvement plus required/optional machine-integer SUM with private non-null prefix counters.',
        'remaining_open': ['Floating SUM', 'Decimal SUM', 'Interval SUM', 'AVG', 'MIN', 'MAX'],
        'files': [CPP, HEADER], 'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
        'apply_check_passed': check.returncode == 0, 'diagnostic_sha256': hashlib.sha256(check.stdout + check.stderr).hexdigest(),
        'compiled': False, 'native_verified': False, 'production_edited': False,
        'validation_required': ['Native ROWS/RANGE boundaries, empty and all-null frames, both directions and peer values.',
            'Signed/unsigned accumulator wrap with required, optional and narrow integral inputs.',
            'Queue-row type agrees with private count output and public output layout excludes auxiliaries.',
            'Fallback SUM/AVG/MIN/MAX and whole/running/suffix frame paths retain ordered behavior.',
            'COUNT/integer-SUM-only general frames have no reachable Fold/Fold1 and scale independently of frame width.']}
    (folder / 'Q01-integer-sum.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps({'id': 'Q01', 'amendment_apply_check_passed': check.returncode == 0,
                      'patch_sha256': metadata['patch_sha256']}))


if __name__ == '__main__':
    main()
