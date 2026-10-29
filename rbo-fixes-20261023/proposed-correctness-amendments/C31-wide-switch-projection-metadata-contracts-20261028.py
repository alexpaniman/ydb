"""Observation-only source tests. Serialized inputs exist only in RAM."""
from pathlib import Path
from types import ModuleType
import hashlib
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DORMANT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))
sys.dont_write_bytecode = True
CHECKS = 0


def check(value):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError('wide projection metadata contract failed')


def sha(value):
    return hashlib.sha256(value).hexdigest()


def load(path):
    module = ModuleType(path.stem)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def render(value):
    return '(' + ' '.join(render(item) if isinstance(item, list) else item for item in value) + ')'


def document(variant, codec='WideFromBlocks', width=2):
    branch, foreign = '$private_branch', '$private_foreign'
    arguments = ['$private_field_' + str(i) for i in range(width)]
    pairs = [["'private_output_" + str(i), value] for i, value in enumerate(arguments)]
    if variant == 'permuted':
        pairs.reverse()
    if variant == 'foreign':
        pairs[-1][1] = foreign
    if variant == 'duplicate_argument':
        pairs[-1][1] = arguments[0]
    if variant == 'duplicate_name':
        pairs[-1][0] = pairs[0][0]
    if variant == 'computation':
        pairs[-1][1] = ['StablePickle', arguments[-1]]
    if variant == 'member':
        pairs[-1][1] = ['Member', arguments[-1], "'private_member"]
    if variant == 'invalid_pair':
        pairs[-1] = ["'private_output", arguments[-1], arguments[0]]
    if variant == 'nonatom_name':
        pairs[-1][0] = ['AsList']
    if variant == 'missing_field':
        pairs.pop()
    source = foreign if variant == 'foreign_source' else branch
    source = [codec, source]
    if variant == 'bad_arity':
        source.append(foreign)
    if variant == 'depth':
        for _ in range(14):
            source = ['ToFlow', source]
    projection = ['NarrowMap', ['ToFlow', source], ['lambda', arguments, ['AsStruct', *pairs]]]
    body = ['FromFlow', projection]
    switch = ['Switch', ['AsList'], "'private_buffer", ["'0"],
              ['lambda', [branch], body], ["'0"], ['lambda', ['$private_identity'], '$private_identity']]
    stage = ['DqPhyStage', [], ['lambda', [foreign], switch]]
    return render([['let', '$private_stage', stage], ['return', '$private_stage']])


