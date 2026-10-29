"""Stage A: closed finite-vector, path and raw-resource evidence checks.

No processes run and no rendered input is written. This stage cannot grant
optimization acceptance: the actual final lowering must first pass the path
pilot, and the later measured contract must be bound at native dispatch.
"""
import hashlib
import json
import math
from pathlib import Path

import native_verify as native
import anonymous_child_measurement as child_protocol
import quality_pairing as pairing
import quality_q01_scaling_checks as scaling
from quality_range_runtime_contract import validate_fixed_runtime
from verification_contract import record_verdict
from optimizer_mode_contract import optimizer_mode_matches_record

VERSION = 'q01_concrete_acceptance_stage_A_v1'
REVISIONS = {'baseline': 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2',
             'fixed': '3e2a57e559557a716bba6f42eccff44bbc2ed8d7'}
ROOT = Path(__file__).resolve().parent
GIB = 1024 ** 3
BINARY_FIELDS = {'resolved_path', 'sha256', 'size_bytes', 'mtime_ns', 'ctime_ns', 'inode', 'device'}


def digest(value):
    return hashlib.sha256(value).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def require(value, reason):
    if not value:
        raise ValueError(reason)


def integer(value, expected=None):
    return type(value) is int and value >= 0 and (expected is None or value == expected)


def hexdigest(value):
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def workload(case, rows):
    """Mirror the bound single-request driver's byte framing, in RAM only."""
    meta = scaling.metadata(case, rows)
    pragmas = dict(meta['pragmas'])
    pragmas['YqlSelect'] = 'force'
    query = ''.join('PRAGMA ' + key + '=' + native.g.quote(value) + '; '
                    for key, value in pragmas.items()) + scaling.recipes(rows)[case] + ';'
    schema, seeds = scaling.schema_and_seeds(rows, case)
    settings = native.config_with((native.REVIEW / 'new.conf').read_text(), meta['table_service_overrides'])
    return {'generator_sha256': digest(Path(scaling.__file__).read_bytes()),
            'query_sha256': digest(query.encode()), 'schema_sha256': digest(schema.encode()),
            'fixture_sha256': [digest((seed + ';').encode()) for seed in seeds],
            'settings_sha256': digest(settings.encode()), 'validation_sha256': []}


def manifest(rows, run_id):
    require(type(rows) is int and rows in scaling.ROWS, 'unreviewed_row_count')
    require(isinstance(run_id, str) and len(run_id) == 32
            and all(c in '0123456789abcdef' for c in run_id), 'invalid_run_identity')
    cases = scaling.finite_profile_cases()
    require(len(cases) == len(set(cases)) == 15, 'finite_profile_vector_changed')
    profiles = [{'case': case, 'cell': scaling.cell(case, rows), 'workload': workload(case, rows)}
                for case in cases]
    schedule = []
    for ordinal, case in enumerate(cases):
        for kind, indices in [('warmup', (0,)), ('measurement', range(5))]:
            for index in indices:
                phases = ('baseline', 'fixed') if (ordinal + index) % 2 == 0 else ('fixed', 'baseline')
                schedule.extend({'case': case, 'phase': phase, 'kind': kind, 'index': index}
                                for phase in phases)
    return {'schema_version': VERSION, 'rows': rows, 'run_id': run_id, 'revisions': REVISIONS,
            'profiles': profiles, 'schedule': schedule,
            'profile_sha256': pairing.profile_fingerprint(['quality_q01_scaling_checks']),
            'contract_sha256': digest(Path(__file__).read_bytes()),
            'superseded_proposal_sha256': digest((ROOT / 'verification' /
                'Q01-resource-scaling-admission-plan-20261027.json').read_bytes()),
            'supersession_scope': 'finite_profile_design_only_existing_oracles_and_gate_unchanged',
            'optimization_acceptance_enabled': False, 'canonical_SQL_cases_required': 652,
            'direct_runtime_cases_required': 42}


def _read_bound(path, expected):
    require(hexdigest(expected), 'artifact_digest_missing')
    raw = Path(path).read_bytes()
    require(digest(raw) == expected, 'artifact_changed')
    return json.loads(raw)


