"""Read-only C41 acceptance wrapper; never runs a build, query or Git command.

The original historical checker and its thirteen completed findings remain
immutable. This wrapper selects the unchanged C41 vector at the amended revision
and refreshes only its installed-source bindings and required suite inventory.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / 'verification/C41-standard-pipeline-exact-vector-20261028.json'
MANIFEST_SHA256 = '2986bdca21258973c08a5a30fcc7a62ceded15f5117f8ac4e674ba99a626b8e9'
LEGACY_PATH = Path(__file__).with_name('historical_rerun_vector_check_20261028.py')
LEGACY_SHA256 = 'c532b97282b370c4398b7a0db158844614a8e2d564ff86bbdd7dd3ecf48c282f'
REVISION = 'c6a5904e300b925032913f51e6a19518ceb842c2'


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def load_checker():
    raw = LEGACY_PATH.read_bytes()
    if digest(raw) != LEGACY_SHA256:
        raise ValueError('historical_checker_changed')
    spec = importlib.util.spec_from_file_location('c41_unchanged_historical_checker', LEGACY_PATH)
    module = importlib.util.module_from_spec(spec)
    exec(compile(raw, str(LEGACY_PATH), 'exec'), module.__dict__)
    return module


def audit(slice_path, fullsuite_build_path, read_bytes=lambda path: path.read_bytes()):
    checker = load_checker()
    require, exact = checker.require, checker.exact
    manifest_raw = read_bytes(MANIFEST_PATH)
    require(digest(manifest_raw) == MANIFEST_SHA256, 'exact_C41_manifest_required')
    manifest = checker.document(manifest_raw)
    require(len(manifest['items']) == 1 and manifest['items'][0]['bug'] == 'C41'
            and manifest['items'][0]['revision'] == REVISION, 'amended_C41_only')

    def suite_sources():
        worktree = Path(manifest['expected_worktree'])
        for relative, expected in manifest['suite_source_bindings'].items():
            require(digest(read_bytes(worktree / relative)) == expected, 'unchanged_full_RBO_suite_required')
        boundary = manifest['required_production_boundary']
        require(digest(read_bytes(worktree / boundary['path'])) == boundary['sha256'],
                'amended_final_query_boundary_required')

    suite_sources()
    raw_slice = read_bytes(Path(slice_path))
    result = checker.audit(manifest, 'C41', slice_path, raw_slice, read_bytes,
                           fullsuite_build_path=fullsuite_build_path)
    records = [checker.document(line) for line in raw_slice.splitlines() if line.strip()]
    first = records[0]
    receipt_path = checker.artifact_path(first['build_receipt_path'], 'verification/build-receipts')
    receipt = checker.document(read_bytes(receipt_path))
    expected_binary = str((Path(manifest['expected_worktree']) / 'ydb/tests/tools/kqprun/kqprun').resolve())
    require(receipt['binary']['resolved_path'] == expected_binary, 'receipt_binary_worktree_binding')
    guard = checker.document(read_bytes(checker.artifact_path(first['native_batch_memory_evidence'])))
    for name in ('peak_scope_usage_bytes', 'peak_group_rss_bytes'):
        require(type(guard.get(name)) is int and 0 <= guard[name] <= 24 * checker.GIB, 'native_peak_budget')
    build_path = checker.artifact_path(str(fullsuite_build_path))
    build = checker.document(read_bytes(build_path))
    require(build['target'].split() == manifest['fullsuite_target'], 'unfiltered_full_suite_target')
    require(isinstance(build.get('diagnostic_sha256'), str)
            and checker.HEX.fullmatch(build['diagnostic_sha256']), 'fullsuite_diagnostic_digest')
    require(type(build.get('elapsed_seconds')) in (int, float)
            and math.isfinite(build['elapsed_seconds']) and build['elapsed_seconds'] >= 0,
            'fullsuite_elapsed_type')
    memory = build['memory']
    for name in ('peak_scope_usage_bytes', 'peak_group_rss_bytes'):
        require(type(memory.get(name)) is int and 0 <= memory[name] <= 52 * checker.GIB, 'fullsuite_peak_budget')
    require(type(memory.get('minimum_host_available_bytes')) is int
            and memory['minimum_host_available_bytes'] >= 36 * checker.GIB, 'fullsuite_host_headroom')
    require(memory.get('cleanup_complete') is not False and 'guard_exception_type' not in memory
            and 'stop_reason' not in memory, 'fullsuite_no_guard_interruption')
    suite_sources()
    checker.source_bindings(manifest, read_bytes)
    require(read_bytes(MANIFEST_PATH) == manifest_raw and read_bytes(Path(slice_path)) == raw_slice,
            'C41_manifest_or_slice_changed_during_check')
    require(digest(read_bytes(receipt_path)) == result['build_receipt_sha256']
            and digest(read_bytes(checker.artifact_path(first['native_batch_memory_evidence']))) == result['guard_sha256']
            and digest(read_bytes(build_path)) == result['fullsuite_build_evidence']['sha256'],
            'C41_evidence_changed_during_check')
    result.update(schema_version='C41_amended_exact_acceptance_v1',
                  manifest_sha256=MANIFEST_SHA256, historical_checker_sha256=LEGACY_SHA256,
                  unchanged_suite_source_files=len(manifest['suite_source_bindings']),
                  original12_semantic_vector_preserved=True,
                  historical13_evidence_reinterpreted=False,
                  automatic_full_finding_promotion=False)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--records', required=True, type=Path)
    parser.add_argument('--fullsuite-build-record', required=True, type=Path)
    args = parser.parse_args()
    try:
        result = audit(args.records, args.fullsuite_build_record)
    except Exception as error:
        # Diagnostic categories only; no payload or partially accepted record.
        category = str(error) if type(error).__name__ == 'VectorError' else type(error).__name__
        print(json.dumps(dict(exact_vector_passed=False, reason=category,
                             automatic_full_finding_promotion=False)))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
