"""Fake-OS/serial-guard regressions only; no native processes or payload files."""
import ast
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ORIGINAL_RUNNER = ROOT / 'proposed-quality-amendments/Q28-depth-runner-20261028/q28_depth_verify.py'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path):
    module = ModuleType(path.stem)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def only_function(path, name, globals_):
    node = next(node for node in ast.parse(path.read_bytes()).body
                if isinstance(node, ast.FunctionDef) and node.name == name)
    tree = ast.Module(body=[node], type_ignores=[])
    exec(compile(tree, str(path), 'exec'), globals_)
    return globals_[name]


class FakeOS:
    MFD_CLOEXEC, MFD_ALLOW_SEALING, SEEK_SET = 1, 2, 0

    def __init__(self, point=None, error=OSError, chunk=3):
        self.point, self.error, self.chunk = point, error, chunk
        self.calls, self.opened, self.closed, self.data = [], {}, [], bytearray()
        self.writes = 0

    def trigger(self, point):
        self.calls.append(point)
        if self.point == point:
            raise self.error()

    def memfd_create(self, name, flags):
        self.trigger('create')
        self.opened[901] = {'flags': flags}
        return 901

    def write(self, fd, value):
        assert fd in self.opened and isinstance(value, memoryview)
        self.writes += 1
        self.trigger('write_' + str(self.writes))
        if self.point == 'zero_progress':
            return 0
        count = min(self.chunk, len(value))
        self.data.extend(value[:count])
        return count

    def lseek(self, fd, offset, origin):
        assert fd in self.opened and offset == 0 and origin == self.SEEK_SET
        self.trigger('seek')
        self.opened[fd]['position'] = 0

    def fchmod(self, fd, mode):
        assert fd in self.opened and mode == 0o700
        self.trigger('mode')
        self.opened[fd]['mode'] = mode

    def close(self, fd):
        assert fd in self.opened and fd not in self.closed
        self.closed.append(fd)
        del self.opened[fd]