def _guard_and_receipt(record, revision):
    require(record.get('native_batch_guard_pass') is True, 'guard_missing')
    memory = _read_bound(record['native_batch_memory_evidence'], record['native_batch_memory_sha256'])
    require(integer(memory.get('exit_code'), 0) and memory.get('cleanup_complete') is True
            and memory.get('watchdog_stopped') is False and memory.get('stdout_overflow') is False,
            'guard_failed')
    require(integer(memory.get('max_scope_gib'), 24) and integer(memory.get('min_host_available_gib'), 36)
            and integer(memory.get('minimum_host_available_bytes'))
            and memory['minimum_host_available_bytes'] >= 36 * GIB, 'guard_budget_changed')
    for name in ('peak_scope_usage_bytes', 'peak_group_rss_bytes'):
        require(integer(memory.get(name)) and memory[name] <= 24 * GIB, 'guard_peak_invalid')
    receipt = _read_bound(record['build_receipt_path'], record['build_receipt_sha256'])
    require(receipt.get('schema_version') == 'successful_runner_build_v1'
            and receipt.get('revision') == revision and integer(receipt.get('build', {}).get('exit_code'), 0)
            and 'ydb/tests/tools/kqprun' in receipt.get('build', {}).get('target', '').split(),
            'receipt_revision_or_target')
    actual, saved = record.get('binary_provenance'), receipt.get('binary')
    require(isinstance(actual, dict) and isinstance(saved, dict)
            and set(actual) == set(saved) == BINARY_FIELDS, 'binary_identity_fields')
    require(hexdigest(actual['sha256']) and actual['size_bytes'] > 0
            and all(integer(actual[k]) and integer(saved[k]) for k in BINARY_FIELDS - {'resolved_path', 'sha256'})
            and all(actual[k] == saved[k] for k in BINARY_FIELDS - {'ctime_ns'}), 'binary_identity_changed')
    require(isinstance(receipt.get('worktree'), str) and Path(receipt['worktree']).is_absolute()
            and (Path(receipt['worktree']) / 'ydb/tests/tools/kqprun/kqprun').resolve()
                == Path(actual['resolved_path']), 'receipt_worktree_binding')


def _path(record, spec, phase):
    witness = record.get('q01_path_witness', {})
    require(witness.get('schema_version') == scaling.paths.VERSION and witness.get('complete') is True
            and witness.get('path_pass') is True and hexdigest(witness.get('physical_ast_sha256'))
            and witness['physical_ast_sha256'] == record.get('physical_ast', {}).get('physical_ast_sha256'),
            'path_missing_or_unbound')
    if spec['family'] == 'setup_scan':
        route, keys = 'setup', {'setup_has_no_window_pipeline'}
    elif phase == 'baseline':
        route, keys = 'baseline_general', {'collector_owns_frame_views', 'baseline_same_queue_frame_fold'}
    elif spec['kind'] == 'range' and spec['minmax']:
        mixed = bool(spec['collectors'] or spec.get('fallback'))
        route = 'fixed_range_mixed' if mixed else 'fixed_range_only'
        keys = {'two_distinct_result_fields', 'same_input_and_minmax_operations',
                'collector_branch_matches_recipe', 'result_members_owned_by_same_stream'}
        if mixed:
            keys |= {'same_stream_prepass_to_collector', 'same_bound_object',
                     'collector_owns_frame_views', 'same_queue_fallback_policy'}
        if spec['collectors']:
            keys.add('same_queue_prefix_endpoint_outputs')
    elif spec['minmax']:
        route, keys = 'fixed_rows_stacks', {'collector_owns_frame_views', 'same_stream_two_stack_chain',
                                          'same_queue_fallback_policy'}
        if spec['collectors']:
            keys.add('same_queue_prefix_endpoint_outputs')
    else:
        route, keys = 'fixed_existing_primitives', {'collector_owns_frame_views', 'eligible_frame_fold_removed',
                                                   'same_queue_prefix_endpoint_outputs'}
    require(witness['route'] == route and set(witness.get('checks', {})) == keys
            and all(v is True for v in witness['checks'].values()), 'wrong_window_route')


