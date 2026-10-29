"""Exercise the actual reviewed handoff helper inside Q28's mocked call path."""
import ast
import contextlib
import hashlib
import io
import json
from pathlib import Path
from types import ModuleType
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
DORMANT = ROOT / 'proposed-quality-amendments'
sha = lambda value: hashlib.sha256(value).hexdigest()
preparation_path = ROOT / 'verification/Q28-sdk-readonly-executable-source-preparation-20261028.json'
assert sha(preparation_path.read_bytes()) == '270562627bab578feb938638b63d7c6490689e559bc1bdab7ce226b3ed23130e'
preparation = json.loads(preparation_path.read_bytes())
author_path = ROOT / preparation['validation']['metadata_script']
assert sha(author_path.read_bytes()) == preparation['validation']['metadata_script_sha256']
author = author_path.read_text()
candidate = (DORMANT / 'q28_compilation_measurement_readonly_executable_20261028.candidate.py').read_bytes()
before = (DORMANT / 'q28_compilation_measurement_version_replay_20261028.candidate.py').read_bytes()
assert sha(candidate) == preparation['module_after_sha256']
assert sha(before) == preparation['module_before_sha256']

# The candidate changes exactly the reviewed call/evidence addition and removes
# the obsolete chmod. All generated input, acceptance and runtime code is equal.
expected = before.replace(
    b"        evidence['generated_executable'] = direct._fd_digest(fds['executable'])\n",
    b"        evidence['native_executable_transport'] = direct._seal_native_executable(fds)\n"
    b"        evidence['generated_executable'] = direct._fd_digest(fds['executable'])\n")
expected = expected.replace(b"        os.fchmod(fds['executable'], 0o700)\n", b'')
expected = expected.replace(
    b"'runtime_identity', 'production_version_infrastructure') if key in evidence}",
    b"'runtime_identity', 'production_version_infrastructure', 'native_executable_transport') if key in evidence}")
assert candidate == expected
tree = ast.parse(candidate)
function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'execute_measurements')
calls = [node for node in ast.walk(function) if isinstance(node, ast.Call)]
seals = [node for node in calls if isinstance(node.func, ast.Attribute) and node.func.attr == '_seal_native_executable']
assert len(seals) == 1 and isinstance(seals[0].args[0], ast.Name) and seals[0].args[0].id == 'fds'

marker = "        direct = importlib.import_module('anonymous_cpp_verify')\n"
assert author.count(marker) == 1
author = author.replace(marker, marker + '        actual_sealer = direct._seal_native_executable\n')
author = author.replace('import ast\n', 'import ast\nimport fcntl\nimport signal\n')
start = author.index('            def seal(fds):\n')
end = author.index('            def prepare(', start)
author = author[:start] + '''            def seal(fds):
                events.append('seal')
                check(events[-2] == 'version_verify' and 'link' in events)
                check(not handoff)
                if scenario == 'seal_failed':
                    raise metadata.MetadataError('mock seal failure')
                if scenario == 'seal_cancelled':
                    raise KeyboardInterrupt()
                writable = fds['executable']
                state = {'sealed': False, 'readonly': None, 'masked': False}
                def fcntl_call(fd, command, *arguments):
                    check(fd in buffers)
                    if command == fcntl.F_GETFL:
                        return os.O_RDWR if fd == writable else os.O_RDONLY
                    if command == fcntl.F_ADD_SEALS:
                        check(fd == writable and arguments == (direct.NATIVE_EXECUTABLE_SEALS,))
                        state['sealed'] = True
                        return 0
                    if command == fcntl.F_GET_SEALS:
                        return 0 if scenario == 'readonly_seals_mismatch' else direct.NATIVE_EXECUTABLE_SEALS
                    raise AssertionError('unexpected fake fcntl command')
                def reopen(path, flags):
                    check(state['masked'] and state['sealed'])
                    check(path == '/proc/self/fd/' + str(writable))
                    check(flags == os.O_RDONLY | os.O_CLOEXEC)
                    if scenario == 'readonly_open_failed':
                        raise OSError('fake read-only open failure')
                    readonly = memory('readonly', buffers[writable])
                    state['readonly'] = readonly
                    return readonly
                def stat(fd):
                    check(fd in buffers)
                    if fd == state['readonly'] and scenario == 'readonly_validation_cancelled':
                        raise KeyboardInterrupt()
                    inode = 11 if fd == state['readonly'] and scenario == 'readonly_identity_mismatch' else 10
                    return types.SimpleNamespace(st_dev=4, st_ino=inode, st_size=len(buffers[fd]))
                def mask(how, value):
                    if how == signal.SIG_BLOCK:
                        check(value == {signal.SIGINT, signal.SIGTERM})
                        state['masked'] = True
                        return set()
                    check(how == signal.SIG_SETMASK and value == set())
                    state['masked'] = False
                    if scenario == 'pending_handoff_cancelled':
                        check(fds['executable'] == state['readonly'] and writable not in buffers)
                        raise KeyboardInterrupt()
                    return {signal.SIGINT, signal.SIGTERM}
                with ExitStack() as inner:
                    inner.enter_context(patch.object(direct.fcntl, 'fcntl', fcntl_call))
                    inner.enter_context(patch.object(direct.os, 'open', reopen))
                    inner.enter_context(patch.object(direct.os, 'fstat', stat))
                    inner.enter_context(patch.object(direct.os, 'readlink', lambda path: '/memfd:fake-executable'))
                    inner.enter_context(patch.object(direct.signal, 'pthread_sigmask', mask))
                    result = actual_sealer(fds)
                check(not state['masked'])
                check(fds['executable'] == state['readonly'] and writable not in buffers)
                check(buffers[fds['executable']] == b'linked metadata executable')
                handoff.update(writable=writable, readonly=state['readonly'])
                if scenario == 'handoff_cancelled':
                    raise KeyboardInterrupt()
                return result
''' + author[end:]
needle = "'readonly_digest_changed'] + ['allocation_'"
assert author.count(needle) == 1
author = author.replace(needle, "'readonly_digest_changed', 'readonly_open_failed', 'readonly_identity_mismatch', "
                       "'readonly_seals_mismatch', 'readonly_validation_cancelled', 'pending_handoff_cancelled'] + ['allocation_'")

