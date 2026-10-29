"""Prepare a bounded Q02 transport/observer repair, with partial proof pending."""
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROJECT = ROOT.parent


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def function(raw, name):
    node = next(n for n in ast.parse(raw).body if isinstance(n, ast.FunctionDef) and n.name == name)
    return ''.join(raw.decode().splitlines(True)[node.lineno - 1:node.end_lineno])


def replace_one(text, old, new):
    assert text.count(old) == 1
    return text.replace(old, new)


def main():
    composition = json.loads((ROOT / 'verification/quality-idle60-ordered-source-composition-20261028.json').read_bytes())
    names = ('quality_window_checks.py', 'native_verify.py', 'quality_pairing.py', 'quality_spill_bounds.py',
             'c06_c50_bounded_integration_checks.py', 'c06_c50_integration_runner.py')
    before = {name: (ROOT / name).read_bytes() for name in names}
    for name in names:
        expected = composition['after_images'].get(ROOT.name + '/' + name,
                   composition['untouched_source_bindings'].get(name))
        assert expected and sha(before[name]) == expected
    after = dict(before)
    window = before['quality_window_checks.py'].decode()
    old_metadata = function(before['quality_window_checks.py'], 'metadata')
    new_metadata = replace_one(old_metadata, "    return {'generator_version': GENERATOR_VERSION,", "    result = {'generator_version': GENERATOR_VERSION,")
    new_metadata += "\n    if case == 'q02_empty':\n        result['q02_empty_result_witness'] = 'q02_empty_result_metadata_v1'\n    return result"
    window = replace_one(window, old_metadata, new_metadata)
    original_parser = function(before['c06_c50_bounded_integration_checks.py'], 'parse_empty_result_witness')
    parser = original_parser.replace('    original = _cell(case, n)\n', '')
    parser = parser.replace("case not in EMPTY_WITNESS_CASES", "case != 'q02_empty' or type(n) is not int or n != 24")
    parser = parser.replace("'name': 'li'", "'name': 'id'")
    parser = parser.replace('original.expected_rows(case, n)', 'expected_rows(case, n)')
    parser = parser.replace('EMPTY_WITNESS_VERSION', "'q02_empty_result_metadata_v1'")
    parser = parser.replace('_hash(payload)', 'hashlib.sha256(payload).hexdigest()')
    window = replace_one(window, '\ndef _parse_ast(text):', '\n' + parser + '\n\n\ndef _parse_ast(text):')
    window = replace_one(window, "    elif case == 'q02_all_dead':", "    elif case in ('q02_all_dead', 'q02_empty'):")

    # Diagnose the already rejected partial shape only. Existing proof logic and
    # complete/folds/admission fields are untouched; raw names never leave RAM.
    inventory = '''
def _q02_fold_shape_inventory(resolve, nodes):
    allowed = {'AsStruct', 'AggrCountInit', 'AggrCountUpdate', 'Member', 'Uint64',
               'Inc', 'Map', 'OrderedMap', 'WinFrame', 'lambda', 'block', 'Coalesce'}
    def label(node):
        node = resolve(node)
        if isinstance(node, str):
            return 'argument' if node.startswith('$') else 'other'
        return node[0] if isinstance(node, list) and node and isinstance(node[0], str) and node[0] in allowed else 'other'
    result = {'version': 'q02_fold_shape_inventory_v1', 'fold_count': 0,
              'frame_sources': {}, 'init_bodies': {}, 'update_bodies': {},
              'observation_only': True, 'state_binding_proved': False}
    def increment(key, value):
        result[key][value] = result[key].get(value, 0) + 1
    for node in nodes:
        if not isinstance(node, list) or not node or node[0] != 'Fold1':
            continue
        result['fold_count'] += 1
        increment('frame_sources', label(node[1]) if len(node) > 1 else 'missing')
        for index, key in ((2, 'init_bodies'), (3, 'update_bodies')):
            value = resolve(node[index]) if len(node) > index else None
            body = value[2] if isinstance(value, list) and len(value) == 3 and value[0] == 'lambda' else None
            increment(key, label(body) if body is not None else 'unsupported_lambda')
    return result

'''
    window = replace_one(window, '\ndef _window_frame_fold_state(text):', inventory + '\ndef _window_frame_fold_state(text):')
    old_fold = function(before['quality_window_checks.py'], '_window_frame_fold_state')
    reasons = {}
    for node in ast.walk(ast.parse(old_fold)):
        if (isinstance(node, ast.Raise) and isinstance(node.exc, ast.Call)
                and isinstance(node.exc.func, ast.Name) and node.exc.func.id == 'ValueError'):
            value = ast.literal_eval(node.exc.args[0])
            reasons[value] = value.lower().replace(' ', '_')
    new_fold = replace_one(old_fold, '        resolve, nodes = parse_physical_graph(text)',
        "        resolve, nodes = parse_physical_graph(text)\n        result['shape_inventory'] = _q02_fold_shape_inventory(resolve, nodes)")
    new_fold = replace_one(new_fold, "        result['shape_rejection'] = type(error).__name__",
        "        result['shape_rejection'] = type(error).__name__\n        result['shape_rejection_reason'] = " + repr(reasons)
        + ".get(str(error), 'unclassified_shape_rejection')")
    window = replace_one(window, old_fold, new_fold)
    after['quality_window_checks.py'] = window.encode()

    native = before['native_verify.py'].decode()
    marker = "    query = module.recipes(n)[case]"
    addition = '''    q02_empty_witness = meta.get('q02_empty_result_witness')
    if q02_empty_witness is not None:
        if (empty_result_witness is not None
            or q02_empty_witness != 'q02_empty_result_metadata_v1'
            or module.__name__ != 'quality_window_checks' or case != 'q02_empty'
            or mode != 'new' or type(n) is not int or n != 24
            or not getattr(module, '_verification_source_sha256', None)):
            raise ValueError('Unreviewed Q02 empty-result witness request')
        empty_result_witness = q02_empty_witness
'''
    native = replace_one(native, marker, addition + marker)
    after['native_verify.py'] = native.encode()

    pairing = before['quality_pairing.py'].decode()
    marker = "    if case.startswith('q01_sum_') and case.endswith('_control') and case != 'q01_sum_native_control':"
    addition = '''    if case == 'q02_empty' and not all(
            record.get('result_output_format') == 'full-json'
            and record.get('empty_result_set_witness_version') == 'q02_empty_result_metadata_v1'
            and record.get('empty_result_set_metadata_verified') is True
            and record.get('result_set_boundaries_known') is True
            and type(record.get('row_count')) is int and record['row_count'] == 0
            and type(record.get('result_set_count')) is int and record['result_set_count'] == 1
            and _digest(record.get('empty_result_set_payload_sha256'))
            and record['empty_result_set_payload_sha256'] == record.get('result_sha256')
            for record in (before, after)):
        return False
'''
    pairing = replace_one(pairing, marker, addition + marker)
    after['quality_pairing.py'] = pairing.encode()

    cap = before['quality_spill_bounds.py'].decode()
    for name in ('quality_window_checks.py', 'quality_pairing.py'):
        cap = replace_one(cap, sha(before[name]), sha(after[name]))
    after['quality_spill_bounds.py'] = cap.encode()
    wrapper = before['c06_c50_bounded_integration_checks.py'].decode()
    for name in ('quality_spill_bounds.py', 'native_verify.py'):
        wrapper = replace_one(wrapper, sha(before[name]), sha(after[name]))
    after['c06_c50_bounded_integration_checks.py'] = wrapper.encode()
    runner = before['c06_c50_integration_runner.py'].decode()
    for name in ('quality_spill_bounds.py', 'native_verify.py', 'c06_c50_bounded_integration_checks.py'):
        runner = replace_one(runner, sha(before[name]), sha(after[name]))
    after['c06_c50_integration_runner.py'] = runner.encode()
    chunks = []
    for name in names:
        ast.parse(after[name])
        (HERE / name).write_bytes(after[name])
        target = ROOT.name + '/' + name
        chunks.append('diff --git a/' + target + ' b/' + target + '\n' + ''.join(difflib.unified_diff(
            before[name].decode().splitlines(True), after[name].decode().splitlines(True),
            fromfile='a/' + target, tofile='b/' + target)))
    patch_path = ROOT / 'proposed-quality-amendments/Q02-empty-result-and-partial-shape-observation-20261028.patch'
    patch_path.write_text(''.join(chunks))
    with tempfile.TemporaryDirectory(prefix='q02-source-apply-') as temp:
        for name in names:
            path = Path(temp) / ROOT.name / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(before[name])
        for command in (['git', 'apply', '--check', '--whitespace=error-all', str(patch_path)], ['git', 'apply', str(patch_path)]):
            subprocess.run(command, cwd=temp, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert all((Path(temp) / ROOT.name / name).read_bytes() == after[name] for name in names)
    images = {'patch': str(patch_path.relative_to(ROOT)), 'patch_sha256': sha(patch_path.read_bytes()),
              'before': {name:sha(raw) for name,raw in before.items()},
              'after': {name:sha(raw) for name,raw in after.items()}, 'base_helper_composition': 60,
              'partial_acceptance_changed':False, 'production_amendment':False}
    (HERE / 'source-images.json').write_text(json.dumps(images, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'patch_sha256':images['patch_sha256'],'changed_helper_files':len(names)}))


if __name__ == '__main__':
    main()
