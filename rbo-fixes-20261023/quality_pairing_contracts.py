"""Synthetic harness contracts only: no database, compiler, queries or plans."""
import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import quality_pairing as pairing
import verify_series as driver


def _record(bug='Q08', phase='fixed'):
    case = 'q08_nonunit_input' if bug == 'Q08' else 'q09_scalar_nonempty'
    fixed = phase == 'fixed'
    record = {'bug': bug, 'case': case, 'mode': 'new', 'rows': 24, 'repetition': 0,
              'phase': phase, 'revision': ('f' if fixed else 'b') * 40,
              'generator_module': 'quality_statistics_checks', 'generator_version': 'contract',
              'generator_sha256': '1' * 64, 'settings_sha256': '2' * 64, 'schema_sha256': '3' * 64,
              'fixture_sha256': ['4' * 64], 'query_sha256': '5' * 64, 'validation_sha256': [],
              'query_syntax': 'yql', 'node_count': 1, 'table_service_overrides': {},
              'quality_profile_sha256': 'd' * 64, 'exit_code': 0, 'semantic_oracle_pass': True,
              'binary_provenance_verified': True, 'binary_provenance': {'sha256': '6' * 64},
              'build_receipt_sha256': '7' * 64, 'query_plan_parsed': True,
              'query_plan_sha256': ('8' if fixed else '9') * 64}
    if bug == 'Q08':
        record.update(quality_assertions={'retained_filter_above_window': True, 'local_selectivity_applied_once': fixed},
                      optimizer_estimates=[{'input_rows': 400, 'output_rows': 160 if fixed else 64, 'local_selectivity': .4}])
    else:
        record.update(quality_assertions={'scalar_final_present': True, 'scalar_estimate_exactly_one': fixed},
                      optimizer_estimates=[{'rows': 1 if fixed else 8, 'bytes': 16}])
    return record


