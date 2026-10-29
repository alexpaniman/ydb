"""Prepare only a C23 amendment artifact; production source stays untouched."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = '9eda179893974bfabe793a15d697b989d9c344bf'
PREFIX = 'ydb/core/kqp/opt/rbo/physical_conversion/'
CPP = PREFIX + 'kqp_rbo_physical_aggregation_builder.cpp'
HEADER = PREFIX + 'kqp_rbo_physical_aggregation_builder.h'


def replace(text, before, after):
    assert text.count(before) == 1, 'Pinned source anchor changed'
    return text.replace(before, after, 1)


def render_files():
    before = {path: subprocess.check_output(['git', 'show', BASE + ':' + path],
                                           cwd=ROOT.parent, text=True)
              for path in (CPP, HEADER)}
    after = dict(before)
    after[CPP] = replace(after[CPP],
        '''    TVector<TExprNode::TPtr> lambdaResults;
    for (ui32 i = 0; i < keyFields.size(); ++i) {
        auto it = lambdaArgsMap.find(keyFields[i]);
        Y_ENSURE(it != lambdaArgsMap.end());
        lambdaResults.push_back(lambdaArgs[it->second]);
    }
''',
        '''    const auto nullKeys = GetNullKeyFields();
    TVector<TExprNode::TPtr> lambdaResults;
    for (ui32 i = 0; i < keyFields.size(); ++i) {
        auto it = lambdaArgsMap.find(keyFields[i]);
        Y_ENSURE(it != lambdaArgsMap.end());
        // The hash runtime accepts Data or Optional<Data> keys. Bare Null has
        // one value, so encode it as one constant without removing the key:
        // grouped empty input must still produce no rows.
        lambdaResults.push_back(nullKeys.contains(keyFields[i])
            ? Ctx.NewCallable(Pos, "Uint8", {Ctx.NewAtom(Pos, "0")})
            : lambdaArgs[it->second]);
    }
''')
    after[CPP] = replace(after[CPP],
        '''    if (isDistinct) {
        // Several result IDs may refer to one grouping key after copy
''',
        '''    const auto nullKeys = GetNullKeyFields();
    const auto finishKey = [&](const TString& name) {
        // Restore the logical type before either aggregation phase emits its
        // result. Aggregate input fields retain their original types throughout.
        return nullKeys.contains(name) ? Ctx.NewCallable(Pos, "Null", {})
            : lambdaArgs.at(lambdaArgsMap.at(name));
    };

    if (isDistinct) {
        // Several result IDs may refer to one grouping key after copy
''')
    after[CPP] = replace(after[CPP],
        '            addResult(lambdaArgs.at(lambdaArgsMap.at(traits.OriginalColName)), traits.ResultColName);',
        '            addResult(finishKey(traits.OriginalColName), traits.ResultColName);')
    after[CPP] = replace(after[CPP],
        '            addResult(lambdaArgs.at(lambdaArgsMap.at(key)), key);',
        '            addResult(finishKey(key), key);')
    after[CPP] = replace(after[CPP],
        'TVector<TString> TPhysicalAggregationBuilder::GetKeyFields() const {',
        '''THashSet<TString> TPhysicalAggregationBuilder::GetNullKeyFields() const {
    THashSet<TString> result;
    for (const auto key : Aggregate.GetKeyColumns().Items()) {
        const auto* type = Aggregate.GetInput()->GetIUType(key, Ctx);
        Y_ENSURE(type, "No type for aggregation key " << key);
        if (type->GetKind() == ETypeAnnotationKind::Null) {
            result.insert(Names.Get(key));
        }
    }
    return result;
}

TVector<TString> TPhysicalAggregationBuilder::GetKeyFields() const {''')
    after[HEADER] = replace(after[HEADER],
        '    TVector<TString> GetKeyFields() const;',
        '    TVector<TString> GetKeyFields() const;\n    THashSet<TString> GetNullKeyFields() const;')
    return {path: (before[path], after[path]) for path in before}


def main():
    files = render_files()
    patch = ''.join(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                    fromfile='a/' + path, tofile='b/' + path))
                    for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments' / 'C23-null-hash-key.patch'
    target.write_text(patch)
    with tempfile.TemporaryDirectory(prefix='c23-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env,
                       check=True, capture_output=True)
        checked = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                                 cwd=ROOT.parent, env=env, capture_output=True)
    evidence = {
        'id': 'C23', 'base': BASE, 'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
        'files': list(files), 'apply_check_passed': checked.returncode == 0,
        'diagnostic_sha256': hashlib.sha256(checked.stdout + checked.stderr).hexdigest(),
        'production_edited': False, 'compiled': False, 'native_verified': False,
        'root_cause': 'The shared DQ hash runtime requires Data or Optional<Data> extracted keys; bare Null reaches it unchanged.',
        'scope': 'Encode only bare-Null hash keys as a constant Uint8 and restore Null in aggregate and DISTINCT result mappings.',
        'invariants': [
            'Key arity and order remain unchanged; keyed empty input stays empty.',
            'Scalar aggregation dispatch and empty-input padding remain unchanged.',
            'Nullable data keys and ordinary data keys use existing paths.',
            'Aggregate argument fields remain logically typed; only extracted keys change.',
            'Both intermediate and final outputs recover the original Null type.',
            'Distinct result aliases restore by original key identity; liveness pruning remains unchanged.'
        ],
        'pending': ['Compile and native row/block controls on the amended branch.',
                    'Nonempty/empty grouping sets, mixed composite keys, distinct aliases, null argument count, phase and shuffle controls.']
    }
    target.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'patch_sha256': evidence['patch_sha256'], 'apply_check_passed': checked.returncode == 0}))
    assert checked.returncode == 0, 'Patch validation failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