def _record(record, frozen, expected):
    phase = expected['phase']
    require(record.get('case') == expected['case'] and record.get('phase') == phase
            and record.get('revision') == REVISIONS[phase] and record.get('mode') == 'new'
            and record.get('query_syntax') == 'yql'
            and optimizer_mode_matches_record(record, require_exclusive=True)
            and integer(record.get('rows'), frozen['rows']) and integer(record.get('repetition'), 0)
            and integer(record.get('node_count'), 1) and integer(record.get('exit_code'), 0)
            and integer(record.get('row_count'), frozen['rows'])
            and integer(record.get('result_set_count'), 1)
            and record.get('result_set_boundaries_known') is True
            and integer(record.get('setup_request_count'), 1)
            and integer(record.get('target_request_count'), 1)
            and integer(record.get('validation_request_count'), 0)
            and record.get('target_results_checked') is True
            and record.get('expected_rejection_configured') is False
            and hexdigest(record.get('result_sha256'))
            and record.get('generator_module') == 'quality_q01_scaling_checks'
            and record.get('generator_version') == scaling.GENERATOR_VERSION
            and record.get('generator_hash_source') == 'loaded_source_snapshot'
            and record.get('quality_profile_sha256') == frozen['profile_sha256']
            and record.get('verification_contract_version') == 'strict_results_and_request_boundaries_v1'
            and record_verdict(record) == 'functional', 'record_identity_or_semantics')
    profile = next(p for p in frozen['profiles'] if p['case'] == expected['case'])
    require(all(record.get(k) == v for k, v in profile['workload'].items()), 'workload_identity_changed')
    _guard_and_receipt(record, REVISIONS[phase])
    _path(record, profile['cell'], phase)
    identity = record.get('q01_measurement_identity', {})
    require(identity.get('run_id') == frozen['run_id'] and identity.get('case') == expected['case']
            and identity.get('phase') == phase and identity.get('revision') == REVISIONS[phase]
            and identity.get('kind') == expected['kind'] and integer(identity.get('index'), expected['index'])
            and integer(identity.get('rows'), frozen['rows']) and identity.get('mode') == 'new'
            and identity.get('cell') == profile['cell'] and identity.get('workload') == profile['workload']
            and identity.get('native_driver_sha256') == digest(Path(native.__file__).read_bytes())
            and identity.get('dependency_source_sha256') == scaling.source_bindings()
            and identity.get('executable_sha256') == record['binary_provenance']['sha256']
            and identity.get('build_receipt_sha256') == record['build_receipt_sha256']
            and identity.get('resource_design_sha256') == frozen['superseded_proposal_sha256'],
            'measurement_coordinates_changed')
    observation = record.get('q01_child_observation', {})
    require(observation.get('schema_version') == 'q01_inherited_scope_child_observation_v1'
            and observation.get('status') == 'raw_observation'
            and observation.get('outer_memory_guard_verified') is True
            and observation.get('outer_memory_guard_admission_pending') is False
            and observation.get('outer_memory_guard_sha256') == record['native_batch_memory_sha256']
            and observation.get('target_group_empty') is True and observation.get('local_cleanup_complete') is True
            and observation.get('metric_scope') == 'whole_kqprun_lifetime_including_startup_schema_fixtures_and_target'
            and observation.get('python_generation_and_result_oracle_included') is False,
            'raw_measurement_scope_or_cleanup')
    bindings = observation['bindings']
    require(bindings.get('identity_sha256') == digest(encoded(identity))
            and integer(bindings.get('timeout_seconds'), 120), 'measurement_identity_unbound')
    require(all(hexdigest(bindings.get(k)) for k in ('inherited_environment_sha256',
                'python_executable_sha256', 'udf_library_sha256', 'inherited_scope_sha256', 'command_sha256'))
            and all(isinstance(bindings.get(k), str) and bindings[k] for k in ('linux_release', 'machine'))
            and isinstance(bindings.get('cpu_affinity'), list) and bool(bindings['cpu_affinity'])
            and all(integer(v) for v in bindings['cpu_affinity'])
            and sorted(set(bindings['cpu_affinity'])) == bindings['cpu_affinity'], 'runtime_environment_binding')
    for key, name in [('protocol_source_sha256', 'anonymous_child_measurement.py'),
                      ('integration_source_sha256', 'q01_native_child_measurement.py'),
                      ('outer_guard_source_sha256', 'anonymous_cpp_guard.py')]:
        require(bindings.get(key) == digest((ROOT / name).read_bytes()), 'measurement_source_changed')
    raw = observation['raw_child_report']
    require(set(raw) == child_protocol.REPORT_KEYS
            and isinstance(raw.get('nonce'), str) and len(raw['nonce']) == 32
            and all(c in '0123456789abcdef' for c in raw['nonce'])
            and raw.get('schema_version') == 'linux_wait4_one_child_v1'
            and digest(encoded(raw)) == observation.get('report_sha256')
            and integer(raw.get('exit_code'), 0) and integer(raw.get('wait_status'), 0)
            and raw.get('termination_signal') is None and raw.get('core_dump_status_bit') is False
            and integer(raw.get('guard_exit_code'), 0) and integer(raw.get('waited_pid'))
            and raw['waited_pid'] > 0, 'child_terminal_record')
    require(all(type(raw.get(k)) in (int, float) and math.isfinite(raw[k]) and raw[k] >= 0
                for k in ('user_cpu_seconds', 'system_cpu_seconds'))
            and math.isfinite(raw['user_cpu_seconds'] + raw['system_cpu_seconds'])
            and integer(raw.get('linux_ru_maxrss_kib'))
            and integer(observation.get('linux_ru_maxrss_bytes'))
            and observation.get('linux_ru_maxrss_bytes') == raw['linux_ru_maxrss_kib'] * 1024,
            'raw_metric_invalid')
    return {'user_cpu_seconds': raw['user_cpu_seconds'], 'system_cpu_seconds': raw['system_cpu_seconds'],
            'max_rss_bytes': observation['linux_ru_maxrss_bytes'],
            'environment_identity': {k: bindings[k] for k in ('inherited_environment_sha256',
                'python_executable_sha256', 'udf_library_sha256', 'linux_release', 'machine', 'cpu_affinity')}}


