"""Run one build with scheduler throttling, a cgroup and a memory watchdog."""
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
import uuid

GIB = 1024 ** 3
HERE = Path(__file__).resolve().parent


def available_memory():
    values = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
    return int(values['MemAvailable'].split()[0]) * 1024


def group_rss(group):
    total = 0
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[2]) == group:
                total += int(fields[21]) * os.sysconf('SC_PAGE_SIZE')
        except (OSError, ValueError, IndexError):
            continue
    return total


def scope_memory(control_group):
    if not control_group:
        return None
    for root, filename in (('/sys/fs/cgroup/memory', 'memory.usage_in_bytes'),
                           ('/sys/fs/cgroup', 'memory.current'),
                           ('/sys/fs/cgroup/unified', 'memory.current')):
        path = Path(root) / control_group.lstrip('/') / filename
        try:
            return int(path.read_text().strip())
        except (OSError, ValueError):
            continue
    return None


def scope_pids(control_group):
    if not control_group:
        return None
    # Hybrid cgroup-v1 servers may expose scope membership without delegating
    # the memory controller. Sum every scope member, including new process groups.
    for root in ('/sys/fs/cgroup/unified', '/sys/fs/cgroup/systemd', '/sys/fs/cgroup'):
        directory = Path(root) / control_group.lstrip('/')
        if not (directory / 'cgroup.procs').exists():
            continue
        pids = set()
        for file in directory.rglob('cgroup.procs'):
            try:
                pids.update(file.read_text().split())
            except OSError:
                continue
        return {int(pid) for pid in pids if pid.isdigit()}
    return None


def scope_rss(control_group):
    pids = scope_pids(control_group)
    if pids is None:
        return None
    total = 0
    for pid in pids:
        try:
            fields = Path('/proc', str(pid), 'stat').read_text().rsplit(')', 1)[1].split()
            total += int(fields[21]) * os.sysconf('SC_PAGE_SIZE')
        except (OSError, ValueError, IndexError):
            continue
    return total


def scope_control_group(unit):
    try:
        result = subprocess.run(['systemctl', '--user', 'show', unit + '.scope',
                                 '--property=ControlGroup', '--value'],
                                capture_output=True, text=True, timeout=2)
        return result.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def stop_scope(unit, control_group, group, signum):
    # Enumerate membership before asking systemd to stop the scope: this also
    # covers compiler children that created separate sessions/process groups.
    pids = scope_pids(control_group) or set()
    try:
        subprocess.run(['systemctl', '--user', 'kill', '--kill-who=all',
                        '--signal=' + signal.Signals(signum).name, unit + '.scope'],
                       capture_output=True, timeout=2)
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        os.killpg(group, signum)
    except (OSError, ProcessLookupError):
        pass
    # Recheck live scope membership before each PID signal, avoiding a PID
    # recycled into another service after the initial snapshot.
    for pid in pids:
        if pid == os.getpid() or pid not in (scope_pids(control_group) or set()):
            continue
        try:
            os.kill(pid, signum)
        except OSError:
            pass


def scope_stopped(unit, control_group):
    pids = scope_pids(control_group)
    if pids is not None:
        return not pids
    # systemd can remove an empty scope before the final membership check.
    # Missing accounting alone is unknown; explicit inactive/dead is proof.
    try:
        result = subprocess.run(['systemctl', '--user', 'show', unit + '.scope',
                                 '--property=ActiveState', '--property=SubState'],
                                capture_output=True, text=True, timeout=2)
    except (OSError, subprocess.SubprocessError):
        return False
    state = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    return state.get('ActiveState') in ('inactive', 'failed') and state.get('SubState') == 'dead'


