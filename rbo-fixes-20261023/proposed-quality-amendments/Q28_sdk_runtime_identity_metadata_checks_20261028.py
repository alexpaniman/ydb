"""Source and mocked metadata checks; no native execution or rendered files."""
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
CANDIDATE = PROPOSED / 'q28_compilation_measurement_runtime_identity_20261028.candidate.py'
ORIGINAL = PROPOSED / 'q28_compilation_measurement_20261028.candidate.py'
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]


def main():
    before, after = ORIGINAL.read_bytes(), CANDIDATE.read_bytes()
    digest = lambda value: hashlib.sha256(value).hexdigest()
    assert digest(before) == '22c807e6780ef984ba7114b917c1d3d3e5f4914bdbd278817961e394472a75ee'
    ast.parse(after)
    module = types.ModuleType('q28_compilation_measurement')
    module.__file__ = str(CANDIDATE)
    sys.modules[module.__name__] = module
    exec(compile(after, str(CANDIDATE), 'exec'), module.__dict__)
    import anonymous_cpp_verify as direct
    import anonymous_cpp_metadata as metadata
    import quality_pairing
    import quality_scheduler_cpp_checks
    checks = 0
    def check(value):
        nonlocal checks
        if not value:
            raise AssertionError('q28_runtime_metadata_contract_' + str(checks + 1))
        checks += 1
    def rejects(call):
        try:
            call()
        except metadata.MetadataError:
            check(True)
        else:
            check(False)

    runtime = {'environment': b'A0=second\0A=first\0Z=last\0', 'affinity': b'1\n3\n'}
    with patch.object(subprocess, 'Popen', side_effect=AssertionError('unexpected_process_launch')):
        with patch.object(os, 'environb', {b'Z': b'last', b'A': b'first', b'A0': b'second'}), \
             patch.object(os, 'sched_getaffinity', return_value={3, 1}):
            check(module._runtime_snapshot() == runtime)
            identity = module._runtime_identity(runtime)
            check(identity['environment_sha256'] == digest(runtime['environment']))
            check(identity['main_thread_affinity_sha256'] == digest(runtime['affinity']))
            check(identity['main_thread_cpu_count'] == 2)
            check(identity['all_worker_thread_affinities_attested'] is False)
            check('first' not in json.dumps(identity))
            module._require_runtime_unchanged(runtime)
            check(True)
        for key in sorted(module.RUNTIME_REJECT_KEYS) + [prefix + b'OUTPUT' for prefix in module.RUNTIME_REJECT_PREFIXES]:
            for value in (b'', b'metadata'):
                with patch.object(os, 'environb', {key: value}), patch.object(os, 'sched_getaffinity', return_value={0}):
                    rejects(module._runtime_snapshot)
        for values in ({}, {b'': b'metadata'}, {b'A=B': b'metadata'}, {b'A': b'bad\0value'}, {b'A': b'x' * (2 * 1024 * 1024)}):
            with patch.object(os, 'environb', values), patch.object(os, 'sched_getaffinity', return_value={0}):
                rejects(module._runtime_snapshot)
        for cpus in (set(), {1024}):
            with patch.object(os, 'environb', {b'A': b'metadata'}), patch.object(os, 'sched_getaffinity', return_value=cpus):
                rejects(module._runtime_snapshot)
        for changed in ({**runtime, 'environment': b'A=changed\0'}, {**runtime, 'affinity': b'2\n'}):
            with patch.object(module, '_runtime_snapshot', return_value=changed):
                rejects(lambda: module._require_runtime_unchanged(runtime))
        for malformed in ('', 'TableServiceConfig {} TableServiceConfig {}',
                          'TableServiceConfig { SpillingServiceConfig {} }',
                          'TableServiceConfig { LocalFileConfig {} }',
                          'TableServiceConfig {} KQPConfig { Settings { Name: "_KqpEnableSpilling" } }'):
            rejects(lambda: module._disable_local_spilling(malformed))
        payloads = module._snapshot_inputs()
        check(tuple(record['case'] for _, record in payloads) == module.EXECUTED_CASES)
        check(len(payloads) == 7 and len(module.CASES) == 8)
        check(module.PENDING_CASE not in module.EXECUTED_CASES)
        for payload, record in payloads:
            check(b'SpillingServiceConfig { LocalFileConfig { Enable: false } }' in payload['config'])
            check(record['pilot_local_file_spilling_enabled'] is False)
            check(record['optimizer_mode_config_verified'] is True)
            check(record['table_service_overrides']['EnableSpillingNodes'] == 'None')
            check(all(digest(value) == record['input_sha256'][key] for key, value in payload.items()))
        cpp = module.render_cpp().encode()
        check(cpp.count(b'RuntimeEnvironment() == expectedEnvironment') == 2)
        check(cpp.count(b'RuntimeAffinity() == expectedAffinity') == 2)
        check(cpp.index(b'phase = "runtime_identity"') < cpp.index(b'TKikimrRunner runner'))
        check(cpp.index(b'phase = "runtime_identity_final"') > cpp.index(b'Check(!cached && duration'))
        check(b'localFiles.HasEnable() && !localFiles.GetEnable()' in cpp)
        check(b'Read(argv[2])' in cpp and b'Read(argv[3])' in cpp and b'index = 7' in cpp)
        check(all(payload['query'] not in cpp and payload['schema'] not in cpp for payload, _ in payloads))
        check(direct.GENERATORS['Q28'] == 'quality_scheduler_cpp_checks')
        check(len(quality_scheduler_cpp_checks.cases()) == 26)
        sample = dict(completed=True, phase='complete', stats_present=True, compilation_present=True,
                      from_cache=False, server_compilation_duration_us=1000, server_compilation_cpu_us=100,
                      result_set_count=1, runtime_environment_verified=True, runtime_affinity_verified=True)
        check(module.parse_measurement(json.dumps(sample).encode(), 0)['measurement_valid'])
        for field in ('runtime_environment_verified', 'runtime_affinity_verified'):
            for value in (False, 1, None):
                rejects(lambda: module.parse_measurement(json.dumps(dict(sample, **{field: value})).encode(), 0))
            missing = dict(sample)
            del missing[field]
            rejects(lambda: module.parse_measurement(json.dumps(missing).encode(), 0))
        for field, value in (('from_cache', True), ('stats_present', False), ('compilation_present', False),
                             ('server_compilation_cpu_us', True), ('server_compilation_duration_us', 0),
                             ('server_compilation_cpu_us', 0), ('result_set_count', 2), ('phase', 'unknown')):
            rejects(lambda: module.parse_measurement(json.dumps(dict(sample, **{field: value})).encode(), 0))
        for phase in ('runtime_identity', 'runtime_identity_final'):
            failure = dict(sample, completed=False, phase=phase, runtime_environment_verified=False)
            check(module.parse_measurement(json.dumps(failure).encode(), 1)['measurement_valid'] is False)

        # Exercise the complete orchestration with fake descriptors/processes.
        # Values stay in local byte buffers; no real memfd, socket or process.
        receipt = {'metadata': {'profile': 'rbo', 'compile_argv': ['metadata-cc'], 'compile_cwd': '/metadata',
                               'compile_environment': {}, 'link_argv': ['metadata-ld'], 'link_cwd': '/metadata',
                               'link_environment': {}}, 'build': {'target': 'ydb/core/kqp/ut/rbo'}, 'files': []}
        receipt_bytes = json.dumps(receipt).encode()
        args = types.SimpleNamespace(command='measure-compilation', bug='Q28', phase='baseline', sample_index=0,
            receipt=types.SimpleNamespace(read_bytes=lambda: receipt_bytes, resolve=lambda: Path('/metadata/receipt')),
            worktree=Path('/metadata'), compile_timeout=1, link_timeout=1, native_timeout=1)
        orchestration = []
        for changed_at in (None, 2, 3, 4, 16):
            descriptors, counter = {}, [100]
            runtime_calls, native_calls, guard_calls = [0], [0], []
            def memfd(label, value=b'', **kwargs):
                counter[0] += 1
                descriptors[counter[0]] = value
                return counter[0]
            def close(fd):
                check(fd in descriptors)
                del descriptors[fd]
            def fd_digest(fd):
                return {'sha256': digest(descriptors[fd]), 'size_bytes': len(descriptors[fd])}
            def runtime_snapshot():
                runtime_calls[0] += 1
                if runtime_calls[0] == changed_at:
                    return dict(runtime, affinity=b'2\n')
                return runtime
            def run(argv, cwd, fds, **kwargs):
                guard_calls.append(dict(kwargs))
                record = {'exit_code': 0, 'cleanup_complete': True, 'watchdog_stopped': False,
                          'stdout_overflow': False, 'environment_sha256': direct._json_digest({})}
                if kwargs.get('stdout_fd'):
                    descriptors[fds[kwargs['stdout_fd']]] = b'metadata-object-or-executable'
                else:
                    native_calls[0] += 1
                    check(argv[:4] == ['@FD:executable@', '@FD:report@', '@FD:runtime_environment@', '@FD:runtime_affinity@'])
                    check(descriptors[fds['runtime_environment']] == runtime['environment'])
                    check(descriptors[fds['runtime_affinity']] == runtime['affinity'])
                    check(kwargs['compiler_environment'] is False and kwargs['retain_stdout'] == 0)
                    check(kwargs['max_scope_gib'] == 12)
                    descriptors[fds['report']] = json.dumps(sample).encode()
                return record, b''
            with ExitStack() as stack:
                stack.enter_context(patch.object(direct, '_expected_revision', return_value='a' * 40))
                stack.enter_context(patch.object(metadata, 'verify_receipt'))
                stack.enter_context(patch.object(quality_pairing, 'profile_fingerprint', return_value='b' * 64))
                stack.enter_context(patch.object(module, '_runtime_snapshot', side_effect=runtime_snapshot))
                stack.enter_context(patch.object(direct, '_memfd', side_effect=memfd))
                stack.enter_context(patch.object(direct, '_fd_digest', side_effect=fd_digest))
                stack.enter_context(patch.object(direct, '_stdout_link_arguments', return_value=(['metadata-ld'], 'metadata')))
                stack.enter_context(patch.object(direct, '_archive_response_arguments', return_value=(['metadata-ld'], {'responses': []})))
                stack.enter_context(patch.object(direct, 'run_anonymous', side_effect=run))
                stack.enter_context(patch.object(os, 'fchmod'))
                stack.enter_context(patch.object(os, 'close', side_effect=close))
                stack.enter_context(patch.object(os, 'pread', side_effect=lambda fd, size, offset: descriptors[fd][offset:offset+size]))
                try:
                    record, code = module.execute_measurements(args)
                except metadata.MetadataError as error:
                    check(changed_at is not None)
                    check(str(error) == 'q28_runtime_identity_changed')
                    check(error.verification_stage in ('q28_compilation_pilot_native', 'q28_compilation_pilot_final_provenance'))
                else:
                    check(changed_at is None and code == 0)
                    check(record['status'] == 'q28_compilation_pilot_partial_recorded')
                    check(record['pilot_coverage_complete'] is False and record['full_q28_admission'] is False)
                    check(record['pending_cases'] == [{'case': module.PENDING_CASE, 'executed': False, 'reason': module.PENDING_REASON}])
                    check([item['case'] for item in record['records']] == list(module.EXECUTED_CASES))
                    check(all(item['measurement_valid'] for item in record['records']))
                    check(record['query_semantics_verified'] is False and record['quality_improvement_verified'] is False)
                    check(record['runtime_identity'] == module._runtime_identity(runtime))
                check(not descriptors)
                check(guard_calls[1]['max_scope_gib'] == 36)
                check(native_calls[0] == {None: 7, 2: 0, 3: 1, 4: 1, 16: 7}[changed_at])
            orchestration.append({'runtime_change_at_snapshot': changed_at, 'mock_native_calls': native_calls[0]})

    patch_path = PROPOSED / 'Q28-sdk-compilation-runtime-identity-20261028.patch'
    patch_path.write_text(''.join(difflib.unified_diff(before.decode().splitlines(keepends=True), after.decode().splitlines(keepends=True),
        fromfile='a/rbo-fixes-20261023/q28_compilation_measurement.py', tofile='b/rbo-fixes-20261023/q28_compilation_measurement.py')))
    report = {'schema_version': 'q28_runtime_identity_source_preparation_v1',
        'status': 'dormant_source_prepared_independent_and_native_review_pending',
        'patch': str(patch_path.relative_to(ROOT)), 'patch_sha256': digest(patch_path.read_bytes()),
        'apply_after_patch': 'proposed-quality-amendments/Q28-sdk-compilation-pilot-20261028.patch',
        'apply_after_patch_sha256': '9269565adb8713961c864208020b41c0f58dadb1440d347b5834a9fa4bfd8483',
        'module_before_sha256': digest(before), 'module_after_sha256': digest(after),
        'source_checks_sha256': digest(Path(__file__).read_bytes()), 'metadata_checks_passed': checks,
        'generated_cpp_sha256': digest(cpp), 'generated_cpp_bytes': len(cpp),
        'all_intended_cases': list(module.CASES), 'executable_cases': list(module.EXECUTED_CASES),
        'pending_cases': [{'case': module.PENDING_CASE, 'executed': False, 'reason': module.PENDING_REASON}],
        'mocked_orchestration': orchestration,
        'native_runs': 0, 'real_memfds': 0, 'real_process_launches': 0, 'builds': 0,
        'live_helper_edits': False, 'rendered_SQL_CPP_plans_results_or_environment_persisted': False,
        'direct_q28_case_count': 26, 'direct_admission_changed': False, 'shared_guard_changed': False,
        'pinned_source_evidence': {
            'base': 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2',
            'fixed': '567d22ff15c8036cbc9741e0bb29daa6c140f0c1',
            'files_identical_at_base_and_fixed': True,
            'files': [
                {'path': 'ydb/core/protos/table_service_config.proto', 'line': 97,
                 'sha256': 'ad647255a24df1060dd1fa039ef600ee0a022a1dca4ba120bba969eff456dc4e',
                 'meaning': 'Local file spilling defaults enabled.'},
                {'path': 'ydb/core/kqp/ut/common/kqp_ut_common.cpp', 'line': 148,
                 'sha256': '92589cf8a3a3a6b3a7dab33f55127cfecff3b7f815a46d63d78f60ae7179af99',
                 'meaning': 'Runner adds _KqpEnableSpilling when the local service is enabled.'},
                {'path': 'ydb/core/kqp/provider/yql_kikimr_settings.cpp', 'line': 239,
                 'sha256': '59c460367dc0d317cd1dd9f12e21faf5b3a48a3ec47aeaf18e960e91f7143f41',
                 'meaning': 'SpillingEnabled resolves that internal flag.'},
                {'path': 'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp', 'line': 609,
                 'sha256': '593b73e3bfe99b67f9957cb4fe00da320ee0262af75c830929b50b42b8290896',
                 'meaning': 'Shared-channel eligibility also requires the internal spilling flag.'}]},
        'runtime_contract': [
            'Full caller environment bytes and main-thread affinity are snapshotted in RAM; only hashes and CPU count are recorded.',
            'Caller identity is checked before every native sample, immediately after each normal return, and at final provenance.',
            'Native C++ compares its actual environ bytes and sched_getaffinity result with memfd snapshots before runner startup and after the cold compilation statistics check.',
            'The native comparison attests exact bytes against parent-bound hashes. It does not assume systemd preserves the environment and does not rely on the guard override hash.',
            'Test redirection and loader/profile-output override names are rejected before any compilation or native launch.',
            'All seven executable configurations explicitly disable the local file spilling service; adapter checks explicit false before TKikimrRunner creation.'],
        'limits': [
            'Actual C++ compilation, SDK execution, and systemd environment equality remain unverified. Any extra or changed environment entry rejects the sample.',
            'Affinity attestation covers the adapter main thread before/after the target, not every actor worker thread or transient mid-query changes.',
            'The spilling-dependent shape is intentionally unexecuted; disabling the local service changes shared-channel eligibility. RAM-only service/file behavior must be validated before restoring it.',
            'The seven measurements are a partial transport pilot, not full Q28 quality admission, structural scaling, pure optimizer time, or separate semantic verification.',
            'TKikimrRunner port-lock metadata and sockets remain ordinary test infrastructure. No claim of a filesystem sandbox or absence of every runner side effect is made.']}
    report_path = ROOT / 'verification/Q28-sdk-compilation-runtime-identity-source-preparation-20261028.json'
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'metadata_checks_passed': checks, 'patch_sha256': report['patch_sha256'],
                      'module_after_sha256': report['module_after_sha256'], 'report_sha256': digest(report_path.read_bytes())}))


if __name__ == '__main__':
    main()
