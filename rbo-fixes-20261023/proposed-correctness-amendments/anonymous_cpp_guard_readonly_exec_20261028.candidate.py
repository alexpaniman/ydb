"""One scoped anonymous compiler/linker/native process; no generated file output.

This deliberately does not modify the currently running ya-build guard. The
worker reopens the parent's memfds, then explicitly inherits them through the
compiler driver and linker. Only bounded native stdout remains in RAM; compiler
diagnostics are consumed and hashed without returning their text.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import threading
import time
import uuid

from memory_safe_build import (GIB, available_memory, group_rss, scope_control_group,
                               scope_memory, scope_rss, scope_stopped, stop_scope)

HERE = Path(__file__).resolve().parent


class AnonymousGuardError(Exception):
    pass



def _environment_overrides(value, compiler_environment):
    value = {} if value is None else value
    allowed = {'ARCADIA_ROOT_DISTBUILD', 'CPATH', 'DYLD_LIBRARY_PATH', 'LIBRARY_PATH', 'SDKROOT'}
    if (not isinstance(value, dict) or any(key not in allowed for key in value)
        or any(not isinstance(item, str) or '\0' in item for item in value.values())
        or any(value.get(key, '') != '' for key in ('CPATH', 'LIBRARY_PATH', 'SDKROOT'))
        or value and not compiler_environment):
        raise AnonymousGuardError('unaudited_compiler_environment_override')
    return dict(value)


NATIVE_EXECUTABLE_SEALS = (fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW
                           | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)


def _native_executable_name(argv):
    if argv and argv[0].startswith('@FD:') and argv[0].endswith('@'):
        name = argv[0][4:-1]
        if not name or '@' in name:
            raise AnonymousGuardError('invalid_native_executable_descriptor')
        return name
    return None


def _check_native_executable(fd):
    if ((fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE) != os.O_RDONLY
        or fcntl.fcntl(fd, fcntl.F_GET_SEALS) != NATIVE_EXECUTABLE_SEALS):
        raise AnonymousGuardError('native_executable_must_be_read_only_and_sealed')


def _worker(manifest_path):
    """Invoked inside the transient scope; never writes a manifest or source."""
    spec = json.loads(Path(manifest_path).read_bytes())
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    read_only = spec.get('read_only_fds', [])
    if (not isinstance(read_only, list) or any(not isinstance(name, str) for name in read_only)
        or len(set(read_only)) != len(read_only) or not set(read_only).issubset(spec['fds'])
        or spec.get('stdout_fd') in read_only):
        raise AnonymousGuardError('invalid_read_only_descriptor_mapping')
    native_name = _native_executable_name(spec['argv'])
    if native_name is not None and native_name not in read_only:
        raise AnonymousGuardError('native_executable_descriptor_requires_read_only_transport')
    opened = {}
    try:
        for name, path in spec['fds'].items():
            # All paths must be already-open memfds belonging to the parent.
            if not path.startswith('/proc/') or '/fd/' not in path:
                raise AnonymousGuardError('invalid_anonymous_descriptor_path')
            fd = os.open(path, (os.O_RDONLY if name in read_only else os.O_RDWR) | os.O_CLOEXEC)
            opened[name] = fd
            if not os.readlink('/proc/self/fd/' + str(fd)).startswith('/memfd:'):
                raise AnonymousGuardError('descriptor_is_not_anonymous_memory')
        if native_name is not None:
            _check_native_executable(opened[native_name])
        command = []
        for argument in spec['argv']:
            for name, fd in opened.items():
                argument = argument.replace('@FD:' + name + '@', '/proc/self/fd/' + str(fd))
            if '@FD:' in argument:
                raise AnonymousGuardError('unresolved_anonymous_descriptor')
            command.append(argument)
        env = dict(os.environ)
        if spec.get('compiler_environment', True):
            # Eliminate implicit compiler/cache/profile outputs and inherited
            # search paths. Native database batches retain their environment.
            for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
                        'LD_PRELOAD', 'LD_LIBRARY_PATH', 'CCC_OVERRIDE_OPTIONS',
                        'LLVM_PROFILE_FILE', 'CLANG_CRASH_DIAGNOSTICS_DIR',
                        'LLD_REPRODUCE', 'AS_SECURE_LOG_FILE', 'CLANG_MODULE_CACHE_PATH'):
                env.pop(key, None)
            for key in tuple(env):
                if key.startswith(('CC_PRINT_', 'CC_LOG_')):
                    env.pop(key)
            env.update(_environment_overrides(spec.get('environment'), True))
            # Audited input environment cannot override these output guards.
            env.update({'LC_ALL': 'C', 'TMPDIR': '/proc/self/fd/anonymous-no-temp-directory'})
        elif spec.get('environment'):
            raise AnonymousGuardError('unaudited_compiler_environment_override')
        output = opened[spec['stdout_fd']] if spec.get('stdout_fd') else None
        result = subprocess.run(command, cwd=spec['cwd'], env=env,
                                stdout=output, pass_fds=tuple(opened.values()), check=False)
        return result.returncode if result.returncode >= 0 else 128 - result.returncode
    finally:
        for fd in opened.values():
            os.close(fd)


def _consume(stream, result, retain_limit):
    digest = hashlib.sha256()
    count = 0
    retained = bytearray()
    try:
        for block in iter(lambda: stream.read(65536), b''):
            digest.update(block)
            count += len(block)
            if len(retained) < retain_limit:
                retained.extend(block[:retain_limit - len(retained)])
    finally:
        stream.close()
    result.update(sha256=digest.hexdigest(), size_bytes=count,
                  retained=bytes(retained), overflow=count > retain_limit if retain_limit else False)


def run_anonymous(argv, cwd, fds, *, stdout_fd=None, timeout=600,
                  max_scope_gib=24, min_host_gib=36, retain_stdout=1024 * 1024,
                  compiler_environment=True, metrics_path=None, environment=None):
    """Return (sanitized process evidence, bounded RAM-only stdout).

    Inputs are descriptor numbers owned by the caller. argv uses @FD:name@
    tokens. The caller is responsible for the global serial execution lease.
    No scope survives an abnormal return. Normal direct jobs also have no
    legitimate persistent cache daemon, so leftover scope members are stopped.
    """
    if not argv or timeout <= 0 or max_scope_gib <= 0 or min_host_gib < 1:
        raise AnonymousGuardError('invalid_guard_parameters')
    if any(not isinstance(fd, int) for fd in fds.values()):
        raise AnonymousGuardError('invalid_descriptor_mapping')
    read_only = [name for name, fd in fds.items()
                 if (fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE) == os.O_RDONLY]
    if stdout_fd in read_only:
        raise AnonymousGuardError('anonymous_stdout_descriptor_is_read_only')
    native_name = _native_executable_name(argv)
    if native_name is not None:
        if native_name not in fds:
            raise AnonymousGuardError('invalid_native_executable_transport')
        _check_native_executable(fds[native_name])
    environment = _environment_overrides(environment, compiler_environment)
    environment_sha256 = hashlib.sha256(json.dumps(environment, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    start = time.monotonic()
    metrics = {'elapsed_seconds': 0, 'peak_scope_usage_bytes': 0,
               'peak_group_rss_bytes': 0, 'minimum_host_available_bytes': available_memory(),
               'max_scope_gib': max_scope_gib, 'min_host_available_gib': min_host_gib,
               'watchdog_stopped': False, 'cleanup_complete': False,
               'scope_collection_after_completion': False, 'environment_sha256': environment_sha256}
    if native_name is not None:
        metrics['native_executable_transport'] = 'sealed_read_only_memfd_v1'
    if metrics['minimum_host_available_bytes'] < (min_host_gib + 4) * GIB:
        return dict(metrics, exit_code=75, watchdog_stopped=True,
                    stop_reason='insufficient_starting_memory', cleanup_complete=True), b''
    unit = 'rbo-direct-' + uuid.uuid4().hex[:12]
    descriptor = os.memfd_create('rbo-direct-command', os.MFD_CLOEXEC)
    manifest = {'argv': list(argv), 'cwd': str(Path(cwd).resolve()), 'stdout_fd': stdout_fd,
                'compiler_environment': compiler_environment, 'environment': environment,
                'read_only_fds': read_only,
                'fds': {name: '/proc/' + str(os.getpid()) + '/fd/' + str(fd)
                        for name, fd in fds.items()}}
    os.write(descriptor, json.dumps(manifest).encode())
    os.lseek(descriptor, 0, os.SEEK_SET)
    launch = ['systemd-run', '--user', '--scope', '--quiet', '--unit=' + unit,
              '-p', 'MemoryMax=' + str(max_scope_gib + 4) + 'G',
              sys.executable, str(Path(__file__).resolve()), '--worker',
              '/proc/' + str(os.getpid()) + '/fd/' + str(descriptor)]
    process = None
    readers = []
    stdout, stderr = {}, {}
    control_group = None
    stopped_at = None
    original = {}
    if threading.current_thread() is threading.main_thread():
        def cancel(signum, frame):
            raise InterruptedError('anonymous_guard_cancelled')
        for signum in (signal.SIGINT, signal.SIGTERM):
            original[signum] = signal.getsignal(signum)
            signal.signal(signum, cancel)
    exception = None
    try:
        process = subprocess.Popen(launch, cwd=cwd, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        for stream, result, limit in ((process.stdout, stdout, retain_stdout),
                                       (process.stderr, stderr, 0)):
            reader = threading.Thread(target=_consume, args=(stream, result, limit), daemon=True)
            readers.append(reader)
            reader.start()
        while process.poll() is None or any(reader.is_alive() for reader in readers):
            control_group = control_group or scope_control_group(unit)
            rss = group_rss(process.pid)
            scope_usage = scope_memory(control_group)
            scoped_rss = scope_rss(control_group)
            accounted = scope_usage if scope_usage is not None else scoped_rss
            available = available_memory()
            elapsed = time.monotonic() - start
            metrics['peak_group_rss_bytes'] = max(metrics['peak_group_rss_bytes'], rss)
            metrics['peak_scope_usage_bytes'] = max(metrics['peak_scope_usage_bytes'], accounted or 0)
            metrics['minimum_host_available_bytes'] = min(metrics['minimum_host_available_bytes'], available)
            metrics['scope_membership_accounting_available'] = scoped_rss is not None
            metrics['elapsed_seconds'] = round(elapsed, 3)
            missing_accounting = elapsed > 10 and scoped_rss is None
            if missing_accounting and scope_stopped(unit, control_group):
                # systemd may collect a completed scope before the parent's
                # output readers finish. Parent exit alone is not completion.
                metrics['scope_collection_after_completion'] = True
                missing_accounting = False
            if metrics_path is not None:
                Path(metrics_path).write_text(json.dumps(metrics, sort_keys=True, indent=2) + '\n')
            reason = ('host_memory_headroom' if available < min_host_gib * GIB else
                      'direct_memory_budget' if (accounted or rss) > max_scope_gib * GIB else
                      'direct_timeout' if elapsed > timeout else
                      'scope_accounting_unavailable' if missing_accounting else None)
            if stopped_at is None and reason:
                metrics.update(watchdog_stopped=True, stop_reason=reason)
                stopped_at = time.monotonic()
                stop_scope(unit, control_group, process.pid, signal.SIGTERM)
            elif stopped_at is not None and time.monotonic() - stopped_at > 2:
                stop_scope(unit, control_group, process.pid, signal.SIGKILL)
                if time.monotonic() - stopped_at > 8:
                    break
            time.sleep(0.25)
    except BaseException as exc:
        exception = exc
        metrics.update(watchdog_stopped=True, stop_reason='guard_cancelled' if isinstance(
            exc, (KeyboardInterrupt, InterruptedError)) else 'guard_failure',
            guard_exception_type=type(exc).__name__)
    finally:
        for signum in original:
            signal.signal(signum, signal.SIG_IGN)
        try:
            if process is not None:
                control_group = control_group or scope_control_group(unit)
                if process.poll() is None or not scope_stopped(unit, control_group):
                    stop_scope(unit, control_group, process.pid, signal.SIGTERM)
                for attempt in range(5):
                    if process.poll() is not None and scope_stopped(unit, control_group):
                        break
                    stop_scope(unit, control_group, process.pid, signal.SIGKILL)
                    try:
                        process.wait(timeout=0.5)
                    except subprocess.TimeoutExpired:
                        pass
                for reader in readers:
                    reader.join(1)
                metrics['cleanup_complete'] = (process.poll() is not None
                    and scope_stopped(unit, control_group) and not any(t.is_alive() for t in readers))
            else:
                metrics['cleanup_complete'] = True
        finally:
            os.close(descriptor)
            for signum, handler in original.items():
                signal.signal(signum, handler)
    metrics.update(elapsed_seconds=round(time.monotonic() - start, 3),
                   exit_code=75 if metrics['watchdog_stopped'] else process.returncode,
                   stdout_sha256=stdout.get('sha256'), stdout_bytes=stdout.get('size_bytes', 0),
                   stdout_overflow=stdout.get('overflow', False),
                   diagnostic_sha256=stderr.get('sha256'), diagnostic_bytes=stderr.get('size_bytes', 0))
    if not metrics['cleanup_complete']:
        metrics.update(exit_code=75, watchdog_stopped=True, stop_reason='incomplete_scope_cleanup')
    if exception is not None:
        # Preserve a sanitized interruption record for the caller without
        # printing an exception that could contain compiler arguments.
        metrics['cancelled'] = isinstance(exception, (KeyboardInterrupt, InterruptedError))
    if metrics_path is not None:
        Path(metrics_path).write_text(json.dumps(metrics, sort_keys=True, indent=2) + '\n')
    return metrics, stdout.get('retained', b'')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--worker':
        try:
            sys.exit(_worker(sys.argv[2]))
        except BaseException as exc:
            if isinstance(exc, SystemExit):
                raise
            # Safe class only: never show argv, generated source, or diagnostics.
            sys.stderr.write(type(exc).__name__ + '\n')
            sys.exit(70)
    raise SystemExit('This helper is invoked by anonymous_cpp_verify.py')
