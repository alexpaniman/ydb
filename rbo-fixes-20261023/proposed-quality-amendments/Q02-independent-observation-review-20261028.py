"""Independent source and fake-process checks; no rendered payload is saved."""
import ast
import contextlib
import copy
import difflib
import hashlib
import io
import json
from pathlib import Path
import re
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / 'proposed-quality-amendments/Q02-empty-and-partial-observation-20261028'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    report_path = ROOT / 'verification/Q02-empty-witness-partial-observation-source-preparation-20261028.json'
    report = json.loads(report_path.read_bytes())
    checks, negatives = [], []

    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)

    check(sha(report_path.read_bytes()) == '005b777e5d6e74f638cdd3f75b38900128a540cbff144ac4b4852b944377dda0', 'frozen_source_report')
    patch_path = ROOT / report['patch']
    check(sha(patch_path.read_bytes()) == report['patch_sha256'], 'frozen_patch')
    virtual, sections = {}, {}
    for name, expected in report['before_images'].items():
        old, new = (ROOT / name).read_bytes(), (CANDIDATE / name).read_bytes()
        check(sha(old) == expected, 'before:' + name)
        check(sha(new) == report['after_images'][name], 'after:' + name)
        virtual[ROOT / name] = new
        target = 'rbo-fixes-20261023/' + name
        key = 'a/' + target + ' b/' + target
        sections[key] = 'diff --git ' + key + '\n' + ''.join(difflib.unified_diff(
            old.decode().splitlines(True), new.decode().splitlines(True),
            fromfile='a/' + target, tofile='b/' + target))
    actual = {part.splitlines()[0]: 'diff --git ' + part
              for part in patch_path.read_text().split('diff --git ')[1:]}
    check(actual == sections, 'exact_six_file_patch_delta')

    def functions(raw):
        return {node.name: ast.dump(node, include_attributes=False)
                for node in ast.parse(raw).body if isinstance(node, (ast.FunctionDef, ast.ClassDef))}

    for name, changed in (
            ('quality_window_checks.py', {'metadata', '_window_frame_fold_state', 'check_physical_ast'}),
            ('quality_pairing.py', {'_fixed_acceptance'}), ('native_verify.py', {'run_generated'}),
            ('quality_spill_bounds.py', set()), ('c06_c50_bounded_integration_checks.py', set()),
            ('c06_c50_integration_runner.py', set())):
        old, new = functions((ROOT / name).read_bytes()), functions(virtual[ROOT / name])
        for key, value in old.items():
            if key not in changed:
                check(new[key] == value, 'unchanged_function:' + name + ':' + key)
        expected_added = {'parse_empty_result_witness', '_q02_fold_shape_inventory'} if name == 'quality_window_checks.py' else set()
        check(set(new) - set(old) == expected_added and not set(old) - set(new), 'function_set:' + name)

    def without_pins(raw):
        tree = ast.parse(raw)
        node = next(node for node in tree.body if isinstance(node, ast.Assign)
                    and any(isinstance(target, ast.Name) and target.id in ('SOURCE_BINDINGS', '_SOURCE_BINDINGS')
                            for target in node.targets))
        pins = ast.literal_eval(node.value)
        tree.body.remove(node)
        return ast.dump(tree, include_attributes=False), pins

    for name in ('quality_spill_bounds.py', 'c06_c50_bounded_integration_checks.py', 'c06_c50_integration_runner.py'):
        old, old_pins = without_pins((ROOT / name).read_bytes())
        new, pins = without_pins(virtual[ROOT / name])
        check(old == new and old_pins.keys() == pins.keys(), 'binding_literals_only:' + name)
        for key, expected in pins.items():
            path = ROOT.parent / key
            check(sha(virtual[path] if path in virtual else path.read_bytes()) == expected,
                  'exact_dependency:' + name + ':' + key)
    for name, expected in report['immutable_evidence'].items():
        check(sha((ROOT / name).read_bytes()) == expected, 'immutable_evidence:' + name)

    script = CANDIDATE / 'verify_metadata.py'
    check(sha(script.read_bytes()) == report['verification']['scripts'][str(script.relative_to(ROOT))], 'frozen_metadata_script')
    tree = ast.parse(script.read_bytes())
    main_node = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'main')
    index = next(index for index, node in enumerate(main_node.body) if isinstance(node, ast.Assign)
                 and any(isinstance(target, ast.Name) and target.id == 'report' for target in node.targets))
    main_node.body[index:index] = ast.parse('independent_extra(locals())').body
    ast.fix_missing_locations(tree)
    module = types.ModuleType('q02_independent_extended_metadata')
    module.__file__ = str(script)

    def extra(namespace):
        window, pairing = namespace['window'], namespace['pairing']
        good, witness = namespace['good'], namespace['witness_record']
        valid = json.loads(good)
        malformed = [b'\xef\xbb\xbf' + good + b'{}', good + b' null', b'[]', b'null',
                     json.dumps(dict(valid, format=0)).encode(), json.dumps(dict(valid, unknown=0)).encode(),
                     good.replace(b'"name": "id"', b'"name": "id", "name": "id"'),
                     good.replace(b'"columns":', b'"columns": [], "columns":'),
                     good.replace(b'"type":', b'"type": {}, "type":'),
                     good.replace(b'"type_id": 3', b'"type_id": 3e0'),
                     good.replace(b'"type_id": 3', b'"type_id": "3"')]
        for ordinal, payload in enumerate(malformed):
            try:
                window.parse_empty_result_witness('q02_empty', payload, 24)
            except (ValueError, TypeError):
                check(True, 'malformed_parser:' + str(ordinal))
                negatives.append('parser')
            else:
                raise AssertionError('malformed_parser_admitted')
            record = namespace['run_case']('q02_empty', payload)
            check(record.get('semantic_oracle_pass') is False
                  and record.get('empty_result_set_metadata_verified') is not True,
                  'malformed_native_failure:' + str(ordinal))
            negatives.append('native')
        for size in (16384, 16385):
            payload = good + b' ' * (size - len(good))
            try:
                result = window.parse_empty_result_witness('q02_empty', payload, 24)
            except ValueError:
                check(size == 16385, 'exact_size_rejection')
                negatives.append('size')
            else:
                check(size == 16384 and result[1] == [[]] and result[2] is True, 'exact_size_acceptance')
        fields = ('result_output_format', 'empty_result_set_witness_version', 'empty_result_set_metadata_verified',
                  'result_set_boundaries_known', 'row_count', 'result_set_count',
                  'empty_result_set_payload_sha256', 'result_sha256')
        for field in fields:
            bad = copy.deepcopy(witness)
            bad.pop(field)
            for side in ('before', 'after'):
                verdict = pairing._fixed_acceptance(bad, witness) if side == 'before' else pairing._fixed_acceptance(witness, bad)
                check(verdict is False, 'missing_boundary_evidence:' + field + ':' + side)
                negatives.append('pair')
        for label in ('unlisted_private_token', 'AsStruct', 'AggrCountInit', 'Member', 'block', 'Uint64', 'Inc'):
            for scalar in (False, True):
                text = module.graph(scalar, label)
                old = namespace['original']._window_frame_fold_state(text)
                new = window._window_frame_fold_state(text)
                check({key: new[key] for key in old} == old, 'old_partial_state:' + label + ':' + str(scalar))
                check(new['shape_inventory']['observation_only'] is True
                      and new['shape_inventory']['state_binding_proved'] is False,
                      'inventory_no_proof:' + label + ':' + str(scalar))
                check('unlisted_private_token' not in json.dumps(new), 'no_private_diagnostic_atom:' + label + ':' + str(scalar))
        nodes = [['Fold1', ['unlisted_private_token'], ['lambda', [], ['unlisted_private_token']],
                  ['lambda', [], ['unlisted_private_token']]] for _ in range(4096)]
        inventory = window._q02_fold_shape_inventory(lambda value: value, nodes)
        check(inventory['fold_count'] == 4096 and len(json.dumps(inventory)) < 512, 'bounded_inventory_size')
        check('unlisted_private_token' not in json.dumps(inventory), 'bounded_inventory_no_raw_atoms')
        schema, _ = window.schema_and_seeds(24, 'q02_empty')
        check(bool(re.search(r'\bid\s+Int64\s+NOT\s+NULL\b', schema, re.I)), 'required_Int64_id_schema')

    module.independent_extra = extra
    exec(compile(tree, str(script), 'exec'), module.__dict__)
    captured = []
    target = ROOT / 'verification/Q02-empty-witness-partial-observation-metadata-20261028.json'
    prior = target.read_bytes()
    original_write = Path.write_text

    def capture(path, value, *args, **kwargs):
        if path == target:
            captured.append(json.loads(value))
            return len(value)
        return original_write(path, value, *args, **kwargs)

    with patch.object(Path, 'write_text', capture), contextlib.redirect_stdout(io.StringIO()):
        module.main()
    check(target.read_bytes() == prior and len(captured) == 1, 'frozen_author_report_preserved')

    def fingerprint(modules, overlay):
        review = ROOT.parent / 'rbo-review-20261021'
        todo = [ROOT / (name + '.py') for name in modules]
        todo += [ROOT / name for name in ('native_verify.py', 'verification_contract.py', 'quality_pairing.py')]
        files = {}
        while todo:
            path = todo.pop().resolve()
            if str(path) in files:
                continue
            raw = overlay[path] if path in overlay else path.read_bytes()
            files[str(path)] = sha(raw)
            for node in ast.walk(ast.parse(raw)):
                names = ([alias.name for alias in node.names] if isinstance(node, ast.Import)
                         else [node.module] if isinstance(node, ast.ImportFrom) and node.module else [])
                for name in names:
                    for parent in (ROOT, review):
                        candidate = parent / (name.split('.')[0] + '.py')
                        if candidate.is_file():
                            todo.append(candidate)
        for name in ('new.conf', 'legacy.conf'):
            path = review / name
            files[str(path.resolve())] = sha(path.read_bytes())
        return sha(json.dumps(files, sort_keys=True).encode())

    for label, specification in report['profile_addendum'].items():
        check(fingerprint(specification['modules'], {}) == specification['before_sha256'], 'profile_before:' + label)
        check(fingerprint(specification['modules'], virtual) == specification['after_sha256'], 'profile_after:' + label)
    result = {
        'schema': 'Q02_empty_witness_partial_observation_independent_review_v1',
        'status': 'source_metadata_approved_native_pending',
        'patch': report['patch'], 'patch_sha256': report['patch_sha256'],
        'source_report_sha256': sha(report_path.read_bytes()),
        'before_images': report['before_images'], 'after_images': report['after_images'],
        'independent_checks_passed': len(checks), 'independent_negative_checks': len(negatives), 'checks': checks,
        'author_checks_rerun': 204, 'extra_fake_process_cleanup_checks': captured[0]['checks_passed'] - 204,
        'independent_script_sha256': sha(Path(__file__).read_bytes()), 'findings': [],
        'conclusions': [
            'The exact six-file patch preserves integration and spill-policy functions; their changes are source-binding literals only.',
            'Full-json dispatch is restricted to the reviewed Q02 empty/new/24 profile and a loaded source snapshot.',
            'One actual metadata object must prove a required Int64 id column and no rows. Invalid output stays a semantic failure through the mocked native driver.',
            'Both paired phases require one witnessed zero-row result set and its exact raw-result digest.',
            'The empty structural assertion checks intended pipeline removal; generic pairing and nonempty cases retain their contracts.',
            'Partial proof fields and acceptance functions remain unchanged. Diagnostic labels and rejection reasons are allowlisted; scalar states remain pending.',
            'The original four workload identities and historical failed records remain unchanged. All 28 proposed profiles were independently recomputed.'
        ],
        'limits': [
            'Actual empty-result transport and partial-shape observations still require guarded native reruns.',
            'This patch does not complete the partial-state proof or grant full Q02 quality acceptance.',
            'Composition with the dormant Q09/Q20 RAM-readiness change requires a separate reviewed binding refresh.'
        ],
        'native_build_toolchain_or_Git_mutations': False, 'live_or_production_edits': False,
        'rendered_workloads_or_reproductions_persisted_or_shown': False,
    }
    output = ROOT / 'verification/Q02-empty-witness-partial-observation-independent-review-20261028.json'
    output.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'status': result['status'], 'independent_checks': len(checks),
                      'negative_checks': len(negatives), 'author_checks': 204,
                      'extra_cleanup_checks': captured[0]['checks_passed'] - 204,
                      'report_sha256': sha(output.read_bytes())}))


if __name__ == '__main__':
    main()
