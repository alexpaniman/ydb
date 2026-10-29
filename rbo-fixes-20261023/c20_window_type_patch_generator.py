"""Render the C20 annotation amendment from its exact independent branch.

Only production patch text is persisted. This module has no native/compiler
execution, query fixture, rendered SQL or reproduction entry point.
"""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
BASE_COMMIT = 'a58be05380f395cb3a55d0490f5871a5f308f06b'
SOURCE = 'ydb/core/kqp/opt/kqp_type_ann.cpp'


def render_patch():
    original = subprocess.check_output(
        ['git', 'show', BASE_COMMIT + ':' + SOURCE],
        cwd=HERE / 'worktrees/verify', text=True)
    begin = original.index('TStatus AnnotateOpWindow(')
    end = original.index('\nTStatus AnnotateOpRoot(', begin)
    region = original[begin:end]
    old = '''        if (TString(func.Kind()) == "Native") {
            resultType = ctx.MakeType<TDataExprType>(EDataSlot::Uint64);
        } else {
'''
    new = '''        if (TString(func.Kind()) == "Native") {
            resultType = ctx.MakeType<TDataExprType>(EDataSlot::Uint64);
            const bool ansiRank = !func.AnsiRank() || func.AnsiRank().Cast().Value() == "true";
            const auto sortExpressions = opWindow.SortExpressions();
            if (!ansiRank && (function == "rank" || function == "denserank") && sortExpressions.Size() == 1) {
                const auto keyType = sortExpressions.Item(0).Ref().GetTypeAnn();
                Y_ENSURE(keyType, "Unknown window ranking key");
                if (keyType->GetKind() == ETypeAnnotationKind::Optional) {
                    resultType = ctx.MakeType<TOptionalExprType>(resultType);
                }
            }
        } else {
'''
    if region.count(old) != 1:
        raise ValueError('C20 annotation contract changed')
    replacement = original[:begin] + region.replace(old, new) + original[end:]
    patch = ''.join(difflib.unified_diff(original.splitlines(True), replacement.splitlines(True),
                                       fromfile='a/' + SOURCE, tofile='b/' + SOURCE))
    return patch, {
        'source_before_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'source_after_sha256': hashlib.sha256(replacement.encode()).hexdigest(),
    }


if __name__ == '__main__':
    patch, metadata = render_patch()
    target = HERE / 'proposed-correctness-amendments/C20-annotate-legacy-rank.patch'
    target.write_text(patch)
    print(json.dumps(dict(metadata, patch=str(target),
                          patch_sha256=hashlib.sha256(patch.encode()).hexdigest()), sort_keys=True))