def run_contract_checks():
    assertions = 0
    def check(value):
        nonlocal assertions
        assertions += 1
        if not value:
            raise AssertionError('Quality harness contract failed')
    before, after = _record(phase='baseline'), _record()
    check(driver.record_verdict(after) == 'functional')
    check(driver.record_verdict(dict(after, native_batch_guard_pass=False)) == 'failure')
    check(driver.record_verdict(dict(after, native_batch_guard_pass=True)) == 'functional')
    ast_required = dict(after, correctness_physical_ast_required=True)
    check(driver.record_verdict(ast_required) == 'failure')
    ast_good = dict(ast_required, structural_oracle_pass=True, physical_ast_parsed=True, physical_ast_sha256='a' * 64)
    check(driver.record_verdict(ast_good) == 'functional')
    check(driver.record_verdict(dict(ast_good, structural_oracle_pass=False)) == 'failure')
    check(driver.record_verdict(dict(ast_good, physical_ast_sha256=None)) == 'failure')
    def evaluate(b=before, a=after):
        return pairing.pair_quality_records([b], [a], 'Q08', 'b' * 40, 'f' * 40,
            required_case_owners={after['case']: after['generator_module']}, required_profile_sha256='d' * 64)
    paired, success, status = evaluate()
    check(success and status == 'verified_quality' and paired[0]['quality_improvement_verified'])
    # Local pair success cannot stand in for the complete finding profile.
    for requirements in ({},
                         {'required_case_owners': {after['case']: after['generator_module']},
                          'required_profile_sha256': 'e' * 64},
                         {'required_case_owners': {after['case']: 'different'},
                          'required_profile_sha256': 'd' * 64},
                         {'required_case_owners': {after['case']: after['generator_module'],
                                                  'required_other_case': after['generator_module']},
                          'required_profile_sha256': 'd' * 64}):
        rows, success, status = pairing.pair_quality_records(
            [before], [after], 'Q08', 'b' * 40, 'f' * 40, **requirements)
        check(not success and status == 'verified_semantics_quality_pending'
              and rows[0]['finding_profile_coverage_complete'] is False)
    union_before, union_after = copy.deepcopy(before), copy.deepcopy(after)
    for item in (union_before, union_after):
        item.update(bug='Q11', case='q11_primary_range', generator_module='quality_union_checks',
                    quality_assertions={'predicate_reaches_both_access_paths': True},
                    row_sources=[{'range_expression_present': item['phase'] == 'fixed'}])
    union_requirements = {'required_case_owners': {'q11_primary_range': 'quality_union_checks'},
                          'required_profile_sha256': 'd' * 64}
    rows, success, status = pairing.pair_quality_records(
        [union_before], [union_after], 'Q11', 'b' * 40, 'f' * 40, **union_requirements)
    check(not success and status == 'verified_semantics_quality_pending'
          and 'additional_ir_admission_pending' in rows[0]['quality_pairing_reasons'])
    with patch('anonymous_cpp_verify.validate_fixed_admission', return_value=True):
        rows, success, status = pairing.pair_quality_records(
            [union_before], [union_after], 'Q11', 'b' * 40, 'f' * 40,
            ir_evidence={'synthetic': True}, **union_requirements)
        check(success and 'direct_ir_admission_evidence_sha256' in rows[0])
    for case, metric in (('q11_fallible_control', 'ensure_dag_nodes'),
                         ('q29_arithmetic_control', 'comparison_dag_nodes'),
                         ('q29_no_range_control', 'comparison_dag_nodes')):
        control = {'case': case, metric: 2, 'structural_assertions': {'preserved': True}}
        check(pairing._fixed_acceptance(control, control))
        check(not pairing._fixed_acceptance(control, dict(control, **{metric: 1})))
        missing_control = dict(control); missing_control.pop(metric)
        check(not pairing._fixed_acceptance(control, missing_control))
    guard_control = {'case': 'q29_guard_control', 'comparison_dag_nodes': 2,
                     'callables': {'Ensure': 2}, 'structural_assertions': {'preserved': True}}
    check(pairing._fixed_acceptance(guard_control, guard_control))
    check(not pairing._fixed_acceptance(guard_control, dict(guard_control, callables={'Ensure': 1})))
    scaling_before, scaling_after = copy.deepcopy(before), copy.deepcopy(after)
    for item in (scaling_before, scaling_after):
        item.pop('quality_assertions')
        item.update(bug='Q01', case='q01_rows_asc_overlap', generator_module='quality_window_checks',
                    structural_assertions={'count_has_no_frame_fold': item['phase'] == 'fixed'},
                    callables={'Fold': int(item['phase'] == 'baseline'), 'Fold1': 0},
                    # Input flags cannot replace measured resource admission.
                    allocation_scaling_verified=True, q01_resource_scaling_verified=True)
    scaling_requirements = {'required_case_owners': {'q01_rows_asc_overlap': 'quality_window_checks'},
                            'required_profile_sha256': 'd' * 64}
    rows, success, status = pairing.pair_quality_records(
        [scaling_before], [scaling_after], 'Q01', 'b' * 40, 'f' * 40, **scaling_requirements)
    check(not success and status == 'verified_semantics_quality_pending')
    check(rows[0]['q01_paired_structural_contract_pass'] is True
          and rows[0]['q01_structural_improvement_observed'] is True)
    check(rows[0]['q01_resource_scaling_verified'] is False
          and rows[0]['quality_acceptance_pass'] is False
          and rows[0]['quality_improvement_verified'] is False)
    check('required_q01_resource_scaling_evidence_pending' in rows[0]['quality_pairing_reasons'])
    check(driver.record_verdict(rows[0]) == 'functional')
    rows, success, status = pairing.pair_quality_records(
        [scaling_before], [dict(scaling_after, semantic_oracle_pass=False)],
        'Q01', 'b' * 40, 'f' * 40, **scaling_requirements)
    check(not success and status == 'native_failed')
    for field, replacement in [('generator_sha256', 'a' * 64), ('query_sha256', 'a' * 64),
                               ('quality_profile_sha256', None), ('node_count', 4),
                               ('fixture_sha256', []), ('generator_module', 'different')]:
        changed = dict(before, **{field: replacement})
        rows, success, status = evaluate(changed)
        check(not success and status == 'verified_semantics_quality_pending' and not rows[0]['quality_improvement_verified'])
    for changed in (dict(before, semantic_oracle_pass=False), dict(before, binary_provenance_verified=False),
                    dict(before, revision='c' * 40), dict(before, build_receipt_sha256=None)):
        rows, success, status = evaluate(changed)
        check(not success and status == 'verified_semantics_quality_pending')
    for changed in (dict(after, semantic_oracle_pass=False), dict(after, binary_provenance_verified=False),
                    dict(after, revision='c' * 40), dict(after, build_receipt_sha256=None)):
        rows, success, status = evaluate(a=changed)
        check(not success and status == 'native_failed')
    for baselines, fixed in (([], [after]), ([before, before], [after]), ([before], [after, after])):
        rows, success, status = pairing.pair_quality_records(baselines, fixed, 'Q08', 'b' * 40, 'f' * 40)
        check(not success and not all(row['quality_improvement_verified'] for row in rows))
    changed = dict(after, quality_assertions={'local_selectivity_applied_once': False})
    check(evaluate(a=changed)[1] is False)
    unchanged = copy.deepcopy(before); unchanged.update(quality_assertions=after['quality_assertions'], optimizer_estimates=after['optimizer_estimates'])
    check(evaluate(unchanged)[1] is False)
    missing = dict(after); missing.pop('query_plan_sha256')
    check(evaluate(a=missing)[1] is False)
    check(driver.native_command(Path('/unused'), 'fixed', ['C37'], 24)[-2:] == ['new', 'legacy'])
    command = driver.native_command(Path('/unused'), 'fixed', ['Q08'], 24, ['quality_statistics_checks'], 3)
    check(command[-4:] == ['--generator-module', 'quality_statistics_checks', '--repetitions', '3'])
    guard = {'verification_limitation': 'Correctness guard: unsupported residual joins remain rejected.'}
    driver.clear_resolved_transient_limit(guard)
    check('verification_limitation' in guard)
    transient = {'verification_limitation': 'Build watchdog stopped at budget; no native verdict. Worker exception pending.'}
    driver.clear_resolved_transient_limit(transient)
    check('verification_limitation' not in transient and len(transient['resolved_verification_limitations']) == 1)
    stale_sentence = ('Earlier native checks passed before exact executable receipts were required; '
                      'rebuild and rerun are required for final acceptance.')
    stale = {'commit': 'f' * 40, 'provenance_reverification_required': True,
             'verification_limitation': 'Substantive coverage limit remains. ' + stale_sentence}
    driver.clear_resolved_transient_limit(stale, [after])
    check(stale['provenance_reverification_required'])
    driver.clear_resolved_transient_limit(stale, [dict(after, native_batch_guard_pass=True)])
    check('provenance_reverification_required' not in stale and stale['verification_limitation'] == 'Substantive coverage limit remains.')
    plain = {'commit': 'f' * 40, 'provenance_reverification_required': True, 'verification_limitation': stale_sentence}
    driver.clear_resolved_transient_limit(plain, [dict(after, native_batch_guard_pass=True)])
    check('provenance_reverification_required' not in plain and 'verification_limitation' not in plain)
    bad = {'commit': 'f' * 40, 'provenance_reverification_required': True, 'verification_limitation': stale_sentence}
    driver.clear_resolved_transient_limit(bad, [dict(after, native_batch_guard_pass=True, build_receipt_sha256=None)])
    check(bad['provenance_reverification_required'] and bad['verification_limitation'] == stale_sentence)
    check(pairing.generator_modules('Q01') == ['quality_window_checks', 'quality_sum_checks', 'quality_minmax_checks', 'quality_range_minmax_checks'])
    check(pairing.generator_modules('Q15') == ['quality_partition_subset_checks'])
    check(pairing._improvement('q01_minmax_optional_overlap', {'callables': {'Fold1': 1}},
        {'callables': {'Fold1': 0}, 'structural_assertions': {'no_general_frame_fold': True}}))
    check(not pairing._improvement('q01_minmax_optional_overlap', {'callables': {'Fold1': 0}},
        {'callables': {'Fold1': 0}, 'structural_assertions': {'no_general_frame_fold': True}}))
    control = {'case': 'q01_minmax_running_control', 'callables': {'Fold1': 1},
               'structural_assertions': {'physical_plan_parsed': True}}
    check(pairing._fixed_acceptance(control, control))
    check(not pairing._fixed_acceptance(control, dict(control, callables={'Fold1': 0})))
    range_before = {'case': 'q01_range_minmax_int64_optional_asc_overlap',
                    'range_minmax_calls': 0, 'callables': {'Fold1': 1}}
    range_after = {'case': range_before['case'], 'optimization_acceptance_case': True,
                   'range_minmax_calls': 1, 'callables': {'Fold1': 0},
                   'structural_assertions': {'physical_plan_parsed': True,
                       'incremental_range_callable_present': True,
                       'remaining_general_fold_policy': True, 'minmax_only_collector_bypassed': True}}
    check(pairing._fixed_acceptance(range_before, range_after))
    check(pairing._improvement(range_after['case'], range_before, range_after))
    check(not pairing._fixed_acceptance(range_before, dict(range_after, range_minmax_calls=0)))
    check(not pairing._improvement(range_after['case'], range_before, dict(range_after, range_minmax_calls=0)))
    check(not pairing._fixed_acceptance(dict(range_before, range_minmax_calls=1), range_after))
    check(not pairing._fixed_acceptance(range_before, dict(range_after, structural_assertions={'physical_plan_parsed': True})))
    check(not pairing._improvement(range_after['case'], range_before, dict(range_after, optimization_acceptance_case=False)))
    fallback = dict(range_after, case='q01_range_minmax_float_value_asc',
                    optimization_acceptance_case=False, range_minmax_calls=0, callables={'Fold1': 1},
                    structural_assertions={'physical_plan_parsed': True, 'unsupported_scope_not_admitted': True})
    check(pairing._fixed_acceptance(range_before, fallback))
    check(not pairing._fixed_acceptance(range_before, dict(fallback, callables={'Fold1': 0})))
    check(pairing._fixed_acceptance(range_before, dict(range_after, case='q01_minmax_range_control')))
    ir_before, ir_after = copy.deepcopy(before), copy.deepcopy(after)
    for item in (ir_before, ir_after):
        item.update(bug='Q15', case='q15_strict_superset', generator_module='quality_partition_subset_checks',
                    quality_assertions={'expected_shuffle_count': True}, additional_ir_admission_required=True,
                    callables={'DqCnHashShuffle': 2 if item['phase'] == 'baseline' else 1})
    rows, success, status = pairing.pair_quality_records([ir_before], [ir_after], 'Q15', 'b' * 40, 'f' * 40)
    check(not success and status == 'verified_semantics_quality_pending' and 'additional_ir_admission_pending' in rows[0]['quality_pairing_reasons'])
    with patch('anonymous_cpp_verify.validate_fixed_admission', return_value=True):
        rows, success, status = pairing.pair_quality_records([ir_before], [ir_after], 'Q15', 'b' * 40, 'f' * 40,
            ir_evidence={'synthetic': True},
            required_case_owners={ir_after['case']: ir_after['generator_module']}, required_profile_sha256='d' * 64)
        check(success and 'direct_ir_admission_evidence_sha256' in rows[0])
        broken = dict(ir_after, quality_assertions={'expected_shuffle_count': False})
        check(not pairing.pair_quality_records([ir_before], [broken], 'Q15', 'b' * 40, 'f' * 40,
            ir_evidence={'synthetic': True},
            required_case_owners={ir_after['case']: ir_after['generator_module']}, required_profile_sha256='d' * 64)[1])
    with tempfile.TemporaryDirectory(prefix='rbo-native-guard-contract-') as temporary:
        folder = Path(temporary)
        (folder / 'verification').mkdir()
        original = folder / 'verification' / ('fixed-Q08-' + 'f' * 12 + '.jsonl')
        original.write_text(json.dumps({'old_record': True}) + '\n')
        observed = []
        def fake_scope(command, worktree, fds, **options):
            observed.append(options)
            with original.open('a') as stream:
                stream.write(json.dumps(_record()) + '\n')
            return {'exit_code': 0, 'cleanup_complete': True, 'watchdog_stopped': False,
                    'stdout_overflow': True, 'diagnostic_sha256': 'a' * 64}, b'bounded summary'
        with patch.object(driver, 'HERE', folder), patch.object(driver, 'run_anonymous', side_effect=fake_scope):
            completed, paths, groups = driver.invoke_native('/unused', 'fixed', ['Q08'], 'f' * 40, 24)
            check(completed.returncode == 0 and len(groups['Q08']) == 1 and groups['Q08'][0]['native_batch_guard_pass'])
            check(paths['Q08'].name.startswith('guarded-') and paths['Q08'].is_file())
            check(observed[0]['max_scope_gib'] == 24 and observed[0]['min_host_gib'] == 36
                  and observed[0]['compiler_environment'] is False and observed[0]['timeout'] >= 24 * 3600)
            check('native_batch_guard_pass' not in original.read_text())
        def stopped_scope(*args, **options):
            with original.open('a') as stream:
                stream.write(json.dumps(_record()) + '\n')
            return {'exit_code': 75, 'cleanup_complete': True, 'watchdog_stopped': True,
                    'stop_reason': 'direct_memory_budget'}, b''
        with patch.object(driver, 'HERE', folder), patch.object(driver, 'run_anonymous', side_effect=stopped_scope):
            completed, paths, groups = driver.invoke_native('/unused', 'fixed', ['Q08'], 'f' * 40, 24)
            check(completed.returncode == 75 and len(groups['Q08']) == 1 and not groups['Q08'][0]['native_batch_guard_pass'])
            check(driver.record_verdict(groups['Q08'][0]) == 'failure')

    # Run the orchestration with every external action mocked. Temporary JSON
    # files are fabricated harness state, never source or workload artifacts.
    with tempfile.TemporaryDirectory(prefix='rbo-quality-driver-contract-') as temporary:
        folder = Path(temporary)
        (folder / 'verification').mkdir(); (folder / 'cases').mkdir()
        state = {'base': 'b' * 40, 'production_sources_frozen': True, 'authorized_build_workers': 8}
        (folder / 'work-state.json').write_text(json.dumps(state))
        def reset_cases():
            for bug in ('Q08', 'Q09'):
                (folder / 'cases' / (bug + '.json')).write_text(json.dumps({
                    'id': bug, 'commit': 'f' * 40, 'status': 'prepared_unverified'}))
        reset_cases()
        builds, switches = [], []
        def fake_output(command, **kwargs):
            return b'' if command[1] == 'status' else 'b' * 40 + '\n'
        def fake_run(command, **kwargs):
            switches.append(command[-1])
            return SimpleNamespace(returncode=0)
        def fake_build(worktree, target, workers):
            builds.append((target, workers))
            return SimpleNamespace(returncode=0, stdout='', stderr=''), {}
        def fake_native(worktree, phase, bugs, revision, rows, modules=(), repetitions=None):
            paths = {bug: folder / 'verification' / (phase + '-' + bug + '.jsonl') for bug in bugs}
            groups = {bug: [_record(bug, phase)] for bug in bugs}
            return SimpleNamespace(returncode=0, stdout='', stderr=''), paths, groups
        owners = lambda bug, modules, rows: {_record(bug)['case']: 'quality_statistics_checks'}
        argv = ['verify_series.py', '--worktree', '/unused', '--bugs', 'Q08', 'Q09', '--build-workers', '8']
        with patch.object(driver, 'HERE', folder), patch.object(driver, 'emit'), \
             patch.object(driver.subprocess, 'check_output', side_effect=fake_output), \
             patch.object(driver.subprocess, 'run', side_effect=fake_run), \
             patch.object(driver, 'run_build', side_effect=fake_build), \
             patch.object(driver, 'invoke_native', side_effect=fake_native), \
             patch.object(driver, 'selected_cases', side_effect=owners), \
             patch.object(driver, 'profile_fingerprint', return_value='d' * 64), \
             patch('binary_provenance.write_build_receipt', return_value=folder / 'receipt.json'), \
             patch('sys.argv', argv):
            # Q09 needs fixed direct statistics evidence in addition to SQL.
            check(driver.main() == 2)
            check(len(builds) == 3 and all(workers == 8 for _, workers in builds))
            check(switches == ['b' * 40, 'f' * 40, 'f' * 40])
            check(json.loads((folder / 'cases/Q08.json').read_text())['status'] == 'verified_quality')
            check(json.loads((folder / 'cases/Q09.json').read_text())['status'] == 'verified_semantics_quality_pending')
            reset_cases(); builds.clear(); switches.clear()
            with patch.object(driver, 'run_build', return_value=(SimpleNamespace(returncode=1, stdout='', stderr=''), {'watchdog_stopped': True})):
                check(driver.main() == 1)
                check(all(json.loads((folder / 'cases' / (bug + '.json')).read_text())['status'] == 'prepared_unverified' for bug in ('Q08', 'Q09')))
            reset_cases(); builds.clear()
            def failed_baseline_semantics(*args, **kwargs):
                native, paths, groups = fake_native(*args, **kwargs)
                if args[1] == 'baseline' and args[2] == ['Q08']:
                    groups['Q08'][0]['semantic_oracle_pass'] = False
                return native, paths, groups
            with patch.object(driver, 'invoke_native', side_effect=failed_baseline_semantics):
                check(driver.main() == 2 and len(builds) == 3)
                check(json.loads((folder / 'cases/Q08.json').read_text())['status'] == 'verified_semantics_quality_pending')
                check(json.loads((folder / 'cases/Q09.json').read_text())['status'] == 'verified_semantics_quality_pending')
            reset_cases()
            partial_path = folder / 'cases/Q08.json'
            partial_case = json.loads(partial_path.read_text()); partial_case['status'] = 'partial_prepared_unverified'
            partial_path.write_text(json.dumps(partial_case))
            check(driver.main() == 2)
            partial_case = json.loads(partial_path.read_text())
            check(partial_case['status'] == 'partial_verified_quality' and partial_case['implementation_scope_complete'] is False)
            correctness = folder / 'cases/C09.json'
            correctness.write_text(json.dumps({'id':'C09', 'commit':'f'*40, 'status':'prepared_unverified',
                'verification_limitation':'Build watchdog stopped; no native verdict. Worker exception pending.'}))
            builds.clear()
            with patch('sys.argv', ['verify_series.py', '--worktree', '/unused', '--bugs', 'C09', '--build-workers', '8']):
                check(driver.main() == 0 and len(builds) == 1)
                case = json.loads(correctness.read_text())
                check(case['status'] == 'verified' and 'verification_limitation' not in case)
    return {'status': 'synthetic_contracts_passed_native_unverified', 'assertions': assertions,
            'real_builds': 0, 'real_process_launches': 0, 'native_runs': 0,
            'coverage': ['profile/config/workload fingerprint mismatch', 'baseline failure separate from fixed failure',
                         'fixed provenance rejection', 'duplicate/missing pairs', 'semantic-only pending',
                         'one baseline build per invocation', 'approved workers forwarded',
                         'correctness dual-mode command retained', 'guard limitations preserved']}
