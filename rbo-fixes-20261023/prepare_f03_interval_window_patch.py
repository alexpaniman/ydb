"""Prepare the independent physical-window interval SUM repair without source edits."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
SOURCE = 'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_window_builder.cpp'


def render_files():
    before = subprocess.check_output(['git', 'show', BASE + ':' + SOURCE], cwd=ROOT.parent, text=True)
    after = before
    anchor = 'bool IsDecimal(const TTypeAnnotationNode* type) {'
    helper = '''const TDataExprType* GetIntervalType(const TTypeAnnotationNode* type) {
    if (type->GetKind() == ETypeAnnotationKind::Optional) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    if (type->GetKind() != ETypeAnnotationKind::Data) {
        return nullptr;
    }
    const auto* data = type->Cast<TDataExprType>();
    return IsDataTypeInterval(data->GetSlot()) ? data : nullptr;
}

'''
    assert after.count(anchor) == 1
    after = after.replace(anchor, helper + anchor, 1)
    anchor = '''    if (func.Function == "sum") {
        // clang-format off
        auto casted = MakeOptional(Ctx.Builder(Pos)
            .Callable("SafeCast")
                .Add(0, value)
                .Add(1, BuildSumCastTarget(argument))
            .Seal().Build(), isOptional);
'''
    replacement = '''    if (func.Function == "sum") {
        TExprNode::TPtr sumInput;
        if (GetIntervalType(InputItemType(argument))) {
            // Match the SUM traits: retain a wide state until the frame result
            // is finalized, so an out-of-range partial sum can still cancel.
            auto intType = Ctx.NewCallable(Pos, "DataType", {Ctx.NewAtom(Pos, "Int64")});
            auto decimalType = Ctx.NewCallable(Pos, "DataType", {
                Ctx.NewAtom(Pos, "Decimal"), Ctx.NewAtom(Pos, "35"), Ctx.NewAtom(Pos, "0")});
            sumInput = Ctx.NewCallable(Pos, "StrictCast", {value, std::move(intType)});
            sumInput = Ctx.NewCallable(Pos, "StrictCast", {std::move(sumInput), std::move(decimalType)});
        } else {
            sumInput = Ctx.NewCallable(Pos, "SafeCast", {value, BuildSumCastTarget(argument)});
        }
        auto casted = MakeOptional(std::move(sumInput), isOptional);
        // clang-format off
'''
    assert after.count(anchor) == 1
    after = after.replace(anchor, replacement, 1)
    anchor = '''    return IsNonOptionalAggregate(column) ? Ctx.NewCallable(Pos, "Unwrap", {std::move(result)}) : result;
'''
    replacement = '''    if (func.Kind == EWindowFuncKind::Aggregate && func.Function == "sum") {
        if (const auto* intervalType = GetIntervalType(InputItemType(func.Arguments.Items().front()))) {
            auto intType = Ctx.NewCallable(Pos, "DataType", {Ctx.NewAtom(Pos, "Int64")});
            result = Ctx.NewCallable(Pos, "StrictCast", {std::move(result), std::move(intType)});
            result = Ctx.NewCallable(Pos, "StrictCast", {std::move(result), ExpandType(Pos, *intervalType, Ctx)});
        }
    }
    return IsNonOptionalAggregate(column) ? Ctx.NewCallable(Pos, "Unwrap", {std::move(result)}) : result;
'''
    assert after.count(anchor) == 1
    after = after.replace(anchor, replacement, 1)
    return {SOURCE: (before, after)}


def main():
    files = render_files()
    content = ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                      ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                  fromfile='a/' + path, tofile='b/' + path))
                      for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/F03-interval-window-sum.patch'
    target.write_text(content)
    with tempfile.TemporaryDirectory(prefix='f03-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env, capture_output=True, check=True)
        checked = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                                 cwd=ROOT.parent, env=env, capture_output=True)
    report = {'id': 'F03', 'base': BASE, 'patch_sha256': hashlib.sha256(content.encode()).hexdigest(),
              'files': list(files), 'apply_check_passed': checked.returncode == 0,
              'scope': 'Accumulate Interval and Interval64 window SUM through exact Decimal(35,0), with checked final conversion preserving nullability.',
              'compiled': False, 'native_verified': False, 'production_edited': False,
              'pending': ['Independent source review and native confirmation on fixed and pinned-main plans.',
                          'Whole-partition windows expanded into logical aggregates require separate F02 normalization.']}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': checked.returncode == 0}))
    assert checked.returncode == 0, 'Apply-check failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