metadata_path = ROOT / preparation['validation']['metadata_report']
metadata_before = metadata_path.read_bytes()
captured = []
original_write = Path.write_text


def capture_report(path, value, *args, **kwargs):
    if path == metadata_path:
        captured.append(value.encode())
        return len(value)
    return original_write(path, value, *args, **kwargs)


module = ModuleType('q28_actual_handoff_independent')
module.__file__ = str(author_path)
exec(compile(author, str(author_path), 'exec'), module.__dict__)
with patch.object(Path, 'write_text', capture_report), contextlib.redirect_stdout(io.StringIO()):
    module.main()
assert len(captured) == 1 and metadata_path.read_bytes() == metadata_before
result = json.loads(captured[0])
assert len(result['scenarios']) == 34
assert result['native_runs'] == result['build_runs'] == result['toolchain_runs'] == 0
report = {
    'status': 'independently_source_and_metadata_approved_native_pending',
    'patch_sha256': preparation['patch_sha256'],
    'source_preparation_sha256': sha(preparation_path.read_bytes()),
    'module_before_sha256': preparation['module_before_sha256'],
    'module_after_sha256': preparation['module_after_sha256'],
    'integrated_actual_helper_mock_assertions': result['assertions'],
    'mock_scenarios': len(result['scenarios']),
    'scenarios': result['scenarios'],
    'combined_dispatcher_sha256': result['combined_dispatcher_sha256'],
    'source_conclusions': [
        'Exactly one reviewed helper call occurs after successful link/version verification, before the immutable executable digest and before sample descriptor borrowing.',
        'Actual helper runs under fake OS operations inside the complete Q28 execution path; it seals, compares device/inode/size, reopens read-only and closes the writable owner.',
        'All samples borrow the replaced owner. Sample cleanup never closes it; outer cleanup closes it exactly once and retains all other descriptor ownership.',
        'Open/identity/seal rejection, validation cancellation, deferred cancellation after handoff, existing prepare/link/native cancellation and allocation failures leave no fake descriptor unowned.',
        'Complete or failed measurement evidence retains sanitized handoff metadata; final executable hash mutation is rejected.',
        'Generated C++, seven cold samples, runtime attestation, version-object provenance and cache checks remain unchanged.',
        'Direct26 and its acceptance remain separate; shared spilling stays pending; all semantic, optimizer-quality, full-pilot and stage-timing admission flags remain false.'
    ],
    'native_executed': False, 'build_or_toolchain_executed': False,
    'live_helpers_changed': False, 'rendered_inputs_or_graphs_or_cpp_persisted': False,
    'review_script_sha256': sha(Path(__file__).read_bytes()),
    'remaining': 'Actual Q28 link, SDK samples and native environment attestation remain pending; this approval does not establish performance improvement.'
}
output = ROOT / 'verification/Q28-sdk-readonly-executable-independent-review-20261028.json'
output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'assertions': result['assertions'], 'scenarios': len(result['scenarios']),
                  'report_sha256': sha(output.read_bytes()), 'native_executed': False}))
