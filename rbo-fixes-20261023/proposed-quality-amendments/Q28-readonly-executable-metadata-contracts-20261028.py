"""Q28 read-only executable integration checks; all target work mocked."""
import ast
from contextlib import ExitStack
import hashlib
import importlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
COUNT = 0


def check(value):
    global COUNT
    if value is not True:
        raise AssertionError('Q28 executable handoff check ordinal ' + str(COUNT + 1))
    COUNT += 1


def digest(value):
    return hashlib.sha256(value).hexdigest()


def main():
    prep_path = ROOT / 'verification/Q28-sdk-readonly-executable-source-preparation-20261028.json'
    preparation = json.loads(prep_path.read_bytes())
    amendments = [*preparation['depends_on'][:3],
                  dict(patch=preparation['patch'], sha256=preparation['patch_sha256'])]
    for item in [preparation['depends_on'][3], *amendments]:
        check(digest((ROOT / item['patch']).read_bytes()) == item['sha256'])
    live_bindings = {name: digest((ROOT / name).read_bytes()) for name in
                     ('anonymous_cpp_verify.py', 'anonymous_cpp_guard.py', 'correctness_cpp_contract.py')}
    check(live_bindings['anonymous_cpp_verify.py'] == '4b4636dd8a448c279257b698d4c32e8b88cdd0f7956d6769a15f19e99b0f7873')
    check(live_bindings['anonymous_cpp_guard.py'] == '58941801bad6b72f86546c669107e51cf2b765a806b1d8bb999ac4dfbf310819')
    with tempfile.TemporaryDirectory(prefix='q28-replay-independent-') as directory:
        base = Path(directory)
        local = base / ROOT.name
        local.mkdir()
        (base / 'rbo-review-20261021').symlink_to(ROOT.parent / 'rbo-review-20261021', target_is_directory=True)
        for path in ROOT.glob('*.py'):
            shutil.copyfile(path, local / path.name)
        for item in amendments:
            completed = subprocess.run(['git', 'apply', str((ROOT / item['patch']).resolve())], cwd=base,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            check(completed.returncode == 0)
        check(digest((local / 'q28_compilation_measurement.py').read_bytes()) == preparation['module_after_sha256'])
        check(digest((local / 'production_version_object.py').read_bytes()) == preparation['reviewed_version_helper_sha256'])
        sys.path[:0] = [str(local), str(base / 'rbo-review-20261021')]
        module = importlib.import_module('q28_compilation_measurement')
        direct = importlib.import_module('anonymous_cpp_verify')
        metadata = importlib.import_module('anonymous_cpp_metadata')
        pairing = importlib.import_module('quality_pairing')
        version = importlib.import_module('production_version_object')
        check(direct.GENERATORS['Q28'] == 'quality_scheduler_cpp_checks')
        check(len(importlib.import_module(direct.GENERATORS['Q28']).cases()) == 26)
        before = (ROOT / 'proposed-quality-amendments/q28_compilation_measurement_version_replay_20261028.candidate.py').read_bytes()
        after = (local / 'q28_compilation_measurement.py').read_bytes()
        functions = lambda value: {node.name: ast.dump(node) for node in ast.parse(value).body
                                  if isinstance(node, ast.FunctionDef) and node.name != 'execute_measurements'}
        check(functions(before) == functions(after))
        source_hash = digest(module.render_cpp().encode())
        check(source_hash == preparation['generated_cpp_sha256'])
        fingerprint = pairing.profile_fingerprint(['q28_compilation_measurement', module.PROFILE])
        helper = (local / 'production_version_object.py').read_bytes()
        (local / 'production_version_object.py').write_bytes(helper + b'\n# metadata identity control\n')
        check(pairing.profile_fingerprint(['q28_compilation_measurement', module.PROFILE]) != fingerprint)
        (local / 'production_version_object.py').write_bytes(helper)
        check(pairing.profile_fingerprint(['q28_compilation_measurement', module.PROFILE]) == fingerprint)
        payloads = module._snapshot_inputs()
        check(tuple(descriptor['case'] for _, descriptor in payloads) == module.EXECUTED_CASES)
        check(len(payloads) == 7 and module.PENDING_CASE not in module.EXECUTED_CASES)
        check(all(descriptor['pilot_local_file_spilling_enabled'] is False for _, descriptor in payloads))
        receipt = dict(revision='a' * 40, metadata=dict(profile='rbo', compile_argv=['compile'], link_argv=['link'],
                       compile_cwd=str(local), link_cwd=str(local), compile_environment={}, link_environment={}),
                       build=dict(target='ydb/core/kqp/ut/rbo'), files={})
        raw_receipt = json.dumps(receipt).encode()
        receipt_path = types.SimpleNamespace(read_bytes=lambda: raw_receipt, resolve=lambda: local / 'receipt')
        args = types.SimpleNamespace(command='measure-compilation', bug='Q28', phase='baseline', sample_index=0,
            receipt=receipt_path, worktree=local, compile_timeout=7, link_timeout=11, native_timeout=13)
        runtime = {'environment': b'A=metadata\0', 'affinity': b'0\n'}
        valid = dict(completed=True, phase='complete', stats_present=True, compilation_present=True,
                     from_cache=False, server_compilation_duration_us=701, server_compilation_cpu_us=101,
                     result_set_count=1, runtime_environment_verified=True, runtime_affinity_verified=True)
        outcomes = {}
        scenarios = ['normal', 'cold_rejected', 'prepare_cancelled', 'link_cancelled', 'native_cancelled',
                     'postlink_changed', 'final_changed', 'profile_changed', 'seal_failed', 'seal_cancelled', 'handoff_cancelled', 'readonly_digest_changed'] + ['allocation_' + str(i) for i in range(1, 18)]
        for scenario in scenarios:
            buffers, closed, events = {}, [], []
            allocations, checks, native_count, fingerprints = [0], [0], [0], [0]
            handoff = {}
            def memory(label, value=b'', **unused):
                allocations[0] += 1
                if scenario == 'allocation_' + str(allocations[0]):
                    raise OSError('metadata allocation interruption')
                fd = allocations[0] + 100
                buffers[fd] = value
                return fd
            def close(fd):
                check(fd in buffers and fd not in closed)
                closed.append(fd)
                del buffers[fd]
            def identity(fd):
                if fd == handoff.get('readonly'):
                    check(handoff['writable'] not in buffers)
                    if scenario == 'readonly_digest_changed' and native_count[0] == 7:
                        return dict(sha256='f' * 64, size_bytes=len(buffers[fd]))
                return dict(sha256=digest(buffers[fd]), size_bytes=len(buffers[fd]))
            def seal(fds):
                events.append('seal')
                check(events[-2] == 'version_verify' and 'link' in events)
                check(not handoff)
                if scenario == 'seal_failed':
                    raise metadata.MetadataError('mock seal failure')
                if scenario == 'seal_cancelled':
                    raise KeyboardInterrupt()
                writable = fds['executable']
                readonly = memory('readonly', buffers[writable])
                fds['executable'] = readonly
                close(writable)
                handoff.update(writable=writable, readonly=readonly)
                if scenario == 'handoff_cancelled':
                    raise KeyboardInterrupt()
                return dict(version='sealed_read_only_memfd_v1', same_inode=True,
                    write_grow_shrink_and_seal_seals=True, writable_owner_closed=True)
            def prepare(actual_receipt, actual_path, worktree, fds, **settings):
                events.append('version_prepare')
                check(actual_receipt == receipt and actual_path is receipt_path and worktree == local)
                check(settings['timeout'] == 7)
                fds[version.OBJECT] = memory('version', b'production metadata object')
                if scenario == 'prepare_cancelled':
                    raise KeyboardInterrupt()
                return ['link', '@FD:' + version.OBJECT + '@'], {'reviewed': True}
            def verify(evidence, fds):
                events.append('version_verify')
                checks[0] += 1
                check(evidence == {'reviewed': True} and version.OBJECT in fds)
                if (scenario == 'postlink_changed' and checks[0] == 1
                    or scenario == 'final_changed' and checks[0] == 16):
                    raise metadata.MetadataError('metadata version identity changed')
            def transport(argv):
                check(argv == ['link', '@FD:' + version.OBJECT + '@'])
                return argv, {}
            def run(argv, cwd, fds, **settings):
                output = settings.get('stdout_fd')
                result = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False,
                              stdout_overflow=False, environment_sha256=direct._json_digest({}))
                if output == 'object':
                    events.append('compile')
                    check(version.OBJECT not in fds)
                    buffers[fds[output]] = b'compiled metadata adapter'
                elif output == 'executable':
                    events.append('link')
                    check(version.OBJECT in fds and argv == ['link', '@FD:' + version.OBJECT + '@'])
                    check(settings['max_scope_gib'] == 36 and settings['timeout'] == 11)
                    if scenario == 'link_cancelled':
                        raise InterruptedError('metadata cancellation')
                    buffers[fds[output]] = b'linked metadata executable'
                else:
                    events.append('native')
                    check(fds['executable'] == handoff['readonly'] and handoff['writable'] not in buffers)
                    native_count[0] += 1
                    check(version.OBJECT not in fds and 'source' not in fds and 'object' not in fds)
                    check(settings['max_scope_gib'] == 12 and settings['timeout'] == 13)
                    check(settings['compiler_environment'] is False and settings['retain_stdout'] == 0)
                    if scenario == 'native_cancelled':
                        raise KeyboardInterrupt()
                    value = dict(valid)
                    if scenario == 'cold_rejected':
                        value.update(completed=False, phase='cold_sample', from_cache=True)
                        result['exit_code'] = 1
                    buffers[fds['report']] = json.dumps(value).encode()
                return result, b''
            def profile(*unused):
                fingerprints[0] += 1
                return ('c' if scenario == 'profile_changed' and fingerprints[0] > 1 else 'b') * 64
            with ExitStack() as stack:
                for obj, key, value in (
                    (direct, '_expected_revision', lambda *unused: 'a' * 40),
                    (metadata, 'verify_receipt', lambda *unused: None),
                    (pairing, 'profile_fingerprint', profile), (module, '_runtime_snapshot', lambda: runtime),
                    (version, 'prepare_object', prepare), (version, 'verify_object', verify),
                    (direct, '_memfd', memory), (direct, '_fd_digest', identity), (direct, '_seal_native_executable', seal),
                    (direct, '_stdout_link_arguments', transport),
                    (direct, '_archive_response_arguments', lambda argv, *unused: (argv, {'responses': []})),
                    (direct, 'run_anonymous', run), (os, 'close', close), (os, 'fchmod', lambda *unused: None),
                    (os, 'pread', lambda fd, size, offset: buffers[fd][offset:offset + size]),
                ):
                    stack.enter_context(patch.object(obj, key, value))
                try:
                    evidence, code = module.execute_measurements(args)
                except (OSError, KeyboardInterrupt, metadata.MetadataError):
                    check(scenario not in ('normal', 'cold_rejected'))
                else:
                    check(scenario in ('normal', 'cold_rejected'))
                    check(code == (0 if scenario == 'normal' else 2))
                    for key in ('pilot_coverage_complete', 'full_q28_admission', 'query_semantics_verified',
                                'quality_improvement_verified', 'optimizer_stage_timings_verified', 'structural_query_scaling_measured'):
                        check(evidence[key] is False)
                    check(evidence['pending_cases'][0]['case'] == module.PENDING_CASE and evidence['pending_cases'][0]['executed'] is False)
                    check(evidence['native_executable_transport']['writable_owner_closed'] is True)
                    if scenario == 'normal':
                        check(native_count[0] == 7 and checks[0] == 16)
                check(not buffers)
            outcomes[scenario] = dict(fake_descriptors_closed=len(closed), fake_native_calls=native_count[0])
        combined_hash = digest((local / 'anonymous_cpp_verify.py').read_bytes())
    check(all(digest((ROOT / name).read_bytes()) == value for name, value in live_bindings.items()))
    result = dict(status='independent_metadata_passed_native_pending', assertions=COUNT,
        reviewed_patch_sha256=preparation['patch_sha256'], source_preparation_sha256=digest(prep_path.read_bytes()),
        combined_dispatcher_sha256=combined_hash, scenarios=outcomes,
        source_sha256=digest(Path(__file__).read_bytes()), native_runs=0, build_runs=0, toolchain_runs=0,
        live_helper_edits=False, rendered_queries_or_cpp_or_repros_persisted=False)
    output = ROOT / 'verification/Q28-readonly-executable-metadata-contracts-20261028.json'
    output.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps(dict(assertions=COUNT, report_sha256=digest(output.read_bytes()))))


if __name__ == '__main__':
    main()
