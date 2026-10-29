"""Bounded source/metadata checks; no native work or serialized input files."""
from pathlib import Path
from types import ModuleType, SimpleNamespace, MethodType
import ast
import hashlib
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DORMANT = Path(__file__).resolve().parent
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]
sys.dont_write_bytecode = True
CHECKS = 0


def check(value):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError('stage tuple metadata contract failed')


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path):
    module = ModuleType(path.stem)
    module.__file__ = str(path)
    sys.modules[module.__name__] = module
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def exercise(module, depth, variant):
    """Observe only which actual stage walk discovers each distinct join."""
    left = module._Node('GraceJoinCore', ())
    right = module._Node('GraceJoinCore', ())
    value = (left, right)
    for _ in range(depth):
        value = (value, value)
    if variant == 'equal_distinct_tuples':
        value = (tuple([left, right]), tuple([left, right]), value)
    elif variant == 'repeated_callable':
        wrapper = module._Node('AsStruct', (value,))
        value = (wrapper, wrapper)
    program = module._Node('lambda', ((), value))
    stage = module._Node('DqPhyStage', ((), program))
    nodes = {left, right, program, stage}
    expected_stages = {stage}
    if variant == 'nested_stage':
        nested = stage
        program = module._Node('lambda', ((), (nested, nested)))
        stage = module._Node('DqPhyStage', ((), program))
        nodes.update((program, stage))
        expected_stages = {nested}
    elif variant == 'shared_program':
        other = module._Node('DqPhyStage', ((), program))
        nodes.add(other)
        expected_stages.add(other)
    proof = module._Proof(SimpleNamespace(nodes=nodes))
    calls = []
    def stage_observer(self, item):
        calls.append(item)
        # Leave the real proof checks in force. This metadata-only stub makes
        # both original and candidate stop at the same missing-field boundary.
        self.joins[left] = ('Inner', (), (), (), ())
        self.joins[right] = ('Left', (), (), (), ())
    proof._stage = MethodType(stage_observer, proof)
    source = Path(module.__file__).read_text().splitlines()
    start = module._Proof.verify.__code__.co_firstlineno
    line = next(i + 1 for i in range(start - 1, len(source)) if source[i].strip() == 'value = pending.pop()')
    iterations = 0
    def trace(frame, event, arg):
        nonlocal iterations
        if event == 'line' and frame.f_code is module._Proof.verify.__code__ and frame.f_lineno == line:
            iterations += 1
        return trace
    prior = sys.gettrace()
    rejection = None
    sys.settrace(trace)
    try:
        proof.verify(sorted(module.CASES)[0])
    except module._Reject as error:
        rejection = str(error)
    finally:
        sys.settrace(prior)
    check(set(calls) == expected_stages and len(calls) == len(expected_stages))
    check(rejection == 'identity_field_unknown')
    return {'iterations': iterations, 'traced_stages': len(calls), 'rejection': rejection}


