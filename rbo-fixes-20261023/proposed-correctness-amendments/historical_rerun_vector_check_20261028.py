"""Read-only exact-vector audit of fresh guarded historical rerun slices.

No commands, builds, native tools, or workload rendering are performed here.
Passing this checker does not remove a recorded finding's remaining scope limits.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from verification_contract import record_verdict, summarize_verification
from optimizer_mode_contract import optimizer_mode_matches_record

MANIFEST_SHA256 = 'c0540b03972fb3a187bfff74e7932e44fccf64aea625896337638525ac8ffe58'
MANIFEST_PATH = ROOT / 'verification/historical-rerun-exact-vector-20261028.json'
HEX = re.compile(r'[0-9a-f]{64}')
BINARY_FIELDS = {'sha256', 'size_bytes', 'ctime_ns', 'mtime_ns', 'inode', 'device', 'resolved_path'}
GIB = 1024 ** 3

class VectorError(Exception):
    pass

def require(value, category):
    if not value:
        raise VectorError(category)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate_json_key')
        result[key] = value
    return result

def document(data):
    return json.loads(data, object_pairs_hook=unique)

def exact(value, expected):
    return type(value) is type(expected) and value == expected

def artifact_path(value, directory='verification'):
    require(isinstance(value, str), 'artifact_path_missing')
    path = Path(value).resolve()
    require(path.is_relative_to((ROOT / directory).resolve()), 'artifact_outside_verification_directory')
    return path

def source_bindings(manifest, read_bytes):
    for binding in manifest['source_bindings']:
        require(sha(read_bytes(Path(binding['path']))) == binding['sha256'], 'frozen_source_binding_changed')

def audit(manifest, bug, slice_path, raw_slice, read_bytes, *, fullsuite_build_path=None):
    require(manifest.get('schema_version') == 'historical_exact_vectors_v1', 'manifest_version')
    selected = [item for item in manifest['items'] if item['bug'] == bug]
    require(len(selected) == 1, 'bug_not_in_exact_manifest')
    item = selected[0]
    source_bindings(manifest, read_bytes)
    revision = item['revision']
    path = artifact_path(str(slice_path))
    matched = re.fullmatch(r'guarded-fixed-' + re.escape(bug) + '-' + revision[:12] + r'-([0-9]+)\.jsonl', path.name)
    require(matched is not None, 'fresh_guarded_slice_filename_required')
    stamp = matched.group(1)
    require(type(manifest.get('prepared_unix_ns')) is int and int(stamp) >= manifest['prepared_unix_ns'], 'slice_predates_frozen_manifest')
    records = [document(line) for line in raw_slice.splitlines() if line.strip()]
    require(len(records) == item['expected_record_count'], 'exact_record_count')
    expected = {}
    for cell in item['cells']:
        reps = cell['repetitions'] if cell['repetitions'] is not None else [None]
        for repetition in reps:
            key = (cell['case'], cell['mode'], repetition)
            require(key not in expected, 'manifest_duplicate_cell')
            expected[key] = cell
    observed = set()
    receipt_binding = memory_binding = None
    receipt_raw = receipt = memory = None
    qualified = 0
    for record in records:
        require(isinstance(record, dict), 'record_not_object')
        require(exact(record.get('rows'), item['rows']) and record.get('revision') == revision
                and record.get('bug') == bug and record.get('phase') == 'fixed', 'record_revision_phase_size')
        repetition = record.get('repetition')
        require(repetition is None or type(repetition) is int, 'repetition_type')
        key = (record.get('case'), record.get('mode'), repetition)
        require(key in expected and key not in observed, 'missing_duplicate_or_extra_cell')
        observed.add(key)
        cell = expected[key]
        require(type(record.get('exit_code')) is int, 'exit_code_type')
        require(record.get('native_batch_guard_pass') is True, 'native_guard_pass_required')
        require(record.get('binary_provenance_required') is True and record.get('binary_provenance_verified') is True,
                'exact_binary_provenance_required')
        require(record.get('binary_provenance_version') == 'successful_runner_build_v1'
                and record.get('binary_provenance_comparison_version') == 'receipt_ctime_only_revalidation_v1', 'binary_provenance_contract')
        receipt_path = artifact_path(record.get('build_receipt_path'), 'verification/build-receipts')
        receipt_hash = record.get('build_receipt_sha256')
        guard_path = artifact_path(record.get('native_batch_memory_evidence'))
        guard_hash = record.get('native_batch_memory_sha256')
        require(isinstance(receipt_hash, str) and HEX.fullmatch(receipt_hash)
                and isinstance(guard_hash, str) and HEX.fullmatch(guard_hash), 'artifact_hash_format')
        require(guard_path.name == 'native-memory-fixed-' + bug + '-' + revision[:12] + '-' + stamp + '.json', 'guard_slice_stamp_binding')
        if receipt_binding is None:
            receipt_binding, memory_binding = (str(receipt_path), receipt_hash), (str(guard_path), guard_hash)
            receipt_raw, guard_raw = read_bytes(receipt_path), read_bytes(guard_path)
            require(sha(receipt_raw) == receipt_hash and sha(guard_raw) == guard_hash, 'receipt_or_guard_digest')
            receipt, memory = document(receipt_raw), document(guard_raw)
            require(receipt.get('worktree') == manifest['expected_worktree'], 'receipt_worktree_identity')
            require(receipt.get('schema_version') == 'successful_runner_build_v1'
                    and receipt.get('revision') == revision, 'receipt_revision')
            require(type(receipt.get('recorded_unix_ns')) is int
                    and manifest['prepared_unix_ns'] <= receipt['recorded_unix_ns'] <= int(stamp), 'fresh_receipt_before_slice_required')
            build = receipt.get('build', {})
            require(exact(build.get('exit_code'), 0) and 'ydb/tests/tools/kqprun' in build.get('target', '').split(), 'successful_runner_build_required')
            require(exact(memory.get('exit_code'), 0) and memory.get('cleanup_complete') is True
                    and memory.get('watchdog_stopped') is False and memory.get('stdout_overflow') is False, 'fresh_guard_completion')
            require(exact(memory.get('max_scope_gib'), 24) and exact(memory.get('min_host_available_gib'), 36), 'native_memory_limits')
            require(type(memory.get('minimum_host_available_bytes')) is int
                    and memory['minimum_host_available_bytes'] >= 36 * GIB, 'native_memory_headroom')
        else:
            require(receipt_binding == (str(receipt_path), receipt_hash)
                    and memory_binding == (str(guard_path), guard_hash), 'mixed_receipt_or_guard_slice')
        actual, built = record.get('binary_provenance'), receipt.get('binary')
        require(isinstance(actual, dict) and isinstance(built, dict) and set(actual) == set(built) == BINARY_FIELDS, 'binary_identity_fields')
        require(isinstance(actual.get('sha256'), str) and HEX.fullmatch(actual['sha256'])
                and type(actual.get('size_bytes')) is int and actual['size_bytes'] > 0, 'binary_digest_and_size')
        for identity in (actual, built):
            require(all(type(identity[name]) is int and identity[name] >= 0 for name in
                        ('size_bytes', 'ctime_ns', 'mtime_ns', 'inode', 'device'))
                    and identity['size_bytes'] > 0 and identity['inode'] > 0
                    and isinstance(identity['resolved_path'], str) and Path(identity['resolved_path']).is_absolute(),
                    'canonical_binary_identity_types')
        require(isinstance(record.get('result_sha256'), str) and HEX.fullmatch(record['result_sha256'])
                and type(record.get('row_count')) is int and record['row_count'] >= 0, 'result_digest_and_row_count_required')
        require(type(actual.get('ctime_ns')) is int and actual['ctime_ns'] >= 0
                and type(built.get('ctime_ns')) is int and built['ctime_ns'] >= 0
                and all(exact(value, built[key]) for key, value in actual.items() if key != 'ctime_ns'), 'receipt_binary_identity_mismatch')
        require(exact(record.get('build_receipt_ctime_changed'), actual['ctime_ns'] != built['ctime_ns']), 'ctime_revalidation_flag')
        if cell['record_kind'] == 'generator_snapshot':
            require(type(repetition) is int, 'generated_repetition_required')
            require(record.get('generator_hash_source') == 'loaded_source_snapshot', 'loaded_generator_snapshot_required')
            for name in ('generator_module', 'generator_sha256', 'generator_version', 'node_count', 'query_syntax'):
                require(exact(record.get(name), cell[name]), 'generator_or_execution_contract_mismatch')
            for name, expected_value in cell['bindings'].items():
                require(exact(record.get(name), expected_value), 'regenerated_input_hash_mismatch')
            for name, expected_value in cell['required_record_fields'].items():
                require(exact(record.get(name), expected_value), 'required_observation_contract_missing')
            for name, expected_value in cell['required_oracle_fields'].items():
                require(exact(record.get(name), expected_value), 'case_specific_oracle_contract_missing')
            require(optimizer_mode_matches_record(record, require_exclusive=not cell['fallback_requested']), 'optimizer_mode_contract')
            config = record['optimizer_mode_configuration']
            require(exact(config.get('fallback_requested'), cell['fallback_requested']), 'fallback_scope_changed')
            if bug == 'C02' and cell['case'].startswith('c02_volatile_'):
                require(type(record.get('nonvacuous_group_observed')) is bool, 'volatile_nonvacuity_field_required')
        else:
            qualified += 1
            require('repetition' not in record and not any(name in record for name in
                ('generator_module', 'generator_sha256', 'generator_hash_source', 'optimizer_mode_config_verified',
                 'optimizer_mode_configuration', 'settings_sha256', 'query_sha256', 'schema_sha256', 'fixture_sha256')),
                'legacy_adapter_contract_changed_requires_review')
        outcome = cell['expected_outcome']
        require(record_verdict(record) == ('guard' if outcome == 'guard' else 'functional'), 'existing_oracle_verdict_failed')
        if outcome in ('guard', 'target_failure'):
            require(record['exit_code'] > 0 and record.get('failure_stage') == 'target', 'target_rejection_boundary')
            if outcome == 'target_failure':
                require(record.get('expected_failure_observed') is True, 'expected_target_failure_required')
        else:
            require(record['exit_code'] == 0 and record.get('semantic_oracle_pass') is True, 'functional_semantics_required')
    require(observed == set(expected), 'complete_cell_vector_required')
    passed, status = summarize_verification(records, bug)
    require(passed is True and status == item['expected_status'], 'qualified_summary_contract')
    fullsuite = False
    fullsuite_binding = None
    require(item['fullsuite_build_required'] or fullsuite_build_path is None, 'unexpected_fullsuite_build_record')
    if item['fullsuite_build_required']:
        require(fullsuite_build_path is not None, 'C41_fullsuite_build_record_required')
        build_path = artifact_path(str(fullsuite_build_path))
        build_raw = read_bytes(build_path)
        fullsuite_binding = {'path': str(build_path), 'sha256': sha(build_raw)}
        build = document(build_raw)
        require(build.get('bug') == bug and build.get('revision') == revision and exact(build.get('exit_code'), 0), 'C41_successful_fullsuite_revision')
        require(build.get('target', '').split() == ['ydb/tests/tools/kqprun', 'ydb/core/kqp/ut/rbo', '-tA'], 'C41_complete_suite_target_required')
        require(str(artifact_path(build.get('build_receipt'), 'verification/build-receipts')) == receipt_binding[0], 'C41_suite_receipt_path')
        require(all(exact(build.get(name), receipt['build'].get(name)) for name in
                    ('exit_code', 'target', 'elapsed_seconds', 'diagnostic_sha256')), 'C41_fullsuite_receipt_binding')
        budget = build.get('memory', {})
        require(budget.get('watchdog_stopped') is False and exact(budget.get('explicit_build_workers'), 8)
                and exact(budget.get('watchdog_min_host_available_gib'), 36)
                and exact(budget.get('watchdog_max_scope_usage_gib'), 52), 'C41_build_memory_guard')
        fullsuite = True
    source_bindings(manifest, read_bytes)
    if fullsuite_binding is not None:
        require(sha(read_bytes(Path(fullsuite_binding['path']))) == fullsuite_binding['sha256'], 'fullsuite_build_changed_during_check')
    require(sha(read_bytes(Path(receipt_binding[0]))) == receipt_binding[1]
            and sha(read_bytes(Path(memory_binding[0]))) == memory_binding[1]
            and read_bytes(path) == raw_slice, 'artifact_changed_during_vector_check')
    return {'bug': bug, 'revision': revision, 'exact_vector_passed': True,
            'checked_records': len(records), 'slice_sha256': sha(raw_slice),
            'guard_sha256': memory_binding[1], 'build_receipt_sha256': receipt_binding[1],
            'qualified_legacy_adapter_records': qualified, 'qualified_status': status,
            'C41_fullsuite_build_bound': fullsuite, 'fullsuite_build_evidence': fullsuite_binding,
            'C03_expanded_control_vector_present': bug == 'C03',
            'remaining_qualifications': item['remaining_qualifications'],
            'automatic_full_finding_promotion': False}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bug', required=True)
    parser.add_argument('--records', type=Path, required=True)
    parser.add_argument('--fullsuite-build-record', type=Path)
    args = parser.parse_args()
    try:
        manifest_raw = MANIFEST_PATH.read_bytes()
        require(sha(manifest_raw) == MANIFEST_SHA256, 'exact_manifest_hash')
        result = audit(document(manifest_raw), args.bug, args.records, args.records.read_bytes(),
                       lambda path: path.read_bytes(), fullsuite_build_path=args.fullsuite_build_record)
    except (VectorError, OSError, ValueError, TypeError, KeyError, AttributeError) as exc:
        print(json.dumps({'exact_vector_passed': False, 'reason': str(exc) if isinstance(exc, VectorError) else type(exc).__name__,
                          'automatic_full_finding_promotion': False}))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
