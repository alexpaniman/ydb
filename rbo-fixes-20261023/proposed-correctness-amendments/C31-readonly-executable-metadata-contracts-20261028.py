"""Only source patches and mocked descriptor ownership; no native process."""
import ast
import contextlib
import errno
import fcntl
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[1]
D = ROOT / 'proposed-correctness-amendments'
sys.path.insert(0, str(ROOT))
COUNT = 0

def check(value, label):
    global COUNT
    if not value:
        raise AssertionError(label)
    COUNT += 1

def load(name, file):
    module = types.ModuleType(name)
    module.__file__ = str(ROOT / (name + '.py'))
    exec(compile(file.read_bytes(), module.__file__, 'exec'), module.__dict__)
    return module

guard = load('anonymous_cpp_guard', D / 'anonymous_cpp_guard_readonly_exec_20261028.candidate.py')
with patch.dict(sys.modules, {'anonymous_cpp_guard': guard}):
    direct = load('anonymous_cpp_verify', D / 'anonymous_cpp_verify_readonly_exec_20261028.candidate.py')

class FDs:
    def __init__(self, failure=None):
        self.live = {10: {'flags': os.O_RDWR, 'seals': 0, 'inode': 77, 'memfd': True}}
        self.next = 20
        self.closed = []
        self.events = []
        self.failure = failure
    def event(self, tag):
        self.events.append(tag)
        if self.failure == tag:
            raise OSError(errno.EIO, 'mock descriptor operation')
    def chmod(self, fd, mode):
        self.event('chmod')
        check(fd in self.live and mode == 0o700, 'executable permission remains owner-only')
    def control(self, fd, op, value=None):
        state = self.live[fd]
        if op == fcntl.F_GETFL:
            self.event('read_flags' if state['flags'] == os.O_RDONLY else 'write_flags')
            return state['flags']
        if op == fcntl.F_ADD_SEALS:
            self.event('add_seals')
            check(value == guard.NATIVE_EXECUTABLE_SEALS, 'exact four executable seals')
            for entry in self.live.values():
                if entry['inode'] == state['inode']:
                    entry['seals'] = value
            return 0
        if op == fcntl.F_GET_SEALS:
            self.event('get_seals')
            return state['seals']
        raise AssertionError('unexpected descriptor operation')
    def link(self, path):
        self.event('readlink')
        state = self.live[int(path.rsplit('/', 1)[1])]
        return '/memfd:neutral-contract (deleted)' if state['memfd'] else '/not-memory'
    def open(self, path, flags):
        self.event('open')
        old = self.live[int(path.rsplit('/', 1)[1])]
        new = self.next
        self.next += 1
        self.live[new] = dict(old, flags=flags & os.O_ACCMODE)
        return new
    def stat(self, fd):
        self.event('stat_read' if self.live[fd]['flags'] == os.O_RDONLY else 'stat_write')
        return types.SimpleNamespace(st_dev=1, st_ino=self.live[fd]['inode'], st_size=123)
    def close(self, fd):
        check(fd in self.live, 'each descriptor closed exactly once')
        del self.live[fd]
        self.closed.append(fd)
    @contextlib.contextmanager
    def mock(self):
        with patch.object(os, 'fchmod', side_effect=self.chmod), patch.object(os, 'open', side_effect=self.open), patch.object(os, 'readlink', side_effect=self.link), patch.object(os, 'fstat', side_effect=self.stat), patch.object(os, 'close', side_effect=self.close), patch.object(fcntl, 'fcntl', side_effect=self.control):
            yield