def main():
    observer_path = DORMANT / 'c31_switch_shape_wide_projection_20261028.candidate.py'
    wrapper_path = DORMANT / 'c31_scope_observation_wide_projection_20261028.candidate.py'
    patch_path = DORMANT / 'C31-wide-switch-projection-observation-20261028.patch'
    observer = load(observer_path)
    old = load(ROOT / 'c31_switch_shape_observation.py')
    fields_path = ROOT / 'c31_shared_field_probe.py'
    original_field_hash = sha(fields_path.read_bytes())
    check(original_field_hash == 'c2c8d8a8dd78705b73fed406ee412f3a2a0b0c09a142e5e99f43140028625298')
    flags = ('exact_shared_producer_origin_proved', 'aggregation_value_to_identity_binding_proved',
             'full_shared_producer_contract_pass', 'candidate_complete_key_pair_lineage_observed')
    cells = []
    variants = ('plain', 'permuted', 'foreign', 'duplicate_argument', 'duplicate_name',
                'computation', 'member', 'invalid_pair', 'nonatom_name', 'missing_field',
                'foreign_source', 'bad_arity', 'depth')
    for case in sorted(observer.fields.CASES):
        for codec in ('WideFromBlocks', 'WideToBlocks'):
            for variant in variants:
                text = document(variant, codec)
                value = observer.observe(case, text)
                check(value['switch_shape_inventory_complete'] is True)
                check(value['observation_only'] is True)
                check(value['physical_ast_sha256'] == sha(text.encode()))
                for flag in flags:
                    check(value[flag] is False)
                encoded = json.dumps(value)
                check('private' not in encoded and '$' not in encoded)
                counts = value['counters']
                check(counts['branch_status'] == {'direct_same_argument': 1})
                check(counts['mapping_argument_counts'] == {'2': 1})
                expected = 'own_argument_bijection' if variant in ('plain', 'permuted', 'foreign_source', 'bad_arity', 'depth') else 'not_own_argument_bijection'
                check(counts['narrow_projection_shape'] == {expected: 1})
                if variant not in ('foreign_source', 'bad_arity', 'depth'):
                    check(counts['input_chain_terminal'] == {'same_branch_argument': 2})
                    check(counts['input_chain_heads']['3:' + codec] == 1)
                elif variant == 'foreign_source':
                    check(counts['input_chain_terminal'] == {'different_argument': 1, 'same_branch_argument': 1})
                elif variant == 'bad_arity':
                    check(counts['input_chain_terminal'] == {'unsupported_arity': 1, 'same_branch_argument': 1})
                else:
                    check(counts['input_chain_terminal'] == {'depth_limit': 1, 'same_branch_argument': 1})
                if variant == 'foreign':
                    check(counts['struct_values']['foreign_scalar_argument'] == 1)
                if variant == 'computation':
                    check(counts['struct_value_heads']['StablePickle'] == 1)
                if variant == 'member':
                    check(counts['struct_value_heads']['Member'] == 1)
                check(observer.observe(case, text) == value)
                cells.append({'case': case, 'codec': codec, 'variant': variant,
                              'candidate_flags_false': True, 'deterministic': True})
    for width in (0, 1, 3, 128, 129):
        value = observer.observe(sorted(observer.fields.CASES)[0], document('plain', width=width))
        check(value['switch_shape_inventory_complete'] is True)
        status = 'own_argument_bijection' if 0 < width <= 128 else 'not_own_argument_bijection'
        check(value['counters']['narrow_projection_shape'] == {status: 1})
        check(all(value[flag] is False for flag in flags))
    # Unknown wrapper names remain sanitized and traversal stops at them.
    text = document('plain', codec='PrivateUnlistedWrapper')
    value = observer.observe(sorted(observer.fields.CASES)[0], text)
    check(value['counters']['input_chain_terminal'] == {'unsupported_shape': 1, 'same_branch_argument': 1})
    check('PrivateUnlistedWrapper' not in json.dumps(value))
    # Existing evidence is read-only, and still carries no lineage proof.
    evidence_path = ROOT / 'verification' / 'guarded-C31-lexical-observation-1793148426841856297.jsonl'
    evidence_before = sha(evidence_path.read_bytes())
    records = [json.loads(line) for line in evidence_path.read_text().splitlines()]
    check(len(records) == 2)
    for record in records:
        observation = record['c31_scope_observation']['lexical_switch_shape_observation']
        check(observation['switch_shape_inventory_complete'] is True)
        check(all(observation[flag] is False for flag in flags))
        check('3:other_callable' in observation['counters']['input_chain_heads'])
    # Only observer source and wrapper version change at installation.
    with tempfile.TemporaryDirectory(prefix='c31-wide-source-review-') as directory:
        install = Path(directory)
        target = install / 'rbo-fixes-20261023'
        target.mkdir()
        for name in ('c31_switch_shape_observation.py', 'c31_scope_observation.py'):
            (target / name).write_bytes((ROOT / name).read_bytes())
        result = subprocess.run(['git', 'apply', '--check', str(patch_path)], cwd=install,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(result.returncode == 0)
        result = subprocess.run(['git', 'apply', str(patch_path)], cwd=install,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(result.returncode == 0)
        check((target / 'c31_switch_shape_observation.py').read_bytes() == observer_path.read_bytes())
        check((target / 'c31_scope_observation.py').read_bytes() == wrapper_path.read_bytes())
        for name in ('quality_window_checks.py', 'c31_identity_checks.py', 'c31_shared_origin_probe.py', 'c31_shared_field_probe.py'):
            (target / name).write_bytes((ROOT / name).read_bytes())
        wrapper = load(target / 'c31_scope_observation.py')
        check(wrapper._switches.VERSION == observer.VERSION)
        check(wrapper._fields is wrapper._switches.fields)
        check(wrapper._HASHES['c31_shared_field_probe'] == original_field_hash)
        check(wrapper._HASHES['c31_switch_shape_observation'] == sha(observer_path.read_bytes()))
        for case in wrapper.CASES:
            before = load(ROOT / 'c31_scope_observation.py').metadata(case, 24)
            after = wrapper.metadata(case, 24)
            check({k:v for k,v in before.items() if k != 'generator_version'}
                  == {k:v for k,v in after.items() if k != 'generator_version'})
    check(sha(fields_path.read_bytes()) == original_field_hash)
    check(sha(evidence_path.read_bytes()) == evidence_before)
    result = {'status': 'source_metadata_only_passed', 'checks': CHECKS,
              'case_vectors': cells, 'synthetic_observations': len(cells) + 6,
              'actual_evidence_sha256': evidence_before,
              'patch_sha256': sha(patch_path.read_bytes()),
              'observer_sha256': sha(observer_path.read_bytes()),
              'wrapper_sha256': sha(wrapper_path.read_bytes()),
              'field_proof_source_unchanged_sha256': original_field_hash,
              'lineage_admission_changed': False, 'native_runs': 0,
              'rendered_graphs_queries_or_cpp_saved': False, 'live_changes': False}
    path = ROOT / 'verification' / 'C31-wide-switch-projection-metadata-contracts-20261028.json'
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'checks': CHECKS, 'report_sha256': sha(path.read_bytes()), 'native_runs': 0}))


if __name__ == '__main__':
    main()
