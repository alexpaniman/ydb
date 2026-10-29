"""Dormant Q28 integration checks; all process and descriptor work is mocked."""
import ast
from contextlib import ExitStack
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PROPOSED = Path(__file__).resolve().parent
BEFORE = PROPOSED / 'q28_compilation_measurement_runtime_identity_20261028.candidate.py'
CANDIDATE = PROPOSED / 'q28_compilation_measurement_version_replay_20261028.candidate.py'
VERSION_HELPER = ROOT / 'proposed-correctness-amendments/production_version_object_20261028.candidate.py'
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]


def main():
    digest = lambda value: hashlib.sha256(value).hexdigest()
    before, after, helper = BEFORE.read_bytes(), CANDIDATE.read_bytes(), VERSION_HELPER.read_bytes()
    assert digest(before) == '4e29a04b473601f61ba63a156c9c1068f8790325f7937b8e7cba722289713de7'
    assert digest(helper) == '55eb36ef4d02f70c7ef232e3cbf2411ce04166068862f42dfeeaa7bea50678a9'
    def load(name, path, raw):
        module = types.ModuleType(name)
        module.__file__ = str(path)
        sys.modules[name] = module
        exec(compile(raw, str(path), 'exec'), module.__dict__)
        return module
    previous = load('q28_version_review_before', BEFORE, before)
    module = load('q28_compilation_measurement', CANDIDATE, after)
    version = load('production_version_object', VERSION_HELPER, helper)
    import anonymous_cpp_verify as direct
    import anonymous_cpp_metadata as metadata
    import quality_pairing
    import quality_scheduler_cpp_checks
    checks = 0
    def check(value):
        nonlocal checks
        if not value:
            raise AssertionError('q28_version_metadata_contract_' + str(checks + 1))
        checks += 1
    functions = lambda raw: {node.name: ast.dump(node) for node in ast.parse(raw).body
                             if isinstance(node, ast.FunctionDef) and node.name != 'execute_measurements'}
    check(functions(before) == functions(after))
    check(module.render_cpp() == previous.render_cpp())
    check(module.CASES == previous.CASES and module.EXECUTED_CASES == previous.EXECUTED_CASES)
    check(module.PENDING_CASE == previous.PENDING_CASE and module.PENDING_REASON == previous.PENDING_REASON)
    check(len(module.EXECUTED_CASES) == 7 and len(module.CASES) == 8)
    check(direct.GENERATORS['Q28'] == 'quality_scheduler_cpp_checks')
    check(len(quality_scheduler_cpp_checks.cases()) == 26)
    check(module._snapshot_inputs() == previous._snapshot_inputs())

    # The existing fingerprint walks local imports, including the new helper.
    # Substitute only its future installed bytes and the future Q28 module.
    virtual = {(ROOT / 'q28_compilation_measurement.py').resolve(): after,
               (ROOT / 'production_version_object.py').resolve(): helper}
    original_read, original_is_file = Path.read_bytes, Path.is_file
    def read(path):
        return virtual[path.resolve()] if path.resolve() in virtual else original_read(path)
    def is_file(path):
        return path.resolve() in virtual or original_is_file(path)
    with patch.object(Path, 'read_bytes', read), patch.object(Path, 'is_file', is_file):
        fingerprint = quality_pairing.profile_fingerprint(['q28_compilation_measurement', module.PROFILE])
        virtual[(ROOT / 'production_version_object.py').resolve()] = helper + b'\n# Metadata-only identity change.\n'
        check(quality_pairing.profile_fingerprint(['q28_compilation_measurement', module.PROFILE]) != fingerprint)

    receipt = {'revision': 'a' * 40,
        'metadata': {'profile': 'rbo', 'compile_argv': ['metadata-cc'], 'compile_cwd': '/metadata',
                     'compile_environment': {}, 'link_argv': ['metadata-ld', 'original-input'],
                     'link_cwd': '/metadata', 'link_environment': {}},
        'build': {'target': 'ydb/core/kqp/ut/rbo'}, 'files': []}
    raw_receipt = json.dumps(receipt).encode()
    args = types.SimpleNamespace(command='measure-compilation', bug='Q28', phase='baseline', sample_index=0,
        receipt=types.SimpleNamespace(read_bytes=lambda: raw_receipt, resolve=lambda: Path('/metadata/receipt')),
        worktree=Path('/metadata'), compile_timeout=7, link_timeout=11, native_timeout=13)
    runtime = {'environment': b'A=metadata\0', 'affinity': b'0\n'}
    valid = dict(completed=True, phase='complete', stats_present=True, compilation_present=True,
                 from_cache=False, server_compilation_duration_us=1000, server_compilation_cpu_us=100,
                 result_set_count=1, runtime_environment_verified=True, runtime_affinity_verified=True)
    summaries = []
    cases = ('normal', 'prepare_failure', 'link_failure', 'native_guard', 'cold_cache_rejected',
             'changed_post_link', 'changed_pre_native', 'changed_post_native', 'changed_final',
             'profile_changed_final', 'cancel_prepare', 'cancel_link', 'cancel_native')
    cases += tuple('allocate_' + str(index) for index in range(1, 18))
    with patch.object(subprocess, 'Popen', side_effect=AssertionError('unexpected_process_launch')):
        for scenario in cases:
            buffers, closed, events = {}, [], []
            next_fd, native_calls, verify_calls, profile_calls, allocation_calls = [100], [0], [0], [0], [0]
            restored_link = ['metadata-ld', '@FD:production_version_object@', 'original-input']
            infrastructure = {'schema_version': version.VERSION, 'sealed_inputs_and_object': True,
                              'production_version_object': {'sha256': digest(b'metadata-version-object'), 'size_bytes': 23}}
            def memfd(label, value=b'', **kwargs):
                allocation_calls[0] += 1
                if scenario == 'allocate_' + str(allocation_calls[0]):
                    raise OSError('metadata_descriptor_allocation_failed')
                next_fd[0] += 1
                buffers[next_fd[0]] = value
                return next_fd[0]
            def close(fd):
                check(fd in buffers and fd not in closed)
                closed.append(fd)
                del buffers[fd]
            def fd_digest(fd):
                return {'sha256': digest(buffers[fd]), 'size_bytes': len(buffers[fd])}
            def prepare(actual_receipt, path, worktree, fds, *, timeout):
                events.append('version_prepare')
                check(actual_receipt == receipt and path is args.receipt and worktree == args.worktree)
                check(timeout == args.compile_timeout)
                check(version.OBJECT not in fds)
                fds[version.OBJECT] = memfd('version', b'metadata-version-object')
                if scenario == 'cancel_prepare':
                    raise KeyboardInterrupt()
                if scenario == 'prepare_failure':
                    error = metadata.MetadataError('production_version_generate_failed')
                    error.production_version_evidence = {'generate': {'exit_code': 1}}
                    raise error
                return list(restored_link), infrastructure
            def verify(actual_evidence, fds):
                events.append('version_verify')
                verify_calls[0] += 1
                check(actual_evidence is infrastructure)
                check(buffers[fds[version.OBJECT]] == b'metadata-version-object')
                reject_at = {'changed_post_link': 1, 'changed_pre_native': 2,
                             'changed_post_native': 3, 'changed_final': 16}.get(scenario)
                if verify_calls[0] == reject_at:
                    raise metadata.MetadataError('production_version_object_or_inputs_changed')
            def stdout_link(argv):
                check(argv == restored_link)
                return list(argv), 'metadata'
            def archives(argv, fds, closure_files):
                check(argv == restored_link and version.OBJECT in fds)
                return list(argv), {'responses': []}
            def run(argv, cwd, fds, **kwargs):
                output = kwargs.get('stdout_fd')
                record = {'exit_code': 0, 'cleanup_complete': True, 'watchdog_stopped': False,
                          'stdout_overflow': False, 'environment_sha256': direct._json_digest({})}
                if output == 'object':
                    events.append('adapter_compile')
                    check(version.OBJECT not in fds)
                    buffers[fds[output]] = b'metadata-object'
                elif output == 'executable':
                    events.append('link')
                    check(argv == restored_link and version.OBJECT in fds)
                    check(kwargs['max_scope_gib'] == 36 and kwargs['timeout'] == 11)
                    if scenario == 'cancel_link':
                        raise KeyboardInterrupt()
                    buffers[fds[output]] = b'metadata-executable'
                    if scenario == 'link_failure':
                        record['exit_code'] = 1
                else:
                    events.append('native')
                    native_calls[0] += 1
                    check(version.OBJECT not in fds)
                    check(events[-2] == 'version_verify')
                    check(kwargs['max_scope_gib'] == 12 and kwargs['timeout'] == 13)
                    check(kwargs['compiler_environment'] is False and kwargs['retain_stdout'] == 0)
                    if scenario == 'cancel_native':
                        raise KeyboardInterrupt()
                    result = dict(valid)
                    if scenario == 'native_guard':
                        record.update(watchdog_stopped=True, cleanup_complete=False)
                    if scenario == 'cold_cache_rejected':
                        result.update(completed=False, phase='cold_sample', from_cache=True)
                        record['exit_code'] = 1
                    buffers[fds['report']] = json.dumps(result).encode()
                return record, b''
            def fingerprint_value(*unused):
                profile_calls[0] += 1
                return ('c' if scenario == 'profile_changed_final' and profile_calls[0] > 1 else 'b') * 64
            with ExitStack() as stack:
                stack.enter_context(patch.object(direct, '_expected_revision', return_value='a' * 40))
                stack.enter_context(patch.object(metadata, 'verify_receipt'))
                stack.enter_context(patch.object(quality_pairing, 'profile_fingerprint', side_effect=fingerprint_value))
                stack.enter_context(patch.object(module, '_runtime_snapshot', return_value=runtime))
                stack.enter_context(patch.object(version, 'prepare_object', side_effect=prepare))
                stack.enter_context(patch.object(version, 'verify_object', side_effect=verify))
                stack.enter_context(patch.object(direct, '_memfd', side_effect=memfd))
                stack.enter_context(patch.object(direct, '_fd_digest', side_effect=fd_digest))
                stack.enter_context(patch.object(direct, '_stdout_link_arguments', side_effect=stdout_link))
                stack.enter_context(patch.object(direct, '_archive_response_arguments', side_effect=archives))
                stack.enter_context(patch.object(direct, 'run_anonymous', side_effect=run))
                stack.enter_context(patch.object(os, 'fchmod'))
                stack.enter_context(patch.object(os, 'close', side_effect=close))
                stack.enter_context(patch.object(os, 'pread', side_effect=lambda fd, size, offset: buffers[fd][offset:offset+size]))
                try:
                    evidence, code = module.execute_measurements(args)
                except KeyboardInterrupt:
                    check(scenario in ('cancel_prepare', 'cancel_link', 'cancel_native'))
                except OSError:
                    check(scenario.startswith('allocate_'))
                    check(allocation_calls[0] == int(scenario.removeprefix('allocate_')))
                except metadata.MetadataError as error:
                    check(scenario not in ('normal', 'cold_cache_rejected'))
                    check('production_version_infrastructure' in error.stage_evidence)
                    if scenario == 'prepare_failure':
                        check(error.verification_stage == 'q28_compilation_pilot_production_version_infrastructure')
                        check(error.stage_evidence['production_version_infrastructure']['generate']['exit_code'] == 1)
                    elif scenario == 'native_guard':
                        check(str(error) == 'q28_measurement_native_guard_failed')
                        check(error.stage_evidence['last_native']['watchdog_stopped'] is True)
                        check(error.stage_evidence['last_native']['cleanup_complete'] is False)
                    elif scenario == 'link_failure':
                        check(error.verification_stage == 'q28_compilation_pilot_link')
                        check(error.stage_evidence['link']['exit_code'] == 1)
                else:
                    check(scenario in ('normal', 'cold_cache_rejected'))
                    check(evidence['production_version_infrastructure'] is infrastructure)
                    check(code == (0 if scenario == 'normal' else 2))
                    check(evidence['status'] == ('q28_compilation_pilot_partial_recorded' if scenario == 'normal'
                                                  else 'q28_compilation_pilot_incomplete'))
                    for key in ('pilot_coverage_complete', 'full_q28_admission', 'query_semantics_verified',
                                'quality_improvement_verified', 'optimizer_stage_timings_verified', 'structural_query_scaling_measured'):
                        check(evidence[key] is False)
                    check(evidence['pending_cases'] == [{'case': module.PENDING_CASE, 'executed': False,
                                                         'reason': module.PENDING_REASON}])
                    if scenario == 'normal':
                        check([item['case'] for item in evidence['records']] == list(module.EXECUTED_CASES))
                        check(verify_calls[0] == 16 and native_calls[0] == 7)
                check(not buffers)
                if events:
                    check(events[0] == 'adapter_compile')
                if len(events) > 1:
                    check(events[1] == 'version_prepare')
                if len(events) > 2:
                    check(events[2] == 'link')
            summaries.append({'scenario': scenario, 'mock_native_calls': native_calls[0],
                              'version_identity_checks': verify_calls[0], 'allocation_calls': allocation_calls[0],
                              'all_descriptors_closed': not buffers})
    check(VERSION_HELPER.read_bytes() == helper and BEFORE.read_bytes() == before)
    patch_path = PROPOSED / 'Q28-sdk-compilation-version-replay-20261028.patch'
    patch_path.write_text(''.join(difflib.unified_diff(before.decode().splitlines(keepends=True), after.decode().splitlines(keepends=True),
        fromfile='a/rbo-fixes-20261023/q28_compilation_measurement.py', tofile='b/rbo-fixes-20261023/q28_compilation_measurement.py')))
    report = {'schema_version': 'q28_version_replay_source_preparation_v1',
        'status': 'dormant_source_prepared_independent_and_native_review_pending',
        'patch': str(patch_path.relative_to(ROOT)), 'patch_sha256': digest(patch_path.read_bytes()),
        'module_before_sha256': digest(before), 'module_after_sha256': digest(after),
        'source_checks_sha256': digest(Path(__file__).read_bytes()), 'metadata_checks_passed': checks,
        'superseded_candidate': {
            'patch_sha256': '898d12bcb2d9fdce96e3740f4cd5156ae70e14a2f28c92fa8b61a18ce09cd76a',
            'module_sha256': 'e6f623843e5cd90c7fd8b554c6f1c43446edccac5d85e9b66ee171ea6bb86d40',
            'source_report_sha256': '79cd0bdfb314d3a3179c49755720f61f168bcc300a6411b5b457cd991d818dc9',
            'reason': 'Independent review found partial-allocation descriptor ownership gaps in inherited pilot setup.'},
        'depends_on': [
            {'patch': 'proposed-quality-amendments/Q28-sdk-compilation-pilot-20261028.patch',
             'sha256': '9269565adb8713961c864208020b41c0f58dadb1440d347b5834a9fa4bfd8483'},
            {'patch': 'proposed-quality-amendments/Q28-sdk-compilation-runtime-identity-20261028.patch',
             'sha256': '8712ec3993b04acf34b404818f48714588628fd6418315adcaeb57a54168a9e8'},
            {'patch': 'proposed-correctness-amendments/C31-production-version-object-replay-20261028.patch',
             'sha256': 'af0c861afcf3def464bd74707cc8e261f996c2783f92b9a1a5be3c844cba572d'}],
        'reviewed_version_helper_sha256': digest(helper), 'generated_cpp_sha256': digest(module.render_cpp().encode()),
        'generated_adapter_cpp_unchanged': True, 'all_non_execution_functions_unchanged': True,
        'resource_guards_unchanged': True, 'direct_q28_cases': 26, 'direct_admission_changed': False,
        'executable_case_vector': list(module.EXECUTED_CASES),
        'pending_case': {'case': module.PENDING_CASE, 'executed': False, 'reason': module.PENDING_REASON},
        'mocked_scenarios': summaries,
        'integration': [
            'Prepare the actual graph-bound production version object after adapter compilation, using the reviewed shared implementation.',
            'Feed its restored original link ordering through the existing stdout and archive adapters.',
            'Retain the sealed supplementary object in caller-owned descriptors until final cleanup; do not pass it into native SDK samples.',
            'Acquire each initial and per-sample descriptor individually inside its owning try/finally, so partial allocation and cancellation close every acquired descriptor.',
            'Verify object seals, digest, and bound source/tool dependency identities after linking, before and after every native sample, and at final provenance.',
            'Record supplementary production evidence on success and retain available preparation/link/native failure evidence on errors.',
            'Existing profile fingerprint now includes the imported production-version helper, with final revalidation unchanged.'],
        'limits': ['No compilation, production-generation subprocess, link, SDK execution, or actual memfd transport ran.',
                   'This remains a partial seven-cell transport pilot, with full Q28 quality admission false.',
                   'The third patch changes only the new pilot module; original two pilot patches and approved production replay helper are untouched.'],
        'native_runs': 0, 'real_process_launches': 0, 'real_memfds': 0, 'toolchain_runs': 0, 'build_runs': 0,
        'live_helper_edits': False, 'receipt_mutations': False, 'rendered_inputs_or_outputs_persisted': False}
    report_path = ROOT / 'verification/Q28-sdk-compilation-version-replay-source-preparation-20261028.json'
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'metadata_checks_passed': checks, 'patch_sha256': report['patch_sha256'],
                      'module_after_sha256': report['module_after_sha256'], 'report_sha256': digest(report_path.read_bytes())}))


if __name__ == '__main__':
    main()
