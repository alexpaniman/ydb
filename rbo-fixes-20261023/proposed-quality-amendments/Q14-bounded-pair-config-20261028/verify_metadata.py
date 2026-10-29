"""Source-only Q14 configuration contracts; all synthetic evidence stays in RAM."""
import ast
import copy
import hashlib
import json
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.dont_write_bytecode = True
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]


def main():
    checked = 0

    def check(value):
        nonlocal checked
        if value is not True:
            raise AssertionError('Metadata contract failed at ordinal ' + str(checked + 1))
        checked += 1

    digest = lambda raw: hashlib.sha256(raw).hexdigest()
    images = json.loads((HERE / 'source-images.json').read_text())
    virtual = {}
    for name, value in images['before'].items():
        check(digest((ROOT / name).read_bytes()) == value)
        raw = (HERE / name).read_bytes()
        check(digest(raw) == images['after'][name])
        virtual[(ROOT / name).resolve()] = raw

    def load(name, raw):
        module = ModuleType(name)
        module.__file__ = str(ROOT / (name + '.py'))
        exec(compile(raw, module.__file__, 'exec'), module.__dict__)
        return module

    original = load('quality_join_shuffle_checks', (ROOT / 'quality_join_shuffle_checks.py').read_bytes())
    candidate = load('quality_join_shuffle_checks', virtual[ROOT / 'quality_join_shuffle_checks.py'])
    cap = load('quality_spill_bounds', virtual[ROOT / 'quality_spill_bounds.py'])
    oldcap = load('quality_spill_bounds', (ROOT / 'quality_spill_bounds.py').read_bytes())
    changed = [key for key in cap._SOURCE_BINDINGS if cap._SOURCE_BINDINGS[key] != oldcap._SOURCE_BINDINGS[key]]
    check(changed == ['rbo-fixes-20261023/quality_join_shuffle_checks.py'])
    check(len(cap._SOURCE_BINDINGS) == len(oldcap._SOURCE_BINDINGS) == 28)
    check(cap._CELLS == oldcap._CELLS and cap._FIXED_REVISIONS == oldcap._FIXED_REVISIONS)
    check(cap.LOCAL_SERVICE == oldcap.LOCAL_SERVICE and cap.VERSION == oldcap.VERSION)

    functions = lambda data: {item.name: ast.dump(item, include_attributes=False)
                              for item in ast.parse(data).body if isinstance(item, ast.FunctionDef)}
    old_functions = functions((ROOT / 'quality_join_shuffle_checks.py').read_bytes())
    new_functions = functions(virtual[ROOT / 'quality_join_shuffle_checks.py'])
    for name, body in old_functions.items():
        if name != 'pair_observations':
            check(new_functions[name] == body)
    check(set(new_functions) - set(old_functions) == {'_bounded_pair_configuration_matches'})
    check(digest((ROOT / 'quality_rule_cpp_generator.py').read_bytes()) ==
          'e732ca4705744697e0ff597ed930d6aa21403a9f3aa47f6dcede5f2d56ec2cca')
    check(original.recipes(24) == candidate.recipes(24))
    for case in candidate.case_specs():
        check(original.metadata(case, 24) == candidate.metadata(case, 24))
        check(original.schema_and_seeds(24, case) == candidate.schema_and_seeds(24, case))
        check(original.expected_rows(case, 24) == candidate.expected_rows(case, 24))

    # Reuse the reviewed observer's graph fixtures without executing its old
    # orchestration or writing any rendered expression.
    old_tests = ROOT / 'proposed-quality-amendments/Q14_join_bound_source_checks_20261028.py'
    fixture_tree = ast.parse(old_tests.read_bytes())
    fixture_nodes = [node for node in fixture_tree.body
                     if isinstance(node, ast.FunctionDef) and node.name in ('graph', 'render')]
    namespace = {'m': candidate, 'json': json}
    exec(compile(ast.Module(body=fixture_nodes, type_ignores=[]), '<reviewed-observer-fixtures>', 'exec'), namespace)

    from optimizer_mode_contract import optimizer_mode_configuration
    import quality_pairing
    source_digest = images['after']['quality_join_shuffle_checks.py']
    base, revision = cap._BASE_REVISION, cap._FIXED_REVISIONS['Q14']
    receipts = {}
    for phase, selected_revision in (('baseline', base), ('fixed', revision)):
        worktree = ROOT / ('metadata-only-' + phase)
        binary = {'resolved_path': str(worktree / 'ydb/tests/tools/kqprun/kqprun'),
                  'sha256': digest(('fake binary ' + phase).encode()), 'size_bytes': 1,
                  'mtime_ns': 2, 'ctime_ns': 3, 'inode': 4, 'device': 5}
        receipt = {'schema_version': 'successful_runner_build_v1', 'revision': selected_revision,
                   'build': {'exit_code': 0, 'target': 'ydb/tests/tools/kqprun'},
                   'binary': binary, 'worktree': str(worktree)}
        path = ROOT / 'verification' / ('metadata-only-q14-' + phase + '.json')
        raw = json.dumps(receipt, sort_keys=True).encode()
        virtual[path] = raw
        receipts[phase] = (path, raw, binary)

    real_read = Path.read_bytes

    def read(path):
        return virtual[path.resolve()] if path.resolve() in virtual else real_read(path)

    def record(case, fixed, profile):
        phase, selected_revision = ('fixed', revision) if fixed else ('baseline', base)
        path, raw, binary = receipts[phase]
        graph, _ = namespace['graph'](case, fixed)
        result = candidate.check_physical_ast(case, namespace['render'](graph))
        settings = 'EnableNewRBO: true\nEnableFallbackToYqlOptimizer: false\n'
        result.update(case=case, bug='Q14', mode='new', rows=24, repetition=0,
                      phase=phase, revision=selected_revision,
                      generator_module='quality_join_shuffle_checks', generator_sha256=source_digest,
                      generator_version=candidate.VERSION, quality_profile_sha256=profile,
                      settings_sha256=digest(settings.encode()), schema_sha256=digest(b'fake schema'),
                      fixture_sha256=[digest(b'fake fixture')], query_sha256=digest(case.encode()),
                      validation_sha256=[], query_syntax='yql', node_count=4,
                      binary_provenance_verified=True, binary_provenance_version='successful_runner_build_v1',
                      binary_provenance=copy.deepcopy(binary), build_receipt_path=str(path),
                      build_receipt_sha256=digest(raw), semantic_oracle_pass=True,
                      independent_four_node_bag_oracle=True, exit_code=0)
        result.update(optimizer_mode_configuration(settings, 'new', 'yql', {'YqlSelect': 'force'}))
        scope = cap.validated_quality_spill_scope(('Q14',), phase, selected_revision, result)
        module = SimpleNamespace(__name__='quality_join_shuffle_checks', __file__=candidate.__file__,
                                 _verification_source_sha256=source_digest)
        bounded = cap.bound_quality_spill_metadata(module, case, 'new', 24, candidate.metadata(case, 24),
                                                  scope=scope, runner=scope.runner)
        result['table_service_overrides'] = bounded['table_service_overrides']
        result['spill_payload_file_policy'] = bounded['spill_payload_file_policy']
        return result

    with patch.dict(sys.modules, {'quality_spill_bounds': cap}), patch.object(Path, 'read_bytes', read):
        profile = quality_pairing.profile_fingerprint(['quality_join_shuffle_checks'])
        for case in candidate.case_specs():
            before, after = record(case, False, profile), record(case, True, profile)
            check(original.pair_observations(before, after, base, revision, source_digest, profile) is False)
            check(candidate.pair_observations(before, after, base, revision, source_digest, profile))
            for phase in ('baseline', 'fixed'):
                for mutation in ('unbounded', 'cap_zero', 'cap_larger', 'mask_all', 'missing_policy',
                                 'extra_policy', 'wrong_policy_revision', 'wrong_policy_phase',
                                 'wrong_policy_receipt', 'wrong_policy_runner', 'wrong_source_hash',
                                 'wrong_metadata_hash', 'runtime_spill_claim', 'cap_rejection_success',
                                 'boolean_cap', 'float_cap', 'integer_enabled', 'integer_occurrence',
                                 'wrong_receipt_hash', 'missing_receipt', 'wrong_binary',
                                 'wrong_provenance_version', 'wrong_row_count', 'wrong_revision',
                                 'wrong_phase', 'wrong_profile', 'wrong_generator', 'wrong_shape'):
                    left, right = copy.deepcopy(before), copy.deepcopy(after)
                    bad = left if phase == 'baseline' else right
                    config, policy = bad['table_service_overrides'], bad['spill_payload_file_policy']
                    if mutation == 'unbounded': bad['table_service_overrides'] = candidate.metadata(case, 24)['table_service_overrides']
                    elif mutation == 'cap_zero': config['SpillingServiceConfig'] = cap.LOCAL_SERVICE.replace('Size: 1', 'Size: 0')
                    elif mutation == 'cap_larger': config['SpillingServiceConfig'] = cap.LOCAL_SERVICE.replace('Size: 1', 'Size: 2')
                    elif mutation == 'mask_all': config['EnableSpillingNodes'] = 'All'
                    elif mutation == 'missing_policy': bad.pop('spill_payload_file_policy')
                    elif mutation == 'extra_policy': policy['unreviewed'] = True
                    elif mutation == 'wrong_policy_revision': policy['production_revision'] = '0' * 40
                    elif mutation == 'wrong_policy_phase': policy['production_phase'] = 'other'
                    elif mutation == 'wrong_policy_receipt': policy['build_receipt_sha256'] = '0' * 64
                    elif mutation == 'wrong_policy_runner': policy['runner_sha256'] = '0' * 64
                    elif mutation == 'wrong_source_hash': policy['source_bindings_sha256'] = '0' * 64
                    elif mutation == 'wrong_metadata_hash': policy['source_metadata_sha256'] = '0' * 64
                    elif mutation == 'runtime_spill_claim': policy['runtime_spill_occurrence_observed'] = True
                    elif mutation == 'cap_rejection_success': policy['cap_rejection_counts_as_success'] = True
                    elif mutation == 'boolean_cap': policy['cap_bytes'] = True
                    elif mutation == 'float_cap': policy['cap_bytes'] = 1.0
                    elif mutation == 'integer_enabled': policy['local_service_enabled'] = 1
                    elif mutation == 'integer_occurrence': policy['runtime_spill_occurrence_observed'] = 0
                    elif mutation == 'wrong_receipt_hash': bad['build_receipt_sha256'] = '0' * 64
                    elif mutation == 'missing_receipt': bad['build_receipt_path'] = str(ROOT / 'absent-metadata-only-receipt')
                    elif mutation == 'wrong_binary': bad['binary_provenance']['sha256'] = '0' * 64
                    elif mutation == 'wrong_provenance_version': bad['binary_provenance_version'] = 'other'
                    elif mutation == 'wrong_row_count': bad['rows'] = 25
                    elif mutation == 'wrong_revision': bad['revision'] = '0' * 40
                    elif mutation == 'wrong_phase': bad['phase'] = 'other'
                    elif mutation == 'wrong_profile': bad['quality_profile_sha256'] = '0' * 64
                    elif mutation == 'wrong_generator': bad['generator_sha256'] = '0' * 64
                    elif mutation == 'wrong_shape': bad['q14_join_inputs'] = []
                    check(candidate.pair_observations(left, right, base, revision, source_digest, profile) is False)
            # Invalid settings on both sides still satisfy the old paired-field
            # equality check. The bounded configuration witness must reject them.
            for mutation in ('unbounded', 'cap_zero', 'cap_larger', 'mask_all', 'extra_setting', 'integer_boolean'):
                left, right = copy.deepcopy(before), copy.deepcopy(after)
                for bad in (left, right):
                    config = bad['table_service_overrides']
                    if mutation == 'unbounded': bad['table_service_overrides'] = candidate.metadata(case, 24)['table_service_overrides']
                    elif mutation == 'cap_zero': config['SpillingServiceConfig'] = cap.LOCAL_SERVICE.replace('Size: 1', 'Size: 0')
                    elif mutation == 'cap_larger': config['SpillingServiceConfig'] = cap.LOCAL_SERVICE.replace('Size: 1', 'Size: 2')
                    elif mutation == 'mask_all': config['EnableSpillingNodes'] = 'All'
                    elif mutation == 'extra_setting': config['UnreviewedSetting'] = True
                    elif mutation == 'integer_boolean': config['EnableQueryServiceSpilling'] = 1
                check(quality_pairing._matching(left, right))
                check(candidate.pair_observations(left, right, base, revision, source_digest, profile) is False)
            check(after['quality_acceptance_pass'] is False and before['quality_acceptance_pass'] is False)

        for name in cap._SOURCE_BINDINGS:
            path = ROOT.parent / name
            saved = virtual.get(path)
            virtual[path] = b'changed source metadata control'
            check(candidate.pair_observations(before, after, base, revision, source_digest, profile) is False)
            if saved is None:
                del virtual[path]
            else:
                virtual[path] = saved

    for name, value in images['before'].items():
        check(digest((ROOT / name).read_bytes()) == value)
    report = {'schema': 'Q14_bounded_pair_configuration_metadata_contracts_v1',
              'status': 'source_and_metadata_passed_native_pending', 'checks_passed': checked,
              'cases': 6, 'direct12_unchanged': True, 'native_runs': 0, 'live_edits': False,
              'new_workloads': False, 'synthetic_records_persisted': False,
              'full_quality_acceptance_enabled': False, 'after_images': images['after'],
              'canonical_after_profile_sha256': profile}
    return report


if __name__ == '__main__':
    print(json.dumps(main(), sort_keys=True))
