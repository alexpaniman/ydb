"""Opt-in kqprun wait4 observation inside the existing outer native scope.

No new systemd scope is created. The caller must select one cell, one repetition,
and new optimizer mode; the existing serial guard still owns every descendant.
This supplies raw whole-child observations only and implements no admission.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys

VERSION = 'q01_inherited_scope_child_observation_v1'


class NativeMeasurementError(Exception):
    pass


def _digest(raw):
    return hashlib.sha256(raw).hexdigest()


def _hash_file(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def _environment_hash():
    return _digest(json.dumps(dict(os.environ), sort_keys=True, separators=(',', ':')).encode())


def _scope():
    raw = Path('/proc/self/cgroup').read_bytes()
    if not re.search(rb'/rbo-direct-[0-9a-f]{12}\.scope(?:\n|$)', raw):
        raise NativeMeasurementError('existing_native_scope_required')
    return _digest(raw)


def _kill_owned_group(process):
    # start_new_session below gives this wrapper and its target a private group;
    # it does not change their membership in the existing memory-control scope.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _drain_after_stop(process):
    try:
        return process.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        for stream in (process.stdout, process.stderr):
            if stream is not None:
                stream.close()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        # The outer native scope still owns final descendant cleanup. A cell
        # with incomplete local cleanup cannot provide a raw accepted sample.
        raise NativeMeasurementError('measurement_cleanup_incomplete') from None


def run_measured_kqprun(command, descriptors, identity, *, timeout=120):
    """Return CompletedProcess and an unaccepted numeric child observation."""
    import anonymous_child_measurement as protocol
    if (timeout != 120 or not command or Path(command[0]).name != 'kqprun'
        or any(not isinstance(arg, str) or '\0' in arg for arg in command)
        or len(set(descriptors)) != len(descriptors)
        or any(type(fd) is not int or fd < 3 for fd in descriptors)
        or command.count('-u') > 1 or command[-1] == '-u'
        or not isinstance(identity, dict)):
        raise NativeMeasurementError('invalid_q01_measurement_request')
    scope = _scope()
    source = Path(protocol.__file__).read_bytes()
    udf = command[command.index('-u') + 1] if '-u' in command else None
    bindings = {'protocol_source_sha256': _digest(source),
                'integration_source_sha256': _hash_file(__file__),
                'outer_guard_source_sha256': _hash_file(Path(__file__).with_name('anonymous_cpp_guard.py')),
                'python_executable_sha256': _hash_file(sys.executable),
                'identity_sha256': _digest(json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()),
                'command_sha256': _digest(json.dumps(command, separators=(',', ':')).encode()),
                'inherited_scope_sha256': scope,
                'inherited_environment_sha256': _environment_hash(),
                'udf_library_sha256': _hash_file(udf) if udf else None,
                'linux_release': os.uname().release,
                'machine': os.uname().machine,
                'cpu_affinity': sorted(os.sched_getaffinity(0)),
                'timeout_seconds': timeout}
    owned = []
    process = None
    observation = {'schema_version': VERSION, 'status': 'unavailable', 'bindings': bindings,
                   'metric_scope': 'whole_kqprun_lifetime_including_startup_schema_fixtures_and_target',
                   'kernel_attributed_waited_descendants_included': True,
                   'rss_scope': 'linux_wait4_maxrss_not_aggregate_scope_or_operator_peak',
                   'python_generation_and_result_oracle_included': False,
                   'outer_memory_guard_admission_pending': True,
                   'resource_scaling_verified': False, 'quality_improvement_verified': False,
                   'query_semantics_verified': False}
    try:
        wrapper_fd = protocol._memfd('q01-measurement-wrapper', source, seal=True)
        owned.append(wrapper_fd)
        report_fd = protocol._memfd('q01-measurement-report')
        owned.append(report_fd)
        nonce = os.urandom(16).hex()
        path = lambda fd: '/proc/self/fd/' + str(fd)
        argv = [sys.executable, '-I', '-B', path(wrapper_fd), '--wait4-worker', nonce,
                path(report_fd), 'inherit', str(len(descriptors)),
                *[path(fd) for fd in descriptors], '--', *command]
        process = subprocess.Popen(argv, pass_fds=tuple(descriptors) + tuple(owned),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        # The wrapper's target inherits this unchanged cgroup and environment.
        # No subprocess request is made to a systemd service or another scope.
        if _digest(Path('/proc/' + str(process.pid) + '/cgroup').read_bytes()) != scope:
            raise NativeMeasurementError('measurement_scope_changed')
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            _kill_owned_group(process)
            _drain_after_stop(process)
            observation['reason'] = 'query_timeout'
            exc.q01_child_observation = observation
            raise
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            pass
        else:
            _kill_owned_group(process)
            raise NativeMeasurementError('measurement_target_left_descendants')
        completed = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
        try:
            report, digest = protocol._read_report(report_fd, nonce)
            expected_exit = report['exit_code'] if report['exit_code'] is not None else -report['termination_signal']
            unchanged = (_scope() == scope and _hash_file(protocol.__file__) == bindings['protocol_source_sha256']
                         and _hash_file(__file__) == bindings['integration_source_sha256']
                         and _hash_file(Path(__file__).with_name('anonymous_cpp_guard.py')) == bindings['outer_guard_source_sha256']
                         and _environment_hash() == bindings['inherited_environment_sha256']
                         and sorted(os.sched_getaffinity(0)) == bindings['cpu_affinity']
                         and (_hash_file(udf) if udf else None) == bindings['udf_library_sha256']
                         and _hash_file(sys.executable) == bindings['python_executable_sha256'])
            observation.update(raw_child_report=report, report_sha256=digest,
                               linux_ru_maxrss_bytes=report['linux_ru_maxrss_kib'] * 1024,
                               target_group_empty=True, local_cleanup_complete=True)
            observation['status'] = 'raw_observation' if unchanged and process.returncode == expected_exit else 'binding_or_exit_rejected'
        except (protocol.MeasurementError, OSError, ValueError, TypeError, KeyError) as exc:
            observation['reason'] = str(exc) if isinstance(exc, protocol.MeasurementError) else type(exc).__name__
        return completed, observation
    except BaseException as exc:
        if process is not None:
            # The wrapper may already have exited while descendants retain a
            # pipe or remain alive. Stop the private group in that case too.
            _kill_owned_group(process)
            try:
                _drain_after_stop(process)
            except NativeMeasurementError:
                observation['local_cleanup_complete'] = False
            else:
                observation['local_cleanup_complete'] = True
        if not hasattr(exc, 'q01_child_observation'):
            observation['reason'] = str(exc) if isinstance(exc, NativeMeasurementError) else type(exc).__name__
            exc.q01_child_observation = observation
        raise
    finally:
        for fd in owned:
            os.close(fd)