def main():
    helper = ROOT / 'c31_shared_field_probe.py'
    candidate = DORMANT / 'c31_shared_field_probe_stage_tuple_guard_20261028.candidate.py'
    patch = DORMANT / 'C31-field-stage-tuple-identity-guard-20261028.patch'
    wide_patch = DORMANT / 'C31-wide-switch-projection-observation-20261028.patch'
    before, after = helper.read_text(), candidate.read_text()
    check(sha(before.encode()) == 'c2c8d8a8dd78705b73fed406ee412f3a2a0b0c09a142e5e99f43140028625298')
    check(sha(wide_patch.read_bytes()) == '663071e6b09e6db24a991e2df0f706fd3ba33bd237c6da75fb45c11a6c94a5a8')
    restored = after.replace('            seen_tuples = set()\n', '', 1)
    restored = restored.replace('                    identity = id(value)\n                    if identity in seen_tuples:\n                        continue\n                    seen_tuples.add(identity)\n', '', 1)
    check(restored == before)
    old, new = load(helper), load(candidate)
    check(old.CASES == new.CASES and len(new.CASES) == 2)
    # The public observation and every proof function except the local walk
    # are byte-identical; the walk's new operations are precisely above.
    old_ast, new_ast = ast.parse(before), ast.parse(after)
    for a, b in zip(old_ast.body, new_ast.body):
        if isinstance(a, ast.ClassDef) and a.name == '_Proof':
            for left, right in zip(a.body, b.body):
                if isinstance(left, ast.FunctionDef) and left.name == 'verify':
                    continue
                check(ast.dump(left) == ast.dump(right))
        else:
            check(ast.dump(a) == ast.dump(b))
    measurements = []
    for variant in ('plain', 'equal_distinct_tuples', 'repeated_callable', 'nested_stage', 'shared_program'):
        for depth in (0, 1, 4, 8, 12):
            baseline = exercise(old, depth, variant)
            fixed = exercise(new, depth, variant)
            check({k:v for k,v in baseline.items() if k != 'iterations'}
                  == {k:v for k,v in fixed.items() if k != 'iterations'})
            check(fixed['iterations'] <= 12 * (depth + 2))
            check(fixed['iterations'] <= baseline['iterations'])
            measurements.append({'variant': variant, 'tuple_depth': depth,
                                 'before': baseline, 'after': fixed})
    for depth in (64, 256, 1024):
        fixed = exercise(new, depth, 'plain')
        check(fixed['iterations'] <= 3 * depth + 10)
        measurements.append({'variant': 'plain_large_fixed_only', 'tuple_depth': depth, 'after': fixed})
    snapshots = {name: sha((ROOT / name).read_bytes()) for name in (
        'c31_shared_field_probe.py', 'c31_scope_observation.py', 'c31_switch_shape_observation.py',
        'c31_identity_checks.py', 'c31_shared_origin_probe.py', 'quality_window_checks.py')}
    with tempfile.TemporaryDirectory(prefix='c31-stage-tuple-review-') as directory:
        install = Path(directory)
        target = install / 'rbo-fixes-20261023'
        target.mkdir()
        for name in snapshots:
            (target / name).write_bytes((ROOT / name).read_bytes())
        for selected in (patch, wide_patch):
            for args in (['--check'], []):
                result = subprocess.run(['git', 'apply', *args, str(selected)], cwd=install,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                check(result.returncode == 0)
        check((target / 'c31_shared_field_probe.py').read_bytes() == candidate.read_bytes())
        combined_hashes = {name: sha((target / name).read_bytes()) for name in snapshots}
        check(combined_hashes['c31_switch_shape_observation.py'] == 'f7352e08ab1abdcd3aaa82bb2c4f4dc2fe89a6c733ccc48b4b504f7b0b266fc8')
        check(combined_hashes['c31_scope_observation.py'] == '53936c074cbfd900616accec68caaf0308e8866c12db2d04b3fa91d6504df9a7')
        wrapper = load(target / 'c31_scope_observation.py')
        check(wrapper._fields is wrapper._switches.fields)
        check(wrapper._HASHES['c31_shared_field_probe'] == sha(after.encode()))
        check(wrapper._HASHES['c31_switch_shape_observation'] == combined_hashes['c31_switch_shape_observation.py'])
        for case in wrapper.CASES:
            original = load(ROOT / 'c31_scope_observation.py').metadata(case, 24)
            composed = wrapper.metadata(case, 24)
            check({k:v for k,v in original.items() if k != 'generator_version'}
                  == {k:v for k,v in composed.items() if k != 'generator_version'})
        # Rerun the frozen wide-observer checker on the composed source copies.
        # Only its ROOT and expected helper hash are adapted; no original
        # checker, report or candidate is rewritten.
        evidence_name = 'guarded-C31-lexical-observation-1793148426841856297.jsonl'
        checker_root = install / 'checker-root'
        checker_root.mkdir()
        for name in snapshots:
            (checker_root / name).write_bytes(candidate.read_bytes() if name == helper.name else (ROOT / name).read_bytes())
        (checker_root / 'verification').mkdir()
        (checker_root / 'verification' / evidence_name).write_bytes((ROOT / 'verification' / evidence_name).read_bytes())
        sys.modules['c31_shared_field_probe'] = load(checker_root / helper.name)
        checker_path = DORMANT / 'C31-wide-switch-projection-metadata-contracts-20261028.py'
        checker = checker_path.read_text()
        checker = checker.replace('ROOT = Path(__file__).resolve().parents[1]', 'ROOT = Path(' + repr(str(checker_root)) + ')', 1)
        checker = checker.replace('c2c8d8a8dd78705b73fed406ee412f3a2a0b0c09a142e5e99f43140028625298', sha(after.encode()))
        namespace = {'__file__': str(checker_path), '__name__': 'composed_metadata_checker'}
        exec(compile(checker, str(checker_path), 'exec'), namespace)
        namespace['main']()
        composed_report = json.loads((checker_root / 'verification' / 'C31-wide-switch-projection-metadata-contracts-20261028.json').read_bytes())
        check(composed_report['checks'] == 765)
        check(composed_report['field_proof_source_unchanged_sha256'] == sha(after.encode()))
    for name, expected in snapshots.items():
        check(sha((ROOT / name).read_bytes()) == expected)
    result = {'status': 'source_metadata_only_passed', 'checks': CHECKS,
        'composed_observer_checks': composed_report['checks'], 'measurements': measurements,
        'live_before_sha256': snapshots, 'combined_after_sha256': combined_hashes,
        'ordered_patches': [{'path': str(p.relative_to(ROOT)), 'sha256': sha(p.read_bytes())} for p in (patch, wide_patch)],
        'candidate_source_sha256': sha(after.encode()),
        'proof_predicates_unchanged': True, 'supported_profiles_unchanged': sorted(new.CASES),
        'actual_evidence_relabelled': False, 'native_runs': 0, 'build_runs': 0,
        'compiler_runs': 0, 'live_changes': False, 'rendered_inputs_saved': False}
    report = ROOT / 'verification' / 'C31-stage-tuple-guard-combined-metadata-20261028.json'
    report.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'checks': CHECKS, 'composed_observer_checks': composed_report['checks'],
                      'report_sha256': sha(report.read_bytes()), 'native_runs': 0}))


if __name__ == '__main__':
    main()
