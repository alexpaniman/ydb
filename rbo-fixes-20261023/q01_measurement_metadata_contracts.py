"""Metadata-only tests: all process, memfd and signal operations are mocked.

Run only in a temporary candidate copy. No native/build operation occurs and
no rendered workloads, fixtures, rows or diagnostics are saved or printed.
"""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
from types import SimpleNamespace
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

import anonymous_child_measurement as protocol
import q01_native_child_measurement as measured
import quality_q01_scaling_checks as scaling


def main():
    checks = 0
    def check(value):
        nonlocal checks
        if not value:
            raise AssertionError('metadata_contract_failed')
        checks += 1

    # Exact-child wait and inherited stdin extend the reviewed worker protocol.
    for selector, expected in [('inherit', None), ('-', subprocess.DEVNULL), ('0', 31)]:
        process = SimpleNamespace(pid=10001, returncode=None)
        report = []
        with ExitStack() as stack:
            spawn = stack.enter_context(patch.object(protocol.subprocess, 'Popen', return_value=process))
            wait = stack.enter_context(patch.object(protocol.os, 'wait4', return_value=(10001, 0,
                SimpleNamespace(ru_utime=0.25, ru_stime=0.125, ru_maxrss=1024))))
            stack.enter_context(patch.object(protocol, '_fd', side_effect=lambda value: int(value.rsplit('/', 1)[1])))
            stack.enter_context(patch.object(protocol.os, 'fstat', return_value=SimpleNamespace(st_size=0)))
            stack.enter_context(patch.object(protocol.os, 'pwrite', side_effect=lambda fd, raw, offset: report.append(raw) or len(raw)))
            stack.enter_context(patch.object(protocol, '_seal'))
            result = protocol._child(['--wait4-worker', 'a' * 32, '/proc/self/fd/40', selector,
                                      '1', '/proc/self/fd/31', '--', '/metadata/kqprun'])
            check(result == 0)
            check(spawn.call_args.kwargs['stdin'] == expected)
            check(spawn.call_args.kwargs['pass_fds'] == (31,))
            check(wait.call_args.args == (10001, 0))
            check(json.loads(report[0])['waited_pid'] == 10001)

    scope = b'0::/user.slice/rbo-direct-012345abcdef.scope\n'
    original_read = Path.read_bytes
    def read(path):
        return scope if str(path).startswith('/proc/') and str(path).endswith('/cgroup') else original_read(path)
    for scenario in ('normal', 'nonzero', 'signal', 'malformed_report', 'mismatched_exit',
                     'timeout', 'cancel', 'remaining_descendant', 'scope_mismatch', 'binding_changed',
                     'udf_changed', 'environment_changed'):
        returncode = 7 if scenario == 'nonzero' else -signal.SIGTERM if scenario == 'signal' else 0
        process = Mock(pid=10001, returncode=returncode)
        process.communicate.return_value = (b'', b'')
        if scenario == 'timeout':
            process.communicate.side_effect = [subprocess.TimeoutExpired('metadata', 120), (b'', b''), (b'', b'')]
        if scenario == 'cancel':
            process.communicate.side_effect = [KeyboardInterrupt(), (b'', b'')]
        killed, closed = [], []
        def kill(pid, number):
            killed.append((pid, number))
            if number == 0 and scenario != 'remaining_descendant':
                raise ProcessLookupError()
        def report(fd, nonce):
            if scenario == 'malformed_report':
                raise protocol.MeasurementError('child_report_not_sealed')
            value = {'exit_code': returncode if returncode >= 0 else None,
                     'termination_signal': -returncode if returncode < 0 else None,
                     'linux_ru_maxrss_kib': 1024}
            if scenario == 'mismatched_exit':
                value['exit_code'] = 9
            return value, 'b' * 64
        with ExitStack() as stack:
            stack.enter_context(patch.object(Path, 'read_bytes', read))
            stack.enter_context(patch.object(protocol, '_memfd', side_effect=[41, 42]))
            stack.enter_context(patch.object(protocol, '_read_report', side_effect=report))
            spawn = stack.enter_context(patch.object(measured.subprocess, 'Popen', return_value=process))
            stack.enter_context(patch.object(measured.os, 'killpg', side_effect=kill))
            stack.enter_context(patch.object(measured.os, 'close', side_effect=closed.append))
            if scenario == 'scope_mismatch':
                stack.enter_context(patch.object(measured, '_scope', return_value='0' * 64))
            if scenario in ('binding_changed', 'udf_changed'):
                original_hash = measured._hash_file
                seen = {}
                def changed(path):
                    name = str(path)
                    seen[name] = seen.get(name, 0) + 1
                    rejected = ((scenario == 'binding_changed' and name == measured.__file__ and seen[name] > 1)
                                or (scenario == 'udf_changed' and name == protocol.__file__ and seen[name] > 2))
                    return '0' * 64 if rejected else original_hash(path)
                stack.enter_context(patch.object(measured, '_hash_file', side_effect=changed))
            if scenario == 'environment_changed':
                stack.enter_context(patch.object(measured, '_environment_hash', side_effect=['a' * 64, 'b' * 64]))
            try:
                completed, observation = measured.run_measured_kqprun(['/metadata/kqprun', '-u', protocol.__file__], [31, 32], {'sample': 0})
            except BaseException as exc:
                check(scenario in ('timeout', 'cancel', 'remaining_descendant', 'scope_mismatch'))
                observation = exc.q01_child_observation
                check((10001, signal.SIGKILL) in killed)
            else:
                check(completed.returncode == returncode)
                check(observation['status'] == ('unavailable' if scenario == 'malformed_report' else
                      'binding_or_exit_rejected' if scenario in ('mismatched_exit', 'binding_changed', 'udf_changed', 'environment_changed') else 'raw_observation'))
                if 'raw_child_report' in observation:
                    check(observation['linux_ru_maxrss_bytes'] == 1048576)
            check(closed == [41, 42])
            check(spawn.call_args.kwargs['start_new_session'] is True)
            check(spawn.call_args.kwargs['pass_fds'] == (31, 32, 41, 42))
            check(spawn.call_args.args[0][7] == 'inherit')
            check(observation['resource_scaling_verified'] is False)
            check(observation['python_generation_and_result_oracle_included'] is False)
            check(observation['outer_memory_guard_admission_pending'] is True)

    import native_verify as native
    import verify_series as series
    sample = {'case': next(iter(scaling.specifications())), 'rows': 2048,
              'run_id': 'a' * 32, 'kind': 'pilot', 'index': 0}
    command = series.native_command(Path('/metadata'), 'fixed', ['Q01'], 2048,
        ['quality_q01_scaling_checks'], 1, q01_sample=sample)
    for option in ('--measure-q01-child-resources', '--cases', '--q01-run-id', '--q01-sample-kind', '--q01-sample-index'):
        check(command.count(option) == 1)
    check(command[command.index('--modes') + 1] == 'new')
    ordinary = series.native_command(Path('/metadata'), 'fixed', ['C37'], 32)
    check('--measure-q01-child-resources' not in ordinary)
    check(ordinary[ordinary.index('--modes') + 1:] == ['new', 'legacy'])
    for change in ({'index': True}, {'index': -1}, {'index': 5}, {'kind': 'accepted'},
                   {'kind': 'warmup', 'index': 1}, {'run_id': 'x'}, {'rows': 32769}, {'case': 'unknown'}):
        try:
            scaling.validate_sample(dict(sample, **change))
        except ValueError:
            check(True)
        else:
            check(False)
    # Negative CLI admission must fail before source/binary/native operations.
    for extra in (['--repetitions', '2'], ['--modes', 'legacy'], ['--cases', sample['case'], sample['case']],
                  ['--generator-module', 'quality_window_checks']):
        with patch.object(sys, 'argv', command[1:] + extra), patch.object(native.subprocess, 'check_output', side_effect=AssertionError('unexpected_process')):
            try:
                native.main()
            except ValueError:
                check(True)
            else:
                check(False)

    # Exercise the actual native boundary while substituting every OS action.
    native.REVIEW = Path(scaling.g.__file__).resolve().parent
    scaling._verification_source_sha256 = hashlib.sha256(Path(scaling.__file__).read_bytes()).hexdigest()
    expected = scaling.expected_rows(sample['case'], sample['rows'])
    check(scaling.check_rows(sample['case'], expected, sample['rows'])['semantic_oracle_pass'] is True)
    check(scaling.check_rows(sample['case'], list(reversed(expected)), sample['rows'])['semantic_oracle_pass'] is False)
    swapped = [expected[1], expected[0]] + expected[2:]
    check(scaling.check_rows(sample['case'], swapped, sample['rows'])['semantic_oracle_pass'] is False)
    fds = iter(range(100, 120))
    observation = {'status': 'raw_observation', 'resource_scaling_verified': False}
    with ExitStack() as stack:
        stack.enter_context(patch.object(scaling.g, 'anonymous_text', side_effect=lambda label, value: next(fds)))
        stack.enter_context(patch.object(native.os, 'lseek'))
        stack.enter_context(patch.object(native.os, 'read', return_value=b''))
        close = stack.enter_context(patch.object(native.os, 'close'))
        stack.enter_context(patch.object(native, 'parse_rows_output', return_value=(expected, [expected], True)))
        stack.enter_context(patch.object(scaling, 'check_physical_ast', return_value={'quality_oracle_pass': False}))
        stack.enter_context(patch.object(native.subprocess, 'run', side_effect=AssertionError('unexpected_process')))
        run = stack.enter_context(patch.object(measured, 'run_measured_kqprun', return_value=(
            subprocess.CompletedProcess([], 0, b'', b''), observation)))
        record = native.run_generated(scaling, sample['case'], 'new', 2048, measurement_identity=sample)
        check(run.call_count == 1)
        check(run.call_args.kwargs == {'timeout': 120})
        check(record['semantic_oracle_pass'] is True)
        check(record['q01_child_observation'] is observation)
        check(len(record['q01_measurement_identity']['workload']) == 6)
        check(close.call_count == len(run.call_args.args[1]))
        check(record['quality_oracle_pass'] is False)

    # The existing outer guard owns the complete batch, its evidence, and all
    # descendants. Opt-in admission does not weaken its limits or cleanup.
    for change in ({}, {'stdout_overflow': True}, {'watchdog_stopped': True},
                   {'cleanup_complete': False}, {'exit_code': 75}):
        guard = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False, stdout_overflow=False)
        guard.update(change)
        with TemporaryDirectory(prefix='q01-guard-metadata-') as directory:
            here = Path(directory)
            (here / 'verification').mkdir()
            def guarded(command, cwd, fds, **options):
                raw = {'q01_child_observation': {'status': 'raw_observation', 'resource_scaling_verified': False}}
                (here / 'verification' / ('fixed-Q01-' + 'a' * 12 + '.jsonl')).write_text(json.dumps(raw) + '\n')
                return guard, b''
            with patch.object(series, 'HERE', here), patch.object(series, 'run_anonymous', side_effect=guarded) as run:
                completed, paths, records = series.invoke_native(Path('/metadata'), 'fixed', ['Q01'], 'a' * 40,
                    2048, ['quality_q01_scaling_checks'], 1, q01_sample=sample)
                check(run.call_count == 1)
                check(run.call_args.kwargs['max_scope_gib'] == 24)
                check(run.call_args.kwargs['min_host_gib'] == 36)
                check(run.call_args.kwargs['timeout'] == 48 * 3600)
                check(run.call_args.kwargs['compiler_environment'] is False)
                check(len(records['Q01']) == 1)
                observation = records['Q01'][0]['q01_child_observation']
                check(observation['outer_memory_guard_admission_pending'] is False)
                check(observation['outer_memory_guard_verified'] is (not change))
                check(observation['resource_scaling_verified'] is False)
                check(observation['status'] == ('raw_observation' if not change else 'outer_guard_rejected'))
                check(len(observation['outer_memory_guard_sha256']) == 64)

    return {'status': 'metadata_mock_contracts_passed_native_unverified', 'assertions': checks,
            'real_process_launches': 0, 'real_memfds': 0, 'native_runs': 0,
            'rendered_inputs_persisted': False, 'acceptance_thresholds_implemented': False}


if __name__ == '__main__':
    print(json.dumps(main(), sort_keys=True))