def main():
    checks, scenarios = [], []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)

    direct_path = HERE / 'anonymous_cpp_verify.py'
    original_direct = ROOT / direct_path.name
    for before_path, after_path, changed in ((original_direct, direct_path, '_memfd'),
            (ORIGINAL_RUNNER, HERE / 'q28_depth_verify.py', 'main')):
        old, new = ast.parse(before_path.read_bytes()), ast.parse(after_path.read_bytes())
        old.body = [node for node in old.body if not isinstance(node, ast.FunctionDef) or node.name != changed]
        new.body = [node for node in new.body if not isinstance(node, ast.FunctionDef) or node.name != changed]
        check(ast.dump(old) == ast.dump(new), 'only_intended_function_changed:' + changed)

    for executable in (False, True):
        for kind in ('bytes', 'bytearray', 'memoryview', 'empty'):
            value = b'metadata-only-payload' if kind != 'empty' else b''
            payload = bytearray(value) if kind == 'bytearray' else memoryview(value) if kind == 'memoryview' else value
            for chunk in (1, 3, 4096):
                fake = FakeOS(chunk=chunk)
                function = only_function(direct_path, '_memfd', {'os': fake})
                fd = function('metadata', payload, executable=executable)
                label = kind + ':' + str(executable) + ':' + str(chunk)
                check(bytes(fake.data) == value, 'complete_payload:' + label)
                check(fd == 901 and list(fake.opened) == [fd] and not fake.closed, 'single_returned_owner:' + label)
                check(fake.opened[fd]['flags'] == (3 if executable else 1), 'unchanged_creation_flags:' + label)
                check(fake.opened[fd]['position'] == 0, 'rewound_descriptor:' + label)
                check(('mode' in fake.opened[fd]) == executable, 'unchanged_mode_condition:' + label)
                fake.close(fd)
                scenarios.append({'scenario': 'success:' + label, 'passed': True})

    for point in ('create', 'write_1', 'write_2', 'seek', 'mode'):
        for error in (OSError, InterruptedError, KeyboardInterrupt, SystemExit):
            fake = FakeOS(point, error)
            function = only_function(direct_path, '_memfd', {'os': fake})
            try:
                function('metadata', b'metadata-only-payload', executable=True)
            except BaseException as caught:
                check(type(caught) is error, 'original_failure_preserved:' + point + ':' + error.__name__)
            else:
                check(False, 'failure_required')
            check(not fake.opened, 'post_create_owner_closed:' + point + ':' + error.__name__)
            check(fake.closed == ([] if point == 'create' else [901]), 'exactly_one_close:' + point + ':' + error.__name__)
            scenarios.append({'scenario': point + ':' + error.__name__, 'passed': True})

    for label, payload, point, error in (('zero_progress', b'neutral', 'zero_progress', OSError),
                                        ('invalid_buffer', object(), None, TypeError)):
        fake = FakeOS(point)
        function = only_function(direct_path, '_memfd', {'os': fake})
        try:
            function('metadata', payload)
        except error:
            check(not fake.opened and fake.closed == [901], 'exceptional_buffer_cleanup:' + label)
        else:
            check(False, 'exceptional_buffer_must_fail:' + label)
        scenarios.append({'scenario': label, 'passed': True})

    # Demonstrate the prior ownership defect without any real file descriptor.
    for point in ('seek', 'mode'):
        for error in (OSError, KeyboardInterrupt):
            fake = FakeOS(point, error)
            function = only_function(original_direct, '_memfd', {'os': fake})
            try:
                function('metadata', executable=True)
            except error:
                check(list(fake.opened) == [901] and not fake.closed, 'prior_defect_detected:' + point + ':' + error.__name__)
                fake.close(901)
            else:
                check(False, 'prior_failure_required')

    runner_path = HERE / 'q28_depth_verify.py'
    module = load(runner_path)
    argv = ['q28-depth', '--worktree', '/metadata-only/worktree', '--receipt', '/metadata-only/receipt',
            '--original-baseline', '/metadata-only/baseline', '--original-fixed', '/metadata-only/fixed',
            '--expected-revision', 'a' * 40]
    original_evidence = {'status': 'q28_depth_contracts_passed', 'amendment_local_contracts_verified': True,
        'full_q28_admission': False, 'query_semantics_verified': False, 'quality_improvement_verified': False,
        'cases': [{'case_index': i, 'checks_executed': n, 'passed': True}
                  for i, n in enumerate(module.EXPECTED_CHECKS)],
        'native_process': {'exit_code': 0, 'cleanup_complete': True, 'stdout_sha256': 'b' * 64}}
    for point in ('none', 'source', 'helpers', 'entry', 'execute', 'release'):
        for error in ((None,) if point == 'none' else (RuntimeError, InterruptedError, KeyboardInterrupt, SystemExit)):
            calls = []
            def trigger(where):
                calls.append(where)
                if where == point:
                    raise error()
            class Slot:
                def __enter__(self):
                    trigger('entry')
                def __exit__(self, *_):
                    trigger('release')
            direct = SimpleNamespace(serial_slot=Slot)
            def sources():
                trigger('source')
                return {}
            def helpers(_):
                trigger('helpers')
                return direct, None, None, None
            def execute(*_):
                trigger('execute')
                return copy.deepcopy(original_evidence), 0
            with tempfile.TemporaryDirectory(prefix='q28-main-metadata-') as temp:
                fixture = Path(temp)
                (fixture / 'verification').mkdir()
                with patch.object(module, 'ROOT', fixture), patch.object(module, '_sources', sources), \
                        patch.object(module, '_helpers', helpers), patch.object(module, 'execute', execute), \
                        patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
                    code = module.main()
                outputs = list((fixture / 'verification').glob('Q28-depth-fixed-*.json'))
                check(len(outputs) == 1, 'single_exclusive_metadata_record:' + point + ':' + str(error))
                evidence = json.loads(outputs[0].read_bytes())
            label = point + ':' + ('none' if error is None else error.__name__)
            if point == 'none':
                check(code == 0 and evidence == original_evidence, 'unchanged_success_path')
            else:
                check(code == (130 if error in (KeyboardInterrupt, SystemExit) else 1), 'nonzero_failure_exit:' + label)
                check(evidence['status'] == 'q28_depth_harness_failed'
                      and evidence['amendment_local_contracts_verified'] is False, 'failure_revokes_local_admission:' + label)
                check(evidence['failure_class'] == error.__name__, 'failure_class_preserved:' + label)
                if point == 'release':
                    check(evidence['cases'] == original_evidence['cases']
                          and evidence['native_process'] == original_evidence['native_process'],
                          'completed_cases_and_process_evidence_retained:' + label)
                else:
                    check('cases' not in evidence, 'no_uncompleted_cases_fabricated:' + label)
            check(all(evidence[key] is False for key in ('full_q28_admission', 'query_semantics_verified',
                  'quality_improvement_verified')), 'all_broader_claims_stay_false:' + label)
            scenarios.append({'scenario': 'main:' + label, 'passed': True})

    # Rerun the frozen execute/parser/provenance regressions on the patched runner.
    prior = load(ORIGINAL_RUNNER.parent / 'verify_metadata.py')
    prior.DRIVER = runner_path
    old_report = ROOT / 'verification/Q28-depth-fixed-runner-metadata-checks-20261028.json'
    saved_report = old_report.read_bytes()
    intercepted = []
    write_text = Path.write_text
    def capture_report(path, text, *args, **kwargs):
        if path == old_report:
            intercepted.append(json.loads(text))
            return len(text)
        return write_text(path, text, *args, **kwargs)
    with patch.object(Path, 'write_text', capture_report), contextlib.redirect_stdout(io.StringIO()):
        prior.main()
    check(old_report.read_bytes() == saved_report, 'frozen_original_mock_report_not_rewritten')
    check(len(intercepted) == 1 and intercepted[0]['checks_passed'] == 255
          and intercepted[0]['scenario_count'] == 52, 'all_prior_255_checks_52_scenarios_pass')
    report = {'schema': 'q28_cleanup_followups_metadata_v1', 'native_work_executed': False,
        'checks_passed': len(checks), 'checks': checks, 'scenario_count': len(scenarios), 'scenarios': scenarios,
        'prior_runner_regressions': {'checks_passed': 255, 'scenario_count': 52, 'original_report_preserved': True},
        'sources': {path.name: sha(path.read_bytes()) for path in (direct_path, runner_path)},
        'prior_failure_scope': 'Verification infrastructure only, not a new production RBO finding.',
        'limits': ['Abrupt process death cannot guarantee final metadata persistence.',
                   'The memfd repair covers post-create initialization, not unrelated caller allocation patterns.',
                   'No compiler, linker, native subprocess, actual descriptor or workload artifact used.']}
    target = ROOT / 'verification/Q28-cleanup-followups-metadata-checks-20261028.json'
    target.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'checks_passed': len(checks), 'scenarios': len(scenarios),
                      'prior_checks': 255, 'report_sha256': sha(target.read_bytes())}))


if __name__ == '__main__':
    main()
