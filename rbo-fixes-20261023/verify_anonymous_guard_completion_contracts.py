"""Mock scope collection while output drains; never launch a real process."""
import hashlib
import io
import json
from pathlib import Path
from unittest.mock import patch

import anonymous_cpp_guard as guard

ROOT = Path(__file__).resolve().parent


def scenario(*, parent_exited=True, scope_complete=True, elapsed=12, pressure=False,
             accounted=None, probe_raises=False, timeout=600):
    class Clock:
        now = 0
        def monotonic(self): return self.now
        def sleep(self, duration): self.now += duration
    clock = Clock()
    stopped = []
    class Process:
        pid = 987654321
        returncode = None
        def __init__(self):
            self.stdout, self.stderr = io.BytesIO(b'{"count":32}\n'), io.BytesIO(b'')
        def poll(self):
            if parent_exited or clock.now >= elapsed + 1 or stopped:
                self.returncode = 0
            return self.returncode
        def wait(self, timeout=None): return self.poll()
    process = Process()
    class Reader:
        def __init__(self, target, args, daemon): self.target, self.args = target, args
        def start(self): self.target(*self.args)
        def is_alive(self): return clock.now < elapsed + 1 and not stopped
        def join(self, timeout=None): pass
    def spawn(*args, **kwargs):
        clock.now = elapsed
        return process
    completion_calls = 0
    def completed(*args):
        nonlocal completion_calls
        completion_calls += 1
        if probe_raises and completion_calls == 1:
            raise OSError('unavailable scope state')
        if probe_raises and not stopped:
            return False
        return scope_complete or bool(stopped) or clock.now >= elapsed + 1
    def stop(*args):
        stopped.append(args[-1])
        process.returncode = 0
    def available():
        return (32 if pressure and clock.now else 80) * guard.GIB
    with patch.object(guard.time, 'monotonic', side_effect=clock.monotonic), \
         patch.object(guard.time, 'sleep', side_effect=clock.sleep), \
         patch.object(guard.threading, 'Thread', Reader), \
         patch.object(guard.subprocess, 'Popen', side_effect=spawn), \
         patch.object(guard, 'available_memory', side_effect=available), \
         patch.object(guard, 'scope_control_group', return_value='/mock/completed'), \
         patch.object(guard, 'group_rss', return_value=0), \
         patch.object(guard, 'scope_memory', return_value=None), \
         patch.object(guard, 'scope_rss', return_value=accounted), \
         patch.object(guard, 'scope_stopped', side_effect=completed) as probes, \
         patch.object(guard, 'stop_scope', side_effect=stop):
        record, output = guard.run_anonymous(['/not-executed'], '/tmp', {}, timeout=timeout)
    return record, output, stopped, probes.call_count


def main():
    assertions = 0
    def check(value):
        nonlocal assertions
        assert value
        assertions += 1
    for parent_exited in (True, False):
        record, output, stopped, probes = scenario(parent_exited=parent_exited)
        check(record['exit_code'] == 0 and record['cleanup_complete'] and not record['watchdog_stopped'])
        check(record['scope_collection_after_completion'] is True and probes > 0)
        check(not stopped and output == b'{"count":32}\n')
    for parent_exited in (True, False):
        record, _, stopped, _ = scenario(parent_exited=parent_exited, scope_complete=False)
        check(record['exit_code'] == 75 and record['watchdog_stopped'] and record['cleanup_complete'])
        check(record['stop_reason'] == 'scope_accounting_unavailable' and bool(stopped))
    record, _, stopped, _ = scenario(probe_raises=True)
    check(record['exit_code'] == 75 and record['stop_reason'] == 'guard_failure' and bool(stopped))
    record, _, stopped, _ = scenario(scope_complete=False, elapsed=8)
    check(record['exit_code'] == 0 and record['cleanup_complete'] and not stopped)
    check(record['scope_collection_after_completion'] is False)
    record, _, stopped, _ = scenario(scope_complete=False, accounted=0)
    check(record['exit_code'] == 0 and not stopped and record['scope_collection_after_completion'] is False)
    for options, reason in (({'pressure': True}, 'host_memory_headroom'), ({'timeout': 11}, 'direct_timeout'),
                            ({'accounted': 25 * guard.GIB}, 'direct_memory_budget')):
        record, _, stopped, _ = scenario(**options)
        check(record['exit_code'] == 75 and record['stop_reason'] == reason and bool(stopped))
    result = dict(mock_contracts_passed=True, assertions=assertions,
                  guard_sha256=hashlib.sha256(Path(guard.__file__).read_bytes()).hexdigest(),
                  actual_processes_started=0, native_runs=0, generated_inputs_saved=False,
                  coverage=['completed scope after parent exit while readers drain',
                            'completed scope before launcher exit while readers drain',
                            'live or unknown scope despite exited parent',
                            'unknown scope with live parent', 'completion probe failure',
                            'startup grace', 'available zero membership accounting',
                            'host pressure, timeout and scope budget remain enforced'])
    (ROOT/'verification/anonymous-guard-completion-contracts.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,sort_keys=True))


if __name__ == '__main__':
    main()
