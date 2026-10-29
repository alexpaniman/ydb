"""Independent source/metadata review; rendered workloads remain in memory."""
import ast
import copy
import hashlib
import importlib
import importlib.abc
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT.parent
REVIEW = PROJECT / 'rbo-review-20261021'
sys.dont_write_bytecode = True


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def literals(raw):
    result = {}
    for node in ast.parse(raw).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            try:
                result[node.targets[0].id] = ast.literal_eval(node.value)
            except (ValueError, TypeError):
                pass
    return result


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)

    artifact_pins = {
        'verification/quality-idle60-frozen-review-package-20261028.json': '579425461797e5c84454d40e46e7774fae4efa64bb584e47a3cae4874401c408',
        'verification/quality-idle60-ordered-source-composition-20261028.json': '76b66904ad0033bd74d20780578b0c0151d3351a2b1ac8f039f2cfadc269b95f',
        'verification/quality-idle60-composed-metadata-checks-20261028.json': '94b53b22cfb74eaa9ae8bb6a29061b8bdf2f4f9e9f433862b5299510e99a1664',
    }
    for name, expected in artifact_pins.items():
        check(sha((ROOT / name).read_bytes()) == expected, 'frozen_artifact:' + name)
    manifest = json.loads((ROOT / 'verification/quality-idle60-ordered-source-composition-20261028.json').read_bytes())
    actual_read = Path.read_bytes
    virtual = {}
    for name, expected in manifest['after_images'].items():
        path = (PROJECT / name).resolve()
        raw = (ROOT / manifest['after_images_directory'] / path.name).read_bytes()
        check(sha(raw) == expected, 'exact_after_image:' + path.name)
        virtual[path] = raw
    for name, expected in manifest['before_images'].items():
        path = PROJECT / name
        check(not path.exists() if expected is None else sha(path.read_bytes()) == expected,
              'still_exact_live59_before_image:' + path.name)
    for step in manifest['ordered_patches']:
        check(sha((ROOT / step['path']).read_bytes()) == step['sha256'], 'frozen_patch:' + str(step['ordinal']))
    for name, expected in manifest['untouched_source_bindings'].items():
        check(sha((ROOT / name).read_bytes()) == expected, 'untouched_source:' + name)

    cap_path = ROOT / 'quality_spill_bounds.py'
    old_cap, new_cap = literals(cap_path.read_bytes()), literals(virtual[cap_path])
    check(old_cap['_CELLS'] == new_cap['_CELLS'] and len(new_cap['_CELLS']) == 82, 'all82_exact_original_cells')
    check(set(old_cap['_SOURCE_BINDINGS']) == set(new_cap['_SOURCE_BINDINGS']) and len(new_cap['_SOURCE_BINDINGS']) == 28,
          'exact28_existing_dependency_keys')
    for key in ('VERSION', 'LOCAL_SERVICE', '_BASE_REVISION'):
        check(old_cap[key] == new_cap[key], 'unchanged_cap_constant:' + key)
    for bug, revision in new_cap['_FIXED_REVISIONS'].items():
        check(revision == (manifest['new_Q28_revision']['revision'] if bug == 'Q28' else old_cap['_FIXED_REVISIONS'][bug]),
              'only_reviewed_revision_change:' + bug)
    functions = lambda raw: {n.name: ast.dump(n) for n in ast.parse(raw).body if isinstance(n, (ast.FunctionDef, ast.ClassDef))}
    check(functions(cap_path.read_bytes()) == functions(virtual[cap_path]), 'all_spill_functions_and_classes_unchanged')
    for name, expected in new_cap['_SOURCE_BINDINGS'].items():
        path = PROJECT / name
        check(sha(virtual.get(path, actual_read(path))) == expected, 'actual_final_dependency:' + name)
    check('rbo-fixes-20261023/native_verify.py' not in new_cap['_SOURCE_BINDINGS'], 'no_invented_native_cap_dependency')
    check(virtual[ROOT / 'native_verify.py'] == actual_read(ROOT / 'native_verify.py'), 'native59_unchanged')
    for name in ('c06_c50_bounded_integration_checks.py', 'c06_c50_integration_runner.py'):
        original = ast.parse(actual_read(ROOT / name))
        final = ast.parse(virtual[ROOT / name])
        remove_bindings = lambda tree: ast.dump(ast.Module(body=[n for n in tree.body if not (
            isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'SOURCE_BINDINGS' for t in n.targets))],
            type_ignores=[]))
        check(remove_bindings(original) == remove_bindings(final), 'integration_only_dependency_literals:' + name)
    check(sha(virtual[ROOT / 'quality_join_shuffle_checks.py']) ==
          '2d817f9ba3fc4af3c0306910ce65a3b3640d5ecbe7ae3b14db7fdff6a10190cc', 'exact_reviewed_Q14_functional_image')

    git = lambda *args: subprocess.check_output(['git', '-C', str(PROJECT), *args], stderr=subprocess.PIPE)
    proof_path = ROOT / manifest['new_Q28_revision']['spill_source_proof_path']
    check(sha(proof_path.read_bytes()) == manifest['new_Q28_revision']['spill_source_proof_sha256'], 'exact_spill_source_proof')
    proof = json.loads(proof_path.read_bytes())
    check(len(proof['base_source_sha256']) == 48, 'exact48_audited_spill_sources')
    revisions = {'baseline': new_cap['_BASE_REVISION'], **new_cap['_FIXED_REVISIONS']}
    for bug, revision in revisions.items():
        if bug != 'baseline':
            check(git('show', '-s', '--format=%P', revision).decode().strip() == new_cap['_BASE_REVISION'],
                  'independent_main_parent:' + bug)
            case = json.loads((ROOT / 'cases' / (bug + '.json')).read_bytes())
            check(case['commit'] == revision, 'current_case_revision:' + bug)
        for name, expected in proof['base_source_sha256'].items():
            expected = proof['fixed_source_overrides'].get(bug, {}).get(name, expected)
            check(sha(git('show', revision + ':' + name)) == expected, 'actual_audited_source:' + bug + ':' + name)
    q28 = manifest['new_Q28_revision']
    check(git('diff', '--name-only', q28['old_revision'], q28['revision']).decode().splitlines() == [q28['only_changed_path_from_old']],
          'Q28_only_reviewed_header_changed')
    old = git('show', q28['old_revision'] + ':' + q28['only_changed_path_from_old'])
    new = git('show', q28['revision'] + ':' + q28['only_changed_path_from_old'])
    prefix, suffix = old.split(b'class TPushMapElementsIntoMapRule', 1)
    check(new == prefix + b'class TPushMapElementsIntoMapRule' + suffix.replace(
        b'LocalRewriteDepth() const override { return 2; }', b'LocalRewriteDepth() const override { return 1; }', 1),
        'Q28_exact_rule_local_depth_amendment')

    def read(path):
        return virtual[path.resolve()] if path.resolve() in virtual else actual_read(path)
    def profile(modules, composed):
        pending = [ROOT / (name + '.py') for name in modules]
        pending.extend(ROOT / name for name in ('native_verify.py', 'verification_contract.py', 'quality_pairing.py'))
        files = {}
        while pending:
            path = pending.pop().resolve()
            if str(path) in files:
                continue
            raw = read(path) if composed else actual_read(path)
            files[str(path)] = sha(raw)
            for node in ast.walk(ast.parse(raw)):
                imports = ([n.name for n in node.names] if isinstance(node, ast.Import) else
                           [node.module] if isinstance(node, ast.ImportFrom) and node.module else [])
                for name in imports:
                    for directory in (ROOT, REVIEW):
                        dependency = directory / (name.split('.')[0] + '.py')
                        if (composed and dependency in virtual) or dependency.is_file():
                            pending.append(dependency)
        for name in ('new.conf', 'legacy.conf'):
            path = REVIEW / name
            files[str(path)] = sha(actual_read(path))
        return sha(json.dumps(files, sort_keys=True).encode()), files
    profile_sizes = {}
    for label, spec in manifest['profiles'].items():
        value, files = profile(spec['modules'], True)
        check(value == spec['after_sha256'], 'independent_canonical_after_profile:' + label)
        check(files[str(ROOT / 'native_verify.py')] == manifest['native59_sha256'], 'recursive_native59_binding:' + label)
        profile_sizes[label] = len(files)
        if spec['ledger59_before_sha256'] is not None:
            check(profile(spec['modules'], False)[0] == spec['ledger59_before_sha256'], 'independent_ledger59_profile:' + label)
    check(len(profile_sizes) == 28, 'all28_profile_labels')

    class Sources(importlib.abc.MetaPathFinder, importlib.abc.Loader):
        paths = {}
        def find_spec(self, fullname, path=None, target=None):
            if '.' not in fullname:
                for directory in (ROOT, REVIEW):
                    candidate = directory / (fullname + '.py')
                    if candidate in virtual or candidate.is_file():
                        self.paths[fullname] = candidate
                        return importlib.util.spec_from_loader(fullname, self)
        def create_module(self, spec):
            return None
        def exec_module(self, module):
            path = self.paths[module.__name__]
            module.__file__ = str(path)
            raw = read(path)
            module._verification_source_sha256 = sha(raw)
            exec(compile(raw, str(path), 'exec'), module.__dict__)
    finder = Sources()
    sys.meta_path.insert(0, finder)
    try:
        with patch.object(Path, 'read_bytes', read):
            cap = importlib.import_module('quality_spill_bounds')
            native = importlib.import_module('native_verify')
            receipts = {}
            for bug, revision in revisions.items():
                worktree = ROOT / ('metadata-only-idle60-' + bug)
                binary = {'resolved_path': str(worktree / 'ydb/tests/tools/kqprun/kqprun'),
                          'sha256': sha(bug.encode()), 'size_bytes': 1, 'mtime_ns': 2, 'ctime_ns': 3, 'inode': 4, 'device': 5}
                path = ROOT / ('metadata-only-idle60-' + bug + '-receipt.json')
                raw = json.dumps({'schema_version': 'successful_runner_build_v1', 'revision': revision,
                    'build': {'exit_code': 0, 'target': 'ydb/tests/tools/kqprun'}, 'binary': binary,
                    'worktree': str(worktree)}, sort_keys=True).encode()
                virtual[path] = raw
                receipts[bug] = {'binary_provenance_verified': True, 'binary_provenance_version': 'successful_runner_build_v1',
                    'binary_provenance': binary, 'build_receipt_path': str(path), 'build_receipt_sha256': sha(raw)}
            for case, entry in cap._CELLS.items():
                module = importlib.import_module(entry['module'])
                meta = native.module_metadata(module, case, 'new', 24)
                bug = case.split('_', 1)[0].upper()
                check(cap._digest(meta) == entry['metadata_sha256'], 'actual_original_metadata:' + case)
                for phase, selected in (('baseline', 'baseline'), ('fixed', bug)):
                    revision = revisions[selected]
                    scope = cap.validated_quality_spill_scope((bug,), phase, revision, receipts[selected])
                    value = cap.bound_quality_spill_metadata(module, case, 'new', 24, meta, scope=scope, runner=scope.runner)
                    expected = copy.deepcopy(meta)
                    expected['table_service_overrides'].update(EnableSpillingNodes='None', SpillingServiceConfig=cap.LOCAL_SERVICE)
                    policy = value.pop('spill_payload_file_policy')
                    check(value == expected, 'only_two_bounded_configuration_changes:' + case + ':' + phase)
                    check(policy['production_revision'] == revision and policy['production_phase'] == phase
                          and policy['build_receipt_sha256'] == receipts[selected]['build_receipt_sha256']
                          and policy['runner_sha256'] == receipts[selected]['binary_provenance']['sha256']
                          and policy['runtime_spill_occurrence_observed'] is False
                          and policy['cap_rejection_counts_as_success'] is False,
                          'actual_validated_receipt_policy:' + case + ':' + phase)
                    for reason, kwargs in (('missing_scope', {'scope': None, 'runner': scope.runner}),
                                           ('wrong_runner', {'scope': scope, 'runner': '/invalid-runner'})):
                        try:
                            cap.bound_quality_spill_metadata(module, case, 'new', 24, meta, **kwargs)
                        except ValueError:
                            check(True, reason + ':' + case + ':' + phase)
                        else:
                            check(False, reason + ':' + case + ':' + phase)
            for bad_revision in (q28['old_revision'], new_cap['_FIXED_REVISIONS']['Q14'], '0' * 40):
                try:
                    cap.validated_quality_spill_scope(('Q28',), 'fixed', bad_revision, receipts['Q28'])
                except ValueError:
                    check(True, 'unreviewed_Q28_revision_rejected:' + bad_revision[:8])
                else:
                    check(False, 'unreviewed_Q28_revision_accepted')

            q14 = importlib.import_module('quality_join_shuffle_checks')
            from optimizer_mode_contract import optimizer_mode_configuration
            fixture = ast.parse(actual_read(ROOT / 'proposed-quality-amendments/Q14_join_bound_source_checks_20261028.py'))
            scope = {'m': q14, 'json': json}
            exec(compile(ast.Module(body=[n for n in fixture.body if isinstance(n, ast.FunctionDef)
                       and n.name in ('graph', 'render')], type_ignores=[]), '<RAM-only-reviewed-graph-functions>', 'exec'), scope)
            digest = sha(read(ROOT / 'quality_join_shuffle_checks.py'))
            expected_profile = manifest['profiles']['Q14']['after_sha256']
            def q14_record(case, phase):
                selected = 'baseline' if phase == 'baseline' else 'Q14'
                graph, _ = scope['graph'](case, phase == 'fixed')
                record = q14.check_physical_ast(case, scope['render'](graph))
                settings = 'EnableNewRBO: true\nEnableFallbackToYqlOptimizer: false\n'
                record.update(copy.deepcopy(receipts[selected]))
                record.update(case=case, bug='Q14', mode='new', rows=24, repetition=0, phase=phase,
                    revision=revisions[selected], generator_module=q14.__name__, generator_sha256=digest,
                    generator_version=q14.VERSION, quality_profile_sha256=expected_profile, settings_sha256=sha(settings.encode()),
                    schema_sha256=sha(b'metadata-only'), fixture_sha256=[sha(b'metadata-only')], query_sha256=sha(case.encode()),
                    validation_sha256=[], query_syntax='yql', node_count=4, semantic_oracle_pass=True,
                    independent_four_node_bag_oracle=True, exit_code=0)
                record.update(optimizer_mode_configuration(settings, 'new', 'yql', {'YqlSelect': 'force'}))
                validated = cap.validated_quality_spill_scope(('Q14',), phase, revisions[selected], record)
                value = cap.bound_quality_spill_metadata(q14, case, 'new', 24, q14.metadata(case,24),
                                                         scope=validated, runner=validated.runner)
                record.update({key: value[key] for key in ('table_service_overrides', 'spill_payload_file_policy')})
                return record
            for case in q14.case_specs():
                before, after = q14_record(case, 'baseline'), q14_record(case, 'fixed')
                accept = lambda left, right: q14.pair_observations(left, right, revisions['baseline'], revisions['Q14'], digest, expected_profile)
                check(accept(before, after), 'final_composed_Q14_bounded_pair:' + case)
                for side in ('baseline', 'fixed'):
                    for mutation in ('profile59', 'cap_boolean', 'cap_float', 'policy_revision', 'receipt_hash',
                                     'source_hash', 'mask_all', 'original_config', 'missing_binary_field', 'missing_policy', 'shape'):
                        left, right = copy.deepcopy(before), copy.deepcopy(after)
                        bad = left if side == 'baseline' else right
                        if mutation == 'profile59': bad['quality_profile_sha256'] = manifest['profiles']['Q14']['ledger59_before_sha256']
                        elif mutation == 'cap_boolean': bad['spill_payload_file_policy']['cap_bytes'] = True
                        elif mutation == 'cap_float': bad['spill_payload_file_policy']['cap_bytes'] = 1.0
                        elif mutation == 'policy_revision': bad['spill_payload_file_policy']['production_revision'] = '0' * 40
                        elif mutation == 'receipt_hash': bad['build_receipt_sha256'] = '0' * 64
                        elif mutation == 'source_hash': bad['spill_payload_file_policy']['source_bindings_sha256'] = '0' * 64
                        elif mutation == 'mask_all': bad['table_service_overrides']['EnableSpillingNodes'] = 'All'
                        elif mutation == 'original_config': bad['table_service_overrides'] = q14.metadata(case,24)['table_service_overrides']
                        elif mutation == 'missing_binary_field': bad['binary_provenance'].pop('inode')
                        elif mutation == 'missing_policy': bad.pop('spill_payload_file_policy')
                        elif mutation == 'shape': bad['q14_join_inputs'] = []
                        check(accept(left,right) is False, 'Q14_negative:' + case + ':' + side + ':' + mutation)
                check(before['quality_acceptance_pass'] is False and after['quality_acceptance_pass'] is False,
                      'Q14_pair_cannot_grant_whole_finding_quality:' + case)
    finally:
        sys.meta_path.remove(finder)

    for name, expected in manifest['before_images'].items():
        path = PROJECT / name
        check(not path.exists() if expected is None else sha(actual_read(path)) == expected,
              'live59_still_unchanged:' + path.name)
    for name, expected in artifact_pins.items():
        check(sha(actual_read(ROOT / name)) == expected, 'frozen_report_unchanged:' + name)
    report = {
        'schema': 'quality_idle60_binding_revision_independent_review_v1',
        'status': 'source_and_metadata_approved_actual_runs_pending',
        'artifacts': artifact_pins, 'checks_passed': len(checks), 'checks': checks,
        'final_after_images': manifest['after_images'], 'profile_source_counts': profile_sizes,
        'audit_counts': {'source_images': 720, 'audited_production_revisions': 15, 'independent_main_children': 14,
                        'spill_cells': 82, 'receipt_backed_cell_phases': 164, 'profiles': 28, 'Q14_positive_pairs': 6,
                        'Q14_negative_pairs': 132},
        'scope': [
            'All 48 audited production files were compared at baseline and each of 14 exact fixed revisions, including the new Q28 child.',
            'The 82 cells, source metadata digests, 28 dependency keys and spill policy functions remain unchanged; only reviewed source hashes and Q28 revision change.',
            'Q14 final pairs require exact bounded configuration, source/receipt identity, four-node semantic evidence and join-specific descriptors; all six SQL pairs and twelve direct contracts remain mandatory.',
            'Canonical installed-path recursive hashes were independently rebuilt for all 28 profiles; native59 is bound by those profiles, not by an invented cap dependency.',
            'Q08 and Q02 records created under ledger59 remain immutable point-in-time evidence. Future ledger60 batches require fresh profiles and same-profile baseline/fixed pairs.',
            'The exact eight-patch composition must be installed atomically at an idle boundary. Intermediate dependency snapshots are not executable states.',
            'Q01 semantic/path/allocation/raw evidence remains separated from optimization acceptance; Q28 original26, extra4, SQL8 and partial SDK7 remain distinct.',
        ],
        'limitations': ['No native or compilation evidence is created by these metadata checks.',
                        'Existing spill cap rejection remains a test failure; this review does not assert actual spill occurrence.',
                        'Q01 and Q28 broader performance acceptance remains pending the existing finite evidence plan.'],
        'live_edits': False, 'native_build_toolchain_executed': False, 'git_mutation': False,
        'rendered_workloads_or_synthetic_records_persisted': False,
        'script_sha256': sha(actual_read(Path(__file__))),
    }
    out = ROOT / 'verification/quality-idle60-binding-revision-independent-review-20261028.json'
    out.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'checks_passed':len(checks), 'report_sha256':sha(out.read_bytes())}))


if __name__ == '__main__':
    main()
