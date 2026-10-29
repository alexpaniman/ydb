"""Independent metadata/OS-mock review; no target or real memfd operations."""
import ast
from contextlib import ExitStack
import hashlib
import importlib
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
from unittest.mock import Mock, patch


def main():
    sys.dont_write_bytecode = True
    root = Path(__file__).resolve().parent.parent
    prep = json.loads((root / 'verification/Q01-native-child-measurement-source-preparation-20261028.json').read_bytes())
    plan_path = root / 'verification/idle-artifact-application-plan-20261028-latest.json'
    plan = json.loads(plan_path.read_bytes())
    checks = 0
    def check(value):
        nonlocal checks
        if not value:
            raise AssertionError('Q01 independent metadata contract failed')
        checks += 1
    with tempfile.TemporaryDirectory(prefix='q01-independent-metadata-') as folder:
        base = Path(folder); here = base / root.name; here.mkdir()
        for path in root.glob('*.py'):
            shutil.copyfile(path, here / path.name)
        for name in plan['files']:
            if (root / name).is_file():
                shutil.copyfile(root / name, here / name)
        additional = [prep['dependencies'][1], {'path': prep['patch'], 'sha256': prep['patch_sha256']}]
        for entry in plan['patches'] + additional:
            path = root / entry.get('patch', entry.get('path'))
            check(hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'])
            result = subprocess.run(['git', 'apply', str(path)], cwd=base, capture_output=True)
            check(result.returncode == 0)
        for name, identities in prep['files'].items():
            path = here / name
            check(hashlib.sha256(path.read_bytes()).hexdigest() == identities['after_sha256'])
            ast.parse(path.read_bytes()); check(True)
        sys.path[:0] = [str(here), str(root), str(root.parent / 'rbo-review-20261021')]
        protocol = importlib.import_module('anonymous_child_measurement')
        measured = importlib.import_module('q01_native_child_measurement')
        scaling = importlib.import_module('quality_q01_scaling_checks')
        author = importlib.import_module('q01_measurement_metadata_contracts').main()
        check(author['assertions'] == prep['checks']['new_mock_transport_and_driver']['assertions']
              and author['real_process_launches'] == 0)
        # Independent direct membership oracle at boundaries and in the middle;
        # it does not use the candidate's prefix sums or monotone queues.
        sampled_rows = 0
        for size in scaling.ROWS:
            for case, spec in scaling.specifications().items():
                expected = scaling.expected_rows(case, size)
                check(len(expected) == size)
                if size == 2048:
                    check(scaling.check_rows(case, expected, size)['semantic_oracle_pass'])
                    check(not scaling.check_rows(case, list(reversed(expected)), size)['semantic_oracle_pass'])
                for identity in (1, 2, 3, size // 3, size // 2, size - 1, size):
                    ids = [index for index in range(1, size + 1) if index % 3 == identity % 3]
                    if spec['kind'] == 'range':
                        members = [index for index in ids if identity - spec['frame_width'] <= index <= identity + 1]
                    else:
                        position = ids.index(identity)
                        members = ids[max(0, position - spec['frame_width']):position + 2]
                    values = [index % 97 - 48 for index in members if index % 13 >= 9]
                    row = {'id': identity}
                    if spec['minmax']:
                        row.update(lo=min(values) if values else None, hi=max(values) if values else None)
                    if spec['collectors']:
                        row.update(c=len(members), cv=len(values), s=sum(values) if values else None)
                    check(expected[identity - 1] == row); sampled_rows += 1
                del expected
        scope = b'0::/user.slice/rbo-direct-012345abcdef.scope\n'
        original_read = Path.read_bytes
        for scenario in ('spawn_failure', 'first_memfd_failure', 'second_memfd_failure',
                         'scope_read_failure', 'communicate_failure', 'cleanup_incomplete', 'affinity_changed'):
            process = Mock(pid=27182, returncode=0)
            process.communicate.return_value = (b'', b'')
            if scenario == 'communicate_failure':
                process.communicate.side_effect = [OSError('metadata'), (b'', b'')]
            if scenario == 'cleanup_incomplete':
                process.communicate.side_effect = subprocess.TimeoutExpired('metadata', 120)
                process.wait.side_effect = subprocess.TimeoutExpired('metadata', 2)
            closed, killed = [], []
            def read(path):
                if str(path).startswith('/proc/') and str(path).endswith('/cgroup'):
                    if scenario == 'scope_read_failure' and '/27182/' in str(path):
                        raise FileNotFoundError('metadata')
                    return scope
                return original_read(path)
            def kill(pid, number):
                killed.append((pid, number))
                if number == 0:
                    raise ProcessLookupError()
            with ExitStack() as stack:
                stack.enter_context(patch.object(Path, 'read_bytes', read))
                effect = (OSError('metadata') if scenario == 'first_memfd_failure' else
                          [61, OSError('metadata')] if scenario == 'second_memfd_failure' else [61, 62])
                stack.enter_context(patch.object(protocol, '_memfd', side_effect=effect))
                stack.enter_context(patch.object(protocol, '_read_report', return_value=(
                    {'exit_code': 0, 'termination_signal': None, 'linux_ru_maxrss_kib': 1}, 'b' * 64)))
                stack.enter_context(patch.object(measured.subprocess, 'Popen',
                    side_effect=OSError('metadata') if scenario == 'spawn_failure' else None,
                    return_value=process))
                stack.enter_context(patch.object(measured.os, 'killpg', side_effect=kill))
                stack.enter_context(patch.object(measured.os, 'close', side_effect=closed.append))
                if scenario == 'affinity_changed':
                    stack.enter_context(patch.object(measured.os, 'sched_getaffinity', side_effect=[{0}, {1}]))
                try:
                    _, observation = measured.run_measured_kqprun(['/metadata/kqprun'], [31, 32], {'metadata': True})
                except BaseException as exc:
                    check(scenario != 'affinity_changed')
                    observation = exc.q01_child_observation
                    check(observation['status'] == 'unavailable')
                    if scenario in ('scope_read_failure', 'communicate_failure', 'cleanup_incomplete'):
                        check(killed and all(pid == 27182 for pid, number in killed))
                        check((27182, signal.SIGKILL) in killed)
                    if scenario == 'cleanup_incomplete':
                        check(observation['local_cleanup_complete'] is False)
                        check(process.stdout.close.called and process.stderr.close.called)
                else:
                    check(scenario == 'affinity_changed' and observation['status'] == 'binding_or_exit_rejected')
                check(closed == ([] if scenario == 'first_memfd_failure' else [61] if scenario == 'second_memfd_failure' else [61, 62]))
                check(observation['resource_scaling_verified'] is False)
                check(observation['outer_memory_guard_admission_pending'] is True)
        # Unsupported caller envelopes must fail before any process launch.
        with patch.object(measured.subprocess, 'Popen', side_effect=AssertionError('unexpected real process')):
            for args in (([], [31], {}), (['/metadata/not-runner'], [31], {}),
                         (['/metadata/kqprun'], [31, 31], {}), (['/metadata/kqprun'], [True], {}),
                         (['/metadata/kqprun', '-u'], [31], {})):
                try:
                    measured.run_measured_kqprun(*args)
                except measured.NativeMeasurementError:
                    check(True)
                else:
                    check(False)
        return {'schema_version': 'q01_native_child_independent_metadata_v1', 'assertions_passed': checks,
                'author_mock_assertions_rerun': author['assertions'], 'independent_oracle_positions': sampled_rows,
                'source_patch_sha256': prep['patch_sha256'],
                'idle_plan_sha256': hashlib.sha256(plan_path.read_bytes()).hexdigest(), 'idle_patch_count': len(plan['patches']),
                'temporary_latest_stack_plus_protocol_and_integration_application': True,
                'real_process_launches': 0, 'real_memfds': 0, 'native_runs': 0, 'builds': 0,
                'graphs': 0, 'toolchain_runs': 0, 'generated_inputs_or_results_persisted': False,
                'production_or_live_helper_edits': False}


if __name__ == '__main__':
    print(json.dumps(main(), sort_keys=True))