for failure in (None, 'write_flags', 'readlink', 'chmod', 'add_seals', 'open', 'stat_write', 'stat_read', 'read_flags', 'get_seals'):
    state = FDs(failure)
    owners = {'executable': 10}
    with state.mock(), patch.object(direct.signal, 'pthread_sigmask', return_value=set()) as masks:
        try:
            evidence = direct._seal_native_executable(owners)
        except OSError:
            check(failure is not None, 'only injected interruption fails')
            check(owners['executable'] == 10 and set(state.live) == {10}, 'reopen failure leaves original caller owner only')
        else:
            check(failure is None, 'all descriptor failures are propagated')
            check(owners['executable'] != 10 and 10 in state.closed, 'writable owner replaced and closed')
            check(set(state.live) == {owners['executable']}, 'only readonly owner remains')
            check(state.live[owners['executable']]['flags'] == os.O_RDONLY, 'caller executable descriptor readonly')
            guard._check_native_executable(owners['executable'])
            check(evidence == {'version': 'sealed_read_only_memfd_v1', 'same_inode': True, 'write_grow_shrink_and_seal_seals': True, 'writable_owner_closed': True}, 'sanitized exact transport evidence')
        check(masks.call_args_list[0].args == (direct.signal.SIG_BLOCK, {direct.signal.SIGINT, direct.signal.SIGTERM}), 'handoff blocks cancellation signals')
        check(masks.call_args_list[-1].args == (direct.signal.SIG_SETMASK, set()), 'handoff restores prior signal mask')
        state.close(owners['executable'])
    check(not state.live, 'caller finally closes all executable descriptors')

# Same-inode identity and immutable data are checked before losing the old owner.
for mutation in ('inode', 'access', 'seals', 'not_memfd'):
    state = FDs()
    owners = {'executable': 10}
    original_open = state.open
    def malformed_open(path, flags):
        fd = original_open(path, flags)
        if mutation == 'inode': state.live[fd]['inode'] = 78
        if mutation == 'access': state.live[fd]['flags'] = os.O_RDWR
        if mutation == 'seals': state.live[fd]['seals'] = 0
        return fd
    state.open = malformed_open
    if mutation == 'not_memfd': state.live[10]['memfd'] = False
    with state.mock():
        try:
            direct._seal_native_executable(owners)
        except direct.MetadataError:
            check(owners == {'executable': 10} and set(state.live) == {10}, 'identity rejection closes only temporary readonly fd')
        else:
            raise AssertionError('invalid executable transport accepted')
        state.close(owners['executable'])

# Worker is exercised with fake manifests/FDs and a mocked subprocess.run.
for variant in ('native_ok', 'native_signal', 'compiler_ok', 'native_writable', 'native_unsealed', 'native_compiler_flag', 'bad_readonly_name', 'duplicate_readonly', 'readonly_stdout', 'non_memfd', 'spawn_failure'):
    state = FDs()
    state.live[10].update(flags=os.O_RDONLY, seals=guard.NATIVE_EXECUTABLE_SEALS)
    state.live[11] = {'flags': os.O_RDWR, 'seals': 0, 'inode': 88, 'memfd': True}
    spec = {'argv': ['@FD:executable@'], 'cwd': '/tmp', 'fds': {'executable': '/proc/1/fd/10', 'report': '/proc/1/fd/11'}, 'read_only_fds': ['executable'], 'compiler_environment': False, 'environment': {}}
    if variant == 'compiler_ok': spec.update(argv=['/compiler/not-executed', '@FD:report@'], compiler_environment=True, stdout_fd='report')
    if variant == 'native_writable': spec['read_only_fds'] = []
    if variant == 'native_unsealed': state.live[10]['seals'] = 0
    if variant == 'native_compiler_flag': spec['compiler_environment'] = True
    if variant == 'bad_readonly_name': spec['read_only_fds'] = ['unknown']
    if variant == 'duplicate_readonly': spec['read_only_fds'] *= 2
    if variant == 'readonly_stdout': spec['stdout_fd'] = 'executable'
    if variant == 'non_memfd': state.live[11]['memfd'] = False
    invoked = []
    def spawn(command, **kwargs):
        invoked.append(True)
        check(set(kwargs['pass_fds']) == set(state.live) - {10, 11}, 'worker passes only owned reopened descriptors')
        if variant == 'compiler_ok':
            check(kwargs['stdout'] is not None, 'compiler output remains writable')
            check(state.live[kwargs['stdout']]['flags'] == os.O_RDWR, 'compiler output descriptor mode preserved')
            check(kwargs['env']['TMPDIR'] == '/proc/self/fd/anonymous-no-temp-directory', 'compiler output guard unchanged')
        else:
            executable = int(command[0].rsplit('/', 1)[1])
            check(all(x['flags'] == os.O_RDONLY for x in state.live.values() if x['inode'] == 77), 'no writable executable descriptor at spawn')
            guard._check_native_executable(executable)
            if variant == 'native_compiler_flag':
                check(kwargs['env']['TMPDIR'] == '/proc/self/fd/anonymous-no-temp-directory', 'direct native compiler-style sanitization preserved')
        if variant == 'spawn_failure': raise OSError(errno.EIO, 'mock spawn failure')
        return types.SimpleNamespace(returncode=-15 if variant == 'native_signal' else 0)
    with state.mock(), patch.object(pathlib.Path, 'read_bytes', return_value=json.dumps(spec).encode()), patch.object(guard.resource, 'setrlimit'), patch.object(guard.subprocess, 'run', side_effect=spawn):
        try:
            result = guard._worker('/manifest/not-persisted')
        except (guard.AnonymousGuardError, OSError):
            check(variant not in ('native_ok', 'native_signal', 'compiler_ok', 'native_compiler_flag'), 'only invalid worker requests rejected')
        else:
            check(variant in ('native_ok', 'native_signal', 'compiler_ok', 'native_compiler_flag'), 'valid worker request executes')
            check(result == (143 if variant == 'native_signal' else 0), 'exit and signal transport preserved')
        check(set(state.live) == {10, 11}, 'worker always closes every reopened descriptor')
    check(bool(invoked) == (variant in ('native_ok', 'native_signal', 'compiler_ok', 'native_compiler_flag', 'spawn_failure')), 'invalid worker never starts target')