def run_build(worktree, target, build_workers=None):
    unit = 'rbo-verify-' + uuid.uuid4().hex[:12]
    command = ['./ya', 'make', '--build', 'relwithdebinfo', '--locked',
               '--memory-limit=40GB', '--link-threads=1', '--test-threads=1', *target.split()]
    if build_workers is not None:
        if not isinstance(build_workers, int) or build_workers <= 0:
            raise ValueError('Build worker limit must be a positive integer')
        command.insert(2, '--threads=' + str(build_workers))
    import shlex
    shell = 'set -o pipefail\n' + shlex.join(command) + ' 2>&1 | tail -n 60'
    launch = ['systemd-run', '--user', '--scope', '--quiet', '--unit=' + unit,
              '-p', 'MemoryMax=72G', '-p', 'MemoryHigh=56G', 'bash', '-c', shell]
    start = time.monotonic()
    metrics = {'scheduler_memory_threshold': '40GB', 'requested_cgroup_memory_max_gib': 72,
               'requested_cgroup_memory_high_gib': 56, 'watchdog_min_host_available_gib': 36,
               'watchdog_max_scope_usage_gib': 52, 'peak_group_rss_bytes': 0,
               'peak_scope_usage_bytes': 0,
               'minimum_host_available_bytes': available_memory(), 'watchdog_stopped': False}
    metrics['explicit_build_workers'] = build_workers
    if available_memory() < 40 * GIB:
        metrics['watchdog_stopped'] = True
        metrics['stop_reason'] = 'insufficient_starting_memory'
        return subprocess.CompletedProcess(launch, 75, '', ''), metrics
    process = None
    reader = None
    output = []
    terminated_at = None
    control_group = None
    completed = False
    previous_sigterm = None
    previous_sigint = None
    if threading.current_thread() is threading.main_thread():
        previous_sigterm = signal.getsignal(signal.SIGTERM)
        previous_sigint = signal.getsignal(signal.SIGINT)
        def cancelled(signum, frame):
            raise InterruptedError('build_guard_cancelled')
        signal.signal(signal.SIGTERM, cancelled)
    try:
        process = subprocess.Popen(launch, cwd=worktree, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        reader = threading.Thread(target=lambda: output.append(process.communicate()), daemon=True)
        reader.start()
        while reader.is_alive():
            if control_group is None:
                control_group = scope_control_group(unit)
            rss = group_rss(process.pid)
            scope_usage = scope_memory(control_group)
            scoped_rss = scope_rss(control_group)
            accounted_memory = scope_usage if scope_usage is not None else scoped_rss
            available = available_memory()
            metrics['peak_group_rss_bytes'] = max(metrics['peak_group_rss_bytes'], rss)
            metrics['peak_scope_usage_bytes'] = max(metrics['peak_scope_usage_bytes'], accounted_memory or 0)
            metrics['scope_memory_accounting_available'] = scope_usage is not None
            metrics['scope_process_rss_available'] = scoped_rss is not None
            metrics['minimum_host_available_bytes'] = min(metrics['minimum_host_available_bytes'], available)
            if terminated_at is None and (available < 36 * GIB or (accounted_memory or rss) > 52 * GIB):
                metrics['watchdog_stopped'] = True
                metrics['stop_reason'] = 'host_memory_headroom' if available < 36 * GIB else 'build_memory_budget'
                terminated_at = time.monotonic()
                stop_scope(unit, control_group, process.pid, signal.SIGTERM)
            elif terminated_at is not None and time.monotonic() - terminated_at > 10:
                stop_scope(unit, control_group, process.pid, signal.SIGKILL)
            metrics['elapsed_seconds'] = round(time.monotonic() - start, 1)
            (HERE / 'verification' / 'active-memory.json').write_text(json.dumps(metrics, indent=2) + '\n')
            reader.join(0.5)
        completed = True
    except BaseException as exc:
        metrics['watchdog_stopped'] = True
        metrics['stop_reason'] = 'guard_cancelled' if isinstance(exc, (KeyboardInterrupt, InterruptedError)) else 'guard_failure'
        metrics['guard_exception_type'] = type(exc).__name__
        raise
    finally:
        # A detached scope outlives this Python process. Always clean it up when
        # the guard stops early, including cancellation and monitoring failures.
        # Ordinary completed builds may retain a shared ya cache service.
        try:
            if process is not None and (not completed or metrics['watchdog_stopped']):
                if previous_sigterm is not None:
                    signal.signal(signal.SIGTERM, signal.SIG_IGN)
                    signal.signal(signal.SIGINT, signal.SIG_IGN)
                metrics['watchdog_stopped'] = True
                control_group = control_group or scope_control_group(unit)
                stop_scope(unit, control_group, process.pid, signal.SIGTERM)
                if reader is not None and reader.is_alive():
                    reader.join(2)
                for attempt in range(4):
                    if (process.poll() is not None and (reader is None or not reader.is_alive())
                            and scope_stopped(unit, control_group)):
                        break
                    stop_scope(unit, control_group, process.pid, signal.SIGKILL)
                    if reader is not None and reader.is_alive():
                        reader.join(0.5)
                    try:
                        process.wait(timeout=0.5)
                    except subprocess.TimeoutExpired:
                        pass
                metrics['cleanup_complete'] = (process.poll() is not None
                    and (reader is None or not reader.is_alive()) and scope_stopped(unit, control_group))
                (HERE / 'verification' / 'active-memory.json').write_text(json.dumps(metrics, indent=2) + '\n')
        finally:
            if previous_sigterm is not None:
                signal.signal(signal.SIGTERM, previous_sigterm)
                signal.signal(signal.SIGINT, previous_sigint)
    stdout, stderr = output[0]
    code = 75 if metrics['watchdog_stopped'] else process.returncode
    return subprocess.CompletedProcess(launch, code, stdout, stderr), metrics