def validate_stage_A(records, frozen, *, kind='pilot', runtime_evidence=None):
    result = {'schema_version': VERSION, 'semantic_path_pass': False, 'allocation_snapshots_pass': False,
              'raw_measurement_vector_pass': False, 'resource_scaling_verified': False,
              'optimization_acceptance_pass': False, 'canonical_SQL_652_admitted': False,
              'pilot_native_identity_and_protocol_pass': False,
              'allocation_scope': 'fixed42_linked_RANGE_runtime_and_direct_outer_collector_only',
              'path_scope': 'local_stream_queue_endpoint_and_stack_fold_topology_not_general_equivalence',
              'reason': 'stage_A_requires_actual_path_before_measured_contract_binding'}
    try:
        require(frozen == manifest(frozen['rows'], frozen['run_id']), 'manifest_or_sources_changed')
        require(kind in ('pilot', 'measurement'), 'unknown_stage')
        expected = ([{'case': p['case'], 'phase': phase, 'kind': 'pilot', 'index': 0} for p in frozen['profiles']
                     for phase in ('baseline', 'fixed')] if kind == 'pilot' else frozen['schedule'])
        require(isinstance(records, list) and len(records) == len(expected), 'incomplete_finite_vector')
        metrics = [_record(record, frozen, cell)
                   for record, cell in zip(records, expected)]
        require(len({str(Path(r['native_batch_memory_evidence']).resolve()) for r in records}) == len(records),
                'single_cell_guard_slices_required')
        require(len({r['q01_child_observation']['bindings']['inherited_scope_sha256'] for r in records}) == len(records)
                and len({r['q01_child_observation']['raw_child_report']['nonce'] for r in records}) == len(records),
                'distinct_scope_and_child_report_required')
        require(all(m['environment_identity'] == metrics[0]['environment_identity'] for m in metrics),
                'incompatible_measurement_environments')
        result['semantic_path_pass'] = True
        result['pilot_native_identity_and_protocol_pass'] = kind == 'pilot'
        if runtime_evidence is not None:
            result['allocation_snapshots_pass'] = validate_fixed_runtime(runtime_evidence, REVISIONS['fixed'])
        if kind == 'measurement':
            result.update(raw_measurement_vector_pass=True, raw_metrics=metrics)
            comparisons = []
            for profile in frozen['profiles']:
                groups = {phase: [metric for cell,metric in zip(expected,metrics)
                    if cell['case']==profile['case'] and cell['phase']==phase and cell['kind']=='measurement']
                    for phase in ('baseline','fixed')}
                require(all(len(values)==5 for values in groups.values()), 'incomplete_comparison_group')
                cpu = {phase: [m['user_cpu_seconds']+m['system_cpu_seconds'] for m in values]
                       for phase,values in groups.items()}
                comparisons.append({'case':profile['case'],
                    'measured_samples_per_phase':5,
                    'total_child_CPU_seconds':cpu,
                    'whole_child_peak_RSS_bytes':{phase:[m['max_rss_bytes'] for m in values]
                                                   for phase,values in groups.items()},
                    'fixed_all_total_CPU_below_baseline':max(cpu['fixed'])<min(cpu['baseline']),
                    'baseline_all_total_CPU_below_fixed':max(cpu['baseline'])<min(cpu['fixed']),
                    'observation_only_no_accepting_threshold':True})
            result['finite_raw_comparisons'] = comparisons
        result['manifest_sha256'] = digest(encoded(frozen))
        result['measured_contract_dispatch_binding_present'] = False
        result['required_next_stage'] = 'Freeze actual path-qualified measured contract; bind its digest at native dispatch and separately require complete canonical652 evidence before any optimization admission.'
    except (OSError, ValueError, TypeError, KeyError, AttributeError, StopIteration, OverflowError):
        result['reason'] = 'incomplete_or_inconsistent_stage_A_evidence'
    return result
