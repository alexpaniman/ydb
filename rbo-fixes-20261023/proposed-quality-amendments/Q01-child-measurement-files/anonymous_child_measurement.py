"""Dormant Linux one-child observations inside the existing anonymous guard.

No acceptance policy is implemented here. CPU/RSS describe the waited child's
lifetime, including fork/exec, startup and setup, and kernel-attributed waited
descendants. ru_maxrss is not an aggregate cgroup peak or an operator-only peak.
The public entry point must use the caller's existing serial verification lease.
"""
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys

VERSION = 'linux_wait4_one_child_v1'
MAX_REPORT = 8192
RESERVED = ('measurement_wrapper', 'measurement_report')
REPORT_KEYS = {'schema_version', 'nonce', 'waited_pid', 'wait_status', 'exit_code',
               'termination_signal', 'core_dump_status_bit', 'guard_exit_code',
               'user_cpu_seconds', 'system_cpu_seconds', 'linux_ru_maxrss_kib'}


class MeasurementError(Exception):
    pass


def _sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def _seal(fd):
    fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
                fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)


def _memfd(name, content=b'', seal=False):
    fd = os.memfd_create(name, os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        offset = 0
        while offset < len(content):
            count = os.write(fd, content[offset:])
            if count <= 0:
                raise MeasurementError('anonymous_write_incomplete')
            offset += count
        os.lseek(fd, 0, os.SEEK_SET)
        if seal:
            _seal(fd)
        return fd
    except BaseException:
        os.close(fd)
        raise


def _fd(value):
    match = re.fullmatch(r'/proc/self/fd/([0-9]+)', value)
    if not match:
        raise MeasurementError('expected_inherited_descriptor')
    fd = int(match.group(1))
    if not os.readlink('/proc/self/fd/' + str(fd)).startswith('/memfd:'):
        raise MeasurementError('expected_anonymous_memory_descriptor')
    return fd


def _record(pid, status, usage, nonce):
    if not (os.WIFEXITED(status) or os.WIFSIGNALED(status)):
        raise MeasurementError('child_has_not_terminated')
    code = os.waitstatus_to_exitcode(status)
    return {'schema_version': VERSION, 'nonce': nonce, 'waited_pid': pid,
            'wait_status': status, 'exit_code': code if code >= 0 else None,
            'termination_signal': -code if code < 0 else None,
            'core_dump_status_bit': bool(os.WCOREDUMP(status)),
            'guard_exit_code': code if code >= 0 else 128 - code,
            'user_cpu_seconds': usage.ru_utime, 'system_cpu_seconds': usage.ru_stime,
            'linux_ru_maxrss_kib': usage.ru_maxrss}


def _child(arguments):
    if sys.platform != 'linux' or len(arguments) < 7 or arguments[0] != '--wait4-worker':
        raise MeasurementError('unsupported_measurement_protocol')
    nonce, report_path, input_index, count_text = arguments[1:5]
    if not re.fullmatch('[0-9a-f]{32}', nonce) or not count_text.isdecimal():
        raise MeasurementError('invalid_measurement_identity')
    count = int(count_text)
    if count > 4096 or len(arguments) < 7 + count or arguments[5 + count] != '--':
        raise MeasurementError('invalid_descriptor_vector')
    report_fd = _fd(report_path)
    payload = [_fd(value) for value in arguments[5:5 + count]]
    if report_fd in payload or len(set(payload)) != len(payload):
        raise MeasurementError('measurement_descriptor_alias')
    if input_index == '-':
        stdin = subprocess.DEVNULL
    elif input_index.isdecimal() and int(input_index) < count:
        stdin = payload[int(input_index)]
    else:
        raise MeasurementError('invalid_stdin_descriptor')
    command = arguments[6 + count:]
    # Same cwd/environment/session/cgroup/rlimits as the existing guard worker.
    # Only payload descriptors reach the target; neither report nor wrapper
    # source descriptors are inherited. Stdout/stderr remain the guard's streams.
    process = subprocess.Popen(command, stdin=stdin, close_fds=True,
                               pass_fds=tuple(payload), restore_signals=True)
    try:
        waited, status, usage = os.wait4(process.pid, 0)
        if waited != process.pid:
            raise MeasurementError('waited_for_unexpected_child')
        process.returncode = os.waitstatus_to_exitcode(status)
    except BaseException:
        # Cancellation cannot produce an admitted sample. The outer guard still
        # owns scope-wide timeout/memory enforcement and descendant cleanup.
        try:
            os.kill(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            _, status, _ = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
        except ChildProcessError:
            process.returncode = 70
        raise
    report = _record(waited, status, usage, nonce)
    raw = json.dumps(report, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()
    if len(raw) > MAX_REPORT or os.fstat(report_fd).st_size != 0:
        raise MeasurementError('invalid_report_size_or_reuse')
    if os.pwrite(report_fd, raw, 0) != len(raw):
        raise MeasurementError('incomplete_measurement_report')
    _seal(report_fd)
    if process.returncode < 0:
        number = -process.returncode
        if number not in (signal.SIGKILL, signal.SIGSTOP):
            signal.signal(number, signal.SIG_DFL)
        signal.pthread_sigmask(signal.SIG_UNBLOCK, {number})
        os.kill(os.getpid(), number)
        os._exit(128 + number)
    return process.returncode


def _read_report(fd, nonce):
    size = os.fstat(fd).st_size
    if not 0 < size <= MAX_REPORT:
        raise MeasurementError('missing_or_oversized_child_report')
    seals = fcntl.fcntl(fd, fcntl.F_GET_SEALS)
    required = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL
    if seals & required != required:
        raise MeasurementError('child_report_not_sealed')
    raw = os.pread(fd, size, 0)
    value = json.loads(raw)
    if (not isinstance(value, dict) or set(value) != REPORT_KEYS
        or value.get('schema_version') != VERSION or value.get('nonce') != nonce
        or type(value.get('waited_pid')) is not int or value['waited_pid'] <= 0
        or type(value.get('wait_status')) is not int or not 0 <= value['wait_status'] <= 65535
        or value.get('exit_code') is not None and type(value['exit_code']) is not int
        or value.get('termination_signal') is not None and type(value['termination_signal']) is not int
        or type(value.get('core_dump_status_bit')) is not bool
        or type(value.get('guard_exit_code')) is not int
        or type(value.get('linux_ru_maxrss_kib')) is not int or value['linux_ru_maxrss_kib'] < 0
        or any(type(value.get(key)) not in (int, float) or not math.isfinite(value[key]) or value[key] < 0
               for key in ('user_cpu_seconds', 'system_cpu_seconds'))):
        raise MeasurementError('invalid_child_resource_record')
    status = value['wait_status']
    if not (os.WIFEXITED(status) or os.WIFSIGNALED(status)):
        raise MeasurementError('child_report_is_not_terminal')
    code = os.waitstatus_to_exitcode(status)
    if (value['exit_code'] != (code if code >= 0 else None)
        or value['termination_signal'] != (-code if code < 0 else None)
        or value['guard_exit_code'] != (code if code >= 0 else 128 - code)
        or value['core_dump_status_bit'] != bool(os.WCOREDUMP(status))):
        raise MeasurementError('child_report_exit_status_disagrees')
    return value, hashlib.sha256(raw).hexdigest()


def run_measured_anonymous(argv, cwd, fds, *, stdin_fd=None, **guard_options):
    """Return (unchanged guard record, bounded stdout, raw child observation).

All guard options pass through unchanged. Stdin is /dev/null unless an existing
payload memfd name is selected; stdout follows existing guard routing/limits.
The result grants no semantic, resource-scaling or optimization acceptance.
"""
    if sys.platform != 'linux' or not hasattr(os, 'wait4'):
        raise MeasurementError('linux_wait4_required')
    if (not argv or any(not isinstance(value, str) or '\0' in value for value in argv)
        or not isinstance(fds, dict) or any(not isinstance(name, str) or not re.fullmatch('[A-Za-z0-9_]+', name)
                                          or name in RESERVED for name in fds)
        or any(type(fd) is not int or fd < 0 for fd in fds.values())
        or stdin_fd is not None and stdin_fd not in fds
        or guard_options.get('stdout_fd') is not None and guard_options['stdout_fd'] not in fds):
        raise MeasurementError('invalid_measured_anonymous_inputs')
    import anonymous_cpp_guard as guard_module
    source = Path(__file__).read_bytes()
    bindings = {'wrapper_source_sha256': hashlib.sha256(source).hexdigest(),
                'guard_source_sha256': _sha256(guard_module.__file__),
                'python_executable_sha256': _sha256(sys.executable),
                'linux_release': os.uname().release,
                'argv_template_sha256': hashlib.sha256(json.dumps(list(argv)).encode()).hexdigest(),
                'stdin_descriptor_name': stdin_fd}
    owned = {}
    try:
        owned[RESERVED[0]] = _memfd('resource-wrapper', source, seal=True)
        owned[RESERVED[1]] = _memfd('resource-report')
        names = sorted(fds)
        nonce = os.urandom(16).hex()
        command = [sys.executable, '-I', '-B', '@FD:' + RESERVED[0] + '@', '--wait4-worker', nonce,
                   '@FD:' + RESERVED[1] + '@', str(names.index(stdin_fd)) if stdin_fd is not None else '-', str(len(names)),
                   *['@FD:' + name + '@' for name in names], '--', *argv]
        guard, stdout = guard_module.run_anonymous(command, cwd, dict(fds, **owned), **guard_options)
        observation = {'schema_version': VERSION, 'status': 'unavailable', 'bindings': bindings,
                       'metric_scope': 'whole_waited_child_lifetime_including_fork_exec_startup_setup',
                       'rss_scope': 'kernel_wait4_maxrss_not_aggregate_scope_peak',
                       'query_semantics_verified': False, 'resource_scaling_verified': False,
                       'quality_improvement_verified': False}
        try:
            value, digest = _read_report(owned[RESERVED[1]], nonce)
            observation.update(raw_child_report=value, report_sha256=digest,
                               linux_ru_maxrss_bytes=value['linux_ru_maxrss_kib'] * 1024)
            unchanged = (hashlib.sha256(Path(__file__).read_bytes()).hexdigest() == bindings['wrapper_source_sha256']
                         and _sha256(guard_module.__file__) == bindings['guard_source_sha256']
                         and _sha256(sys.executable) == bindings['python_executable_sha256'])
            complete = (guard.get('watchdog_stopped') is False and guard.get('stdout_overflow') is False
                        and guard.get('cleanup_complete') is True and type(guard.get('exit_code')) is int
                        and guard['exit_code'] == value['guard_exit_code'])
            observation['status'] = 'raw_observation' if unchanged and complete else 'guard_or_binding_rejected'
        except (MeasurementError, OSError, ValueError, TypeError, KeyError) as exc:
            observation['reason'] = str(exc) if isinstance(exc, MeasurementError) else type(exc).__name__
        return guard, stdout, observation
    finally:
        for fd in owned.values():
            os.close(fd)


if __name__ == '__main__':
    try:
        raise SystemExit(_child(sys.argv[1:]))
    except BaseException as exc:
        if isinstance(exc, SystemExit):
            raise
        sys.stderr.write(type(exc).__name__ + '\n')
        raise SystemExit(70)