# Invalid parent transport must fail before memory guard or scope launch.
for flags, seals in ((os.O_RDWR, guard.NATIVE_EXECUTABLE_SEALS), (os.O_RDONLY, 0), (os.O_RDONLY, fcntl.F_SEAL_WRITE)):
    state = FDs()
    state.live[10].update(flags=flags, seals=seals)
    with state.mock(), patch.object(guard.subprocess, 'Popen', side_effect=AssertionError('scope unexpectedly launched')):
        try:
            guard.run_anonymous(['@FD:executable@'], '/tmp', {'executable': 10}, compiler_environment=False)
        except guard.AnonymousGuardError:
            check(True, 'parent rejects writable or unsealed native executable')
        else:
            raise AssertionError('invalid parent executable accepted')

# Actual direct.execute boundary, stopped before any native scope launch.
# Preserve its default compiler_environment=True policy independently of FD mode.
state = FDs()
receipt = {'metadata': {'profile': 'rbo', 'compile_argv': ['/fake-compiler'],
    'compile_cwd': '/tmp', 'compile_environment': {}, 'link_cwd': '/tmp', 'link_environment': {}},
    'files': {}}
raw_receipt = json.dumps(receipt).encode()
class Receipt:
    def resolve(self): return self
    def read_bytes(self): return raw_receipt
    def __str__(self): return '/metadata-only/receipt'
args = types.SimpleNamespace(bug='C31', phase='fixed', worktree=pathlib.Path('/tmp'),
    receipt=Receipt(), compile_timeout=1, link_timeout=1, native_timeout=1)
next_fd = 30
calls = []
def allocate(name, data=b'', executable=False):
    global next_fd
    if executable: return 10
    fd = next_fd
    next_fd += 1
    state.live[fd] = {'flags': os.O_RDWR, 'seals': 0, 'inode': fd + 100, 'memfd': True}
    return fd

