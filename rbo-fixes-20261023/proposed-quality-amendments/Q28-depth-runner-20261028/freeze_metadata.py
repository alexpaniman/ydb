"""Freeze only the reviewed-source runner install patch and sanitized metadata."""
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import ModuleType

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TARGET = ROOT.name + '/q28_depth_verify.py'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    path = HERE / 'q28_depth_verify.py'
    raw = path.read_bytes()
    module = ModuleType('q28_depth_verify')
    module.__file__ = str(path)
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    source_report_path = ROOT / 'verification/Q28-map-fusion-refresh-depth-source-preparation-20261028.json'
    source_report = json.loads(source_report_path.read_bytes())
    mock_path = ROOT / 'verification/Q28-depth-fixed-runner-metadata-checks-20261028.json'
    mocks = json.loads(mock_path.read_bytes())
    check(mocks['source_sha256'] == sha(raw), 'exact_mocked_runner_after_image')
    check(mocks['checks_passed'] == 255 and mocks['scenario_count'] == 52, 'complete_current_mock_vector')
    check(len({x['scenario'] for x in mocks['scenarios']}) == 52, 'no_duplicate_mock_scenarios')
    check(module.SOURCE_BINDINGS[module.GENERATOR] == source_report['generator_patch']['after_sha256'],
          'independently_reviewed_four_control_generator')
    check(module.RENDERED_SOURCE_SHA256 == source_report['controls']['generated_source_sha256'],
          'same_generated_contract_hash_without_persistence')
    check(module.SOURCE_BINDINGS['quality_scheduler_cpp_checks'] == source_report['existing26']['source_sha256'],
          'original26_source_binding_unchanged')
    for name, expected in module.SOURCE_BINDINGS.items():
        if name == module.GENERATOR:
            current = ROOT / 'proposed-quality-amendments/Q28-map-fusion-refresh-depth-20261028/quality_scheduler_depth_checks.py'
        else:
            current = ROOT / (name + '.py')
        check(sha(current.read_bytes()) == expected, 'frozen_helper_image:' + name)
    for name, call_names in {
        'anonymous_cpp_verify': ['serial_slot', '_expected_revision', '_memfd', '_fd_digest',
                                '_stdout_link_arguments', '_archive_response_arguments',
                                '_seal_native_executable', 'pair_direct'],
        'anonymous_cpp_metadata': ['verify_receipt', 'source_revision'],
        'production_version_object': ['prepare_object', 'verify_object'],
        'anonymous_cpp_guard': ['run_anonymous'],
    }.items():
        functions = {x.name: x for x in ast.parse((ROOT / (name + '.py')).read_bytes()).body if isinstance(x, ast.FunctionDef)}
        for called in call_names:
            check(called in functions, 'actual_helper_API:' + name + '.' + called)
    helper = (ROOT / 'anonymous_cpp_verify.py').read_text()
    check("'Q28': 'quality_scheduler_cpp_checks'" in helper, 'normal_direct26_registration_unchanged')
    check("'Q28'" in helper[helper.index('PAIRED_DIRECT_BUGS'):helper.index('def _current_pair_contract')],
          'normal_direct26_pair_admission_unchanged')
    # Existing fresh closure receipts use exactly the fields required by this route.
    actual_receipt = ROOT / 'verification/cpp-closures/54db45bef39b-1793147648306259464/receipt.json'
    receipt = json.loads(actual_receipt.read_bytes())
    check(receipt['build']['build_record_version'] == 'production_cpp_build_v2'
          and receipt['metadata']['profile'] == 'rbo' and receipt['metadata']['target'] == 'ydb/core/kqp/ut/rbo'
          and receipt['build']['profile'] == 'rbo' and receipt['build']['target'] == 'ydb/core/kqp/ut/rbo'
          and isinstance(receipt['build_record'], dict) and isinstance(receipt['graph_export_record'], dict),
          'fresh_existing_receipt_schema_compatible_no_receipt_reused')
    patch_path = ROOT / 'proposed-quality-amendments/Q28-depth-fixed-runner-20261028.patch'
    patch_path.write_text('diff --git a/' + TARGET + ' b/' + TARGET + '\nnew file mode 100644\n'
        + ''.join(difflib.unified_diff([], raw.decode().splitlines(True), fromfile='/dev/null', tofile='b/' + TARGET)))
    controls_patch = ROOT / source_report['generator_patch']['path']
    with tempfile.TemporaryDirectory(prefix='q28-depth-install-') as td:
        for patch_path_item in (controls_patch, patch_path):
            subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(patch_path_item)], cwd=td,
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run(['git', 'apply', str(patch_path_item)], cwd=td, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check((Path(td) / TARGET).read_bytes() == raw, 'exact_disposable_runner_install')
        installed_generator = Path(td) / ROOT.name / (module.GENERATOR + '.py')
        check(sha(installed_generator.read_bytes()) == module.SOURCE_BINDINGS[module.GENERATOR],
              'exact_disposable_generator_then_runner_composition')
    report = {
        'schema': 'q28_depth_fixed_runner_source_preparation_v1',
        'status': 'dormant_source_and_mock_checked_independent_review_and_native_pending',
        'patch': str(patch_path.relative_to(ROOT)), 'patch_sha256': sha(patch_path.read_bytes()),
        'target': TARGET, 'after_sha256': sha(raw),
        'ordered_installation': [
            {'patch': source_report['generator_patch']['path'], 'sha256': source_report['generator_patch']['sha256']},
            {'patch': str(patch_path.relative_to(ROOT)), 'sha256': sha(patch_path.read_bytes())}],
        'production_amendment_patch_sha256': source_report['production_patch']['sha256'],
        'new_revision': 'Supplied by root only after the independently main-based one-line amendment; no placeholder is admitted.',
        'revision_checks': ['Exact external expected revision equals cases/Q28.json and clean worktree HEAD.',
            'Existing _expected_revision validates the pinned-main parent.',
            'Relative to old Q28, the complete tree changes only the reviewed header.',
            'Exact after-header hash and seven other reviewed production-source hashes are mandatory before and after execution.'],
        'source_bindings': dict(module.SOURCE_BINDINGS), 'production_source_bindings': dict(module.PRODUCTION_BINDINGS),
        'source_scope_report': str(source_report_path.relative_to(ROOT)), 'source_scope_report_sha256': sha(source_report_path.read_bytes()),
        'assertion_vector': list(module.EXPECTED_CHECKS), 'assertion_total': sum(module.EXPECTED_CHECKS),
        'assertion_count_scope': 'Derived from frozen generator source, checked exactly for passing records; actual native execution remains pending.',
        'case_vector': list(module.SPECS),
        'execution': {
            'fixed_only': True, 'generic_generator_or_baseline_override': False,
            'new_build_export_or_graph_commands': False,
            'production_closure_profile': 'rbo', 'production_closure_target': 'ydb/core/kqp/ut/rbo',
            'fresh_v2_build_and_graph_receipt_required': True,
            'compile_timeout_seconds': 900, 'link_timeout_seconds': 900, 'native_timeout_seconds': 120,
            'link_scope_gib': 36, 'native_scope_gib': 12,
            'outer_serial_lock': 'Unchanged anonymous_cpp_verify.serial_slot; requires frozen sources and direct_cpp_verification state.',
            'generated_Cpp_object_executable_transport': 'Anonymous memfds; production version generation/replay, archive-order transport and same-inode sealed read-only executable handoff reuse exact reviewed helpers.',
            'file_or_raw_diagnostic_persistence': 'Only sanitized metadata evidence written under a new exclusive filename. No raw generated input, output, AST, rows or diagnostics.',
            'cleanup': 'Each descriptor becomes caller-owned immediately after allocation inside try/finally. Partial allocation, version preparation and seal/native cancellation all retain cleanup ownership.',
            'failure_evidence': 'Completed parsed case verdicts and sanitized process/hash evidence survive final validation failure/cancellation. Unsafe malformed output is represented only by guarded process hashes; abrupt process death can prevent the final JSON and is not claimed recoverable.'},
        'admission': {
            'existing26_route_modified': False,
            'existing26_paired_evidence_required_before_four': True,
            'existing26_validator': 'Existing pair_direct(Q28) with exact immutable baseline/fixed files, source/vector/closure/guard checks; revalidated after native execution.',
            'four_control_comparator': 'Actual amended production rule versus same-build old-depth-two declaration, not an alternate production baseline.',
            'amendment_local_contracts_verified': 'True only after all four exact records/counter deltas/assertion counts and final revision/receipt/source/object/executable/original26 rechecks pass.',
            'canonical_SQL8_required_separately': True, 'SDK7_remains_optional_partial_measurement_pilot': True,
            'query_semantics_verified': False, 'quality_improvement_verified': False, 'full_q28_admission': False,
            'pure_optimizer_or_whole_query_timing_claim': False},
        'binding_refresh_note': 'This runner pins seven exact helper/generator files. It does not import the quality SQL spill adapter on this path. Any future change to a pinned helper requires a separately reviewed binding-only refresh; the separate Q28 revision/SQL/SDK binding plan remains required.',
        'metadata_checks': {'source_checks_passed': len(checks), 'source_checks': checks,
            'mock_checks_passed': mocks['checks_passed'], 'mock_scenarios': mocks['scenario_count'],
            'mock_report': str(mock_path.relative_to(ROOT)), 'mock_report_sha256': sha(mock_path.read_bytes()),
            'mock_script_sha256': sha((HERE / 'verify_metadata.py').read_bytes()),
            'existing_receipt_schema_example_sha256': sha(actual_receipt.read_bytes())},
        'production_live_helpers_or_Git_objects_modified': False,
        'native_build_toolchain_runs': 0, 'generated_CPP_queries_results_or_repros_persisted': False,
    }
    output = ROOT / 'verification/Q28-depth-fixed-runner-source-preparation-20261028.json'
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'patch_sha256': sha(patch_path.read_bytes()), 'after_sha256': sha(raw),
                      'report_sha256': sha(output.read_bytes()), 'source_checks_passed': len(checks)}))


if __name__ == '__main__':
    main()