def run_boundary(argv, cwd, fds, **kwargs):
    calls.append((argv[0], dict(kwargs)))
    if argv[0] != '@FD:executable@':
        return {'exit_code': 0, 'environment_sha256': direct._json_digest({})}, b''
    check('compiler_environment' not in kwargs, 'actual execute preserves default native environment policy')
    check(kwargs['max_scope_gib'] == 12, 'actual direct native memory cap unchanged')
    result, output = guard.run_anonymous(argv, cwd, fds, **kwargs)
    check(result['native_executable_transport'] == 'sealed_read_only_memfd_v1', 'real guard accepts actual default-policy boundary')
    check(result['stop_reason'] == 'insufficient_starting_memory' and result['watchdog_stopped'], 'low-memory mock prevents any target launch')
    return result, output

version = types.ModuleType('production_version_object')
version.prepare_object = lambda *a, **kw: (['/fake-linker'], {})
version.verify_object = lambda *a, **kw: None
contract = types.ModuleType('correctness_cpp_contract')
contract.profile_digest = lambda *a, **kw: 'f' * 64
with state.mock(), patch.dict(sys.modules, {'production_version_object': version, 'correctness_cpp_contract': contract}), patch.object(direct, '_expected_revision', return_value='f' * 40), patch.object(direct, 'verify_receipt'), patch.object(direct, 'render_suite', return_value=(b'neutral metadata', 1, 'f' * 64)), patch.object(direct, '_memfd', side_effect=allocate), patch.object(direct, '_fd_digest', return_value={'sha256': 'f' * 64, 'size_bytes': 123}), patch.object(direct, '_stdout_link_arguments', side_effect=lambda a: (a, {})), patch.object(direct, '_archive_response_arguments', side_effect=lambda a, *rest: (a, {'responses': []})), patch.object(direct, 'run_anonymous', side_effect=run_boundary), patch.object(direct.signal, 'pthread_sigmask', return_value=set()), patch.object(guard, 'available_memory', return_value=0), patch.object(guard.subprocess, 'Popen', side_effect=AssertionError('native scope unexpectedly launched')):
    evidence, status = direct.execute(args)
    check(status == 1 and evidence['status'] == 'anonymous_native_guard_failed', 'no native verdict accepted by boundary metadata test')
    check('cases' not in evidence, 'no direct case results fabricated')
    check(evidence['native_executable_transport']['writable_owner_closed'], 'execute records completed ownership handoff')
check(len(calls) == 3 and not state.live, 'actual execute closes all descriptors after guard failure')

with tempfile.TemporaryDirectory(prefix='readonly-source-') as tmp:
    for name in ('anonymous_cpp_guard.py', 'anonymous_cpp_verify.py'):
        (pathlib.Path(tmp) / name).write_bytes((ROOT / name).read_bytes())
    result = subprocess.run(['git', 'apply', str(D / 'C31-sealed-readonly-native-executable-20261028.patch')], cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(result.returncode == 0, 'clean two-file source patch application')
    for original, candidate in [('anonymous_cpp_guard.py', 'anonymous_cpp_guard_readonly_exec_20261028.candidate.py'), ('anonymous_cpp_verify.py', 'anonymous_cpp_verify_readonly_exec_20261028.candidate.py')]:
        check((pathlib.Path(tmp) / original).read_bytes() == (D / candidate).read_bytes(), 'exact source after image')

# Reuse existing scope-cancellation tests with the candidate guard, mocked OS processes only.
with patch.dict(sys.modules, {'anonymous_cpp_guard': guard}):
    regressions = load('verify_anonymous_guard_completion_contracts', ROOT / 'verify_anonymous_guard_completion_contracts.py')
    with patch('builtins.print'):
        regressions.main()
check(True, 'existing scope cleanup and pressure mock regressions pass')

report = {'kind': 'metadata_and_mocked_os_only', 'assertions': COUNT, 'descriptor_scenarios': 28,
          'actual_execute_guard_boundary': True,
          'existing_scope_regressions_pass': True, 'native_execution': False,
          'build_toolchain_execution': False, 'live_edits': False,
          'payload_persistence': False, 'fixed_native_contract_acceptance': False}
p = ROOT / 'verification/C31-readonly-executable-metadata-contracts-20261028.json'
p.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'metadata_pass': True, 'assertions': COUNT, 'report_sha256': hashlib.sha256(p.read_bytes()).hexdigest()}))
