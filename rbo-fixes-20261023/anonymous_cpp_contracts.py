"""Mocked contracts for anonymous execution; never launches compiler or native code."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import anonymous_cpp_guard as guard
import anonymous_cpp_metadata as metadata
import anonymous_cpp_verify as verify


def run_contracts():
    assertions = 0
    def check(value):
        nonlocal assertions
        assert value
        assertions += 1
    def rejects(callable_):
        nonlocal assertions
        try:
            callable_()
        except (metadata.MetadataError, ValueError):
            assertions += 1
        else:
            raise AssertionError('Expected closed rejection')

    source = '/production/sample.cpp'
    compile_args = ['/tools/clang++', '-I/production', '-std=c++23', '-g', '-MMD',
                    '-MF', '/output/sample.d', '-c', source, '-o', '/output/sample.o']
    template = metadata.compile_template(compile_args, source)
    check(template == ['/tools/clang++', '-I/production', '-std=c++20', '-g0',
                       '-fno-crash-diagnostics', '-fintegrated-as', '-x', 'c++', '-c', '@FD:source@', '-o', '-'])
    for flag in ('-save-temps=obj', '-ftime-trace', '-fmodules', '-flto=thin',
                 '-fprofile-generate', '-gsplit-dwarf', '-fno-integrated-as',
                 '-Wl,--reproduce=/tmp/unsafe.tar', '-fproc-stat-report=/tmp/stats'):
        rejects(lambda flag=flag: metadata.compile_template(compile_args + [flag], source))
    for arguments in (['-Wl,--reproduce,/tmp/unsafe.tar'], ['-Xlinker', '--time-trace'],
                      ['-Wl,--dependency-file,/tmp/deps'], ['-Wl,-Map,/tmp/map'],
                      ['-Wl,--why-extract=/tmp/extract'], ['-Wl,@unbound'],
                      ['-Wl,--script=unbound'], ['-Xlinker', '@unbound']):
        rejects(lambda arguments=arguments: metadata.compile_template(compile_args + arguments, source))
    rejects(lambda: metadata.compile_template(compile_args + [source], source))
    rejects(lambda: metadata.compile_template(compile_args + ['-o', '/other'], source))
    link, removed = metadata.link_template(['/tools/clang++', '-fuse-ld=lld',
        '/output/yql/essentials/ast/ut/sample.o', '/output/library/cpp/testing/unittest_main/main.o',
        '/output/yql/essentials/ast/libast.a', '-o', '/output/ut'], 'yql/essentials/ast/ut', '/output')
    check(len(removed) == 2 and '/output/yql/essentials/ast/libast.a' in link)
    check(link[-4:] == ['@FD:object@', '-Wl,--no-mmap-output-file', '-o', '@FD:executable@'])
    rejects(lambda: metadata.link_template(['/tools/clang++', '/output/lib.a', '-o', '/output/ut'],
                                           'yql/essentials/ast/ut', '/output'))
    rejects(lambda: metadata._replace('$(UNMAPPED)/clang++', {}))
    rejects(lambda: metadata._unwrap(['/python', '/unknown-wrapper.py', '/tools/clang++'], '/repo', '/out', False))

    valid = [{'case_index': 0, 'checks_executed': 3, 'passed': True},
             {'case_index': 1, 'checks_executed': 1, 'passed': False}]
    encoded = lambda values: b'\n'.join(json.dumps(value).encode() for value in values)
    check(verify.parse_verdicts(encoded(valid), 2, 1, 'Q18') == valid)
    for values, count, code in ((valid, 2, 0), (valid[:1], 2, 0), (valid + valid[:1], 3, 1),
                                 ([dict(valid[0], checks_executed=0)], 1, 0),
                                 ([dict(valid[0], case_index=True)], 1, 0),
                                 ([dict(valid[0], passed=1)], 1, 0),
                                 ([dict(valid[0], raw_source='forbidden')], 1, 0)):
        rejects(lambda values=values, count=count, code=code: verify.parse_verdicts(encoded(values), count, code, 'Q18'))
    check(verify.parse_verdicts(encoded([{'case_index': 0, 'checks': 2, 'passed': True}]), 1, 0, 'Q25')[0]['checks_executed'] == 2)
    rejects(lambda: verify.parse_verdicts(encoded(valid[:1]), 1, 0, 'Q27'))

    def evidence(phase, passed):
        process = {'exit_code': 0, 'cleanup_complete': True, 'watchdog_stopped': False, 'stdout_overflow': False}
        return {'schema_version': 'anonymous_direct_cpp_v1', 'bug': 'Q18', 'phase': phase,
                'revision': ('a' if phase == 'baseline' else 'b') * 40,
                'generator_module': 'quality_cpp_contract_generator', 'generator_sha256': '1' * 64,
                'generated_source_sha256': '2' * 64, 'closure_receipt_sha256': '3' * 64,
                'generated_object': {'sha256': '4' * 64, 'size_bytes': 16},
                'generated_executable': {'sha256': '5' * 64, 'size_bytes': 32},
                'proof_scope': 'linked_production_local_contracts', 'case_count': 1,
                'closure_provenance_verified': True, 'generated_files_persisted': False,
                'compile': dict(process), 'link': dict(process), 'native': dict(process, exit_code=0 if passed else 1),
                'cases': [{'case_index': 0, 'checks_executed': 1, 'passed': passed}]}
    before, after = evidence('baseline', False), evidence('fixed', True)
    case = {'base': 'a' * 40, 'commit': 'b' * 40}
    paired = verify.pair_direct('Q18', before, after, case)
    check(paired['local_contract_improvement_verified'] and not paired['quality_finding_fully_verified'])
    for key, value in (('revision', 'c' * 40), ('closure_provenance_verified', False),
                       ('generated_files_persisted', True), ('closure_receipt_sha256', None),
                       ('generator_sha256', 'd' * 64), ('generated_source_sha256', 'e' * 64),
                       ('generator_module', 'other'), ('case_count', 2), ('proof_scope', 'full_query')):
        rejects(lambda key=key, value=value: verify.pair_direct('Q18', before, dict(after, **{key: value}), case))
    stopped = copy.deepcopy(after)
    stopped['native']['watchdog_stopped'] = True
    rejects(lambda: verify.pair_direct('Q18', before, stopped, case))
    unchanged = evidence('baseline', True)
    check(not verify.pair_direct('Q18', unchanged, after, case)['local_contract_improvement_verified'])
    regressed = evidence('fixed', False)
    check(not verify.pair_direct('Q18', unchanged, regressed, case)['local_contract_improvement_verified'])
    rejects(lambda: verify.pair_direct('Q15', before, after, case))
    check(not verify.validate_fixed_admission(after, 'Q15', 'b' * 40))
    import quality_partition_cpp_checks as q15
    with tempfile.TemporaryDirectory(prefix='rbo-admission-contract-') as temporary:
        receipt_path = Path(temporary) / 'receipt.json'
        receipt = {'schema_version': metadata.VERSION, 'revision': 'b' * 40,
                   'build': {'revision': 'b' * 40, 'exit_code': 0, 'target': 'ydb/core/kqp/ut/rbo'},
                   'metadata': {'profile': 'rbo'}, 'files': {'synthetic-production-object': {'sha256': '6' * 64}}}
        receipt_path.write_text(json.dumps(receipt))
        fixed = evidence('fixed', True)
        fixed.update(bug='Q15', generator_module='quality_partition_cpp_checks',
                     generator_sha256=metadata.digest_file(q15.__file__)['sha256'],
                     generated_source_sha256=hashlib.sha256(q15.render_cpp_suite().encode()).hexdigest(),
                     case_count=len(q15.cases()), closure_receipt_path=str(receipt_path),
                     closure_receipt_sha256=metadata.digest_file(receipt_path)['sha256'],
                     status='direct_local_contracts_passed',
                     cases=[{'case_index': index, 'checks_executed': 1, 'passed': True} for index in range(len(q15.cases()))])
        check(verify.validate_fixed_admission(fixed, 'Q15', 'b' * 40))
        for key, value in (('revision', 'a' * 40), ('phase', 'baseline'), ('generator_sha256', '9' * 64),
                           ('generated_source_sha256', '8' * 64), ('case_count', 1), ('cases', fixed['cases'][:-1])):
            check(not verify.validate_fixed_admission(dict(fixed, **{key: value}), 'Q15', 'b' * 40))
        receipt_path.write_text('{}')
        check(not verify.validate_fixed_admission(fixed, 'Q15', 'b' * 40))

    # Actual memfds contain only a synthetic transport marker. Process creation,
    # core-limit mutation and compiler execution are all mocked.
    marker = b'anonymous-transport-marker'
    source_fd = verify._memfd('transport-contract', marker)
    object_fd = verify._memfd('transport-output')
    manifest_fd = None
    try:
        check(verify._fd_digest(source_fd) == {'sha256': hashlib.sha256(marker).hexdigest(), 'size_bytes': len(marker)})
        manifest = {'argv': ['/tools/clang++', '@FD:source@'], 'cwd': '/tmp', 'stdout_fd': 'object',
                    'fds': {'source': '/proc/' + str(os.getpid()) + '/fd/' + str(source_fd),
                            'object': '/proc/' + str(os.getpid()) + '/fd/' + str(object_fd)}}
        manifest_fd = verify._memfd('transport-manifest', json.dumps(manifest).encode())
        calls = []
        inherited_outputs = {key: 'environment-marker' for key in (
            'LLD_REPRODUCE', 'AS_SECURE_LOG_FILE', 'CLANG_MODULE_CACHE_PATH',
            'CC_PRINT_OPTIONS', 'CC_PRINT_OPTIONS_FILE', 'CC_PRINT_HEADERS',
            'CC_PRINT_HEADERS_FILE', 'CC_LOG_DIAGNOSTICS', 'CC_LOG_DIAGNOSTICS_FILE',
            'CC_PRINT_PROC_STAT', 'CC_PRINT_PROC_STAT_FILE',
            'CC_PRINT_INTERNAL_STAT', 'CC_PRINT_INTERNAL_STAT_FILE')}
        def fake_run(command, **kwargs):
            check(len(kwargs['pass_fds']) == 2 and kwargs['stdout'] in kwargs['pass_fds'])
            check(Path(command[1]).read_bytes() == marker)
            check(kwargs['env']['TMPDIR'].startswith('/proc/') and kwargs['env']['LC_ALL'] == 'C')
            check(not inherited_outputs.keys() & kwargs['env'].keys())
            calls.append(command)
            return subprocess.CompletedProcess(command, 0)
        with patch.object(guard.subprocess, 'run', side_effect=fake_run), patch.object(guard.resource, 'setrlimit') as limits, \
             patch.dict(os.environ, inherited_outputs):
            check(guard._worker('/proc/self/fd/' + str(manifest_fd)) == 0)
            check(limits.call_args.args[0] == guard.resource.RLIMIT_CORE)
        check(len(calls) == 1)
        os.ftruncate(manifest_fd, 0)
        os.lseek(manifest_fd, 0, os.SEEK_SET)
        os.write(manifest_fd, json.dumps(dict(manifest, compiler_environment=False)).encode())
        def native_fake_run(command, **kwargs):
            check(kwargs['env'] == dict(os.environ))
            check(all(kwargs['env'][key] == value for key, value in inherited_outputs.items()))
            return subprocess.CompletedProcess(command, 0)
        with patch.object(guard.subprocess, 'run', side_effect=native_fake_run), \
             patch.object(guard.resource, 'setrlimit'), patch.dict(os.environ, inherited_outputs):
            check(guard._worker('/proc/self/fd/' + str(manifest_fd)) == 0)
    finally:
        os.close(source_fd)
        os.close(object_fd)
        if manifest_fd is not None:
            os.close(manifest_fd)

    consumed = {}
    guard._consume(io.BytesIO(b'abcdef'), consumed, 2)
    check(consumed['retained'] == b'ab' and consumed['size_bytes'] == 6 and consumed['overflow'])
    check(consumed['sha256'] == hashlib.sha256(b'abcdef').hexdigest())
    diagnostics = {}
    guard._consume(io.BytesIO(b'private diagnostics'), diagnostics, 0)
    check(diagnostics['retained'] == b'' and diagnostics['size_bytes'] == 19)
    with patch.object(guard, 'available_memory', return_value=30 * guard.GIB), patch.object(guard.subprocess, 'Popen') as spawn:
        record, output = guard.run_anonymous(['/not-executed'], '/tmp', {})
        check(record['exit_code'] == 75 and record['cleanup_complete'] and not output)
        check(not spawn.called)

    class FakeProcess:
        pid = 99999999
        returncode = 0
        stdout = io.BytesIO(b'{"numeric":1}\n')
        stderr = io.BytesIO(b'private diagnostics')
        def poll(self):
            return self.returncode
        def wait(self, timeout=None):
            return self.returncode
    with patch.object(guard, 'available_memory', return_value=80 * guard.GIB), \
         patch.object(guard.subprocess, 'Popen', return_value=FakeProcess()) as spawn, \
         patch.object(guard, 'scope_control_group', return_value='/mock/scope'), \
         patch.object(guard, 'scope_stopped', return_value=True), \
         patch.object(guard, 'group_rss', return_value=0), \
         patch.object(guard, 'scope_memory', return_value=0), \
         patch.object(guard, 'scope_rss', return_value=0):
        record, output = guard.run_anonymous(['/not-executed'], '/tmp', {})
        check(record['exit_code'] == 0 and record['cleanup_complete'])
        check(output == b'{"numeric":1}\n' and 'private' not in json.dumps(record))
        check('--scope' in spawn.call_args.args[0] and '--worker' in spawn.call_args.args[0])
    with patch.object(guard, 'available_memory', return_value=80 * guard.GIB), \
         patch.object(guard.subprocess, 'Popen', side_effect=OSError('unsafe details')):
        record, output = guard.run_anonymous(['/not-executed'], '/tmp', {})
        check(record['exit_code'] == 75 and record['cleanup_complete'])
        check(record['guard_exception_type'] == 'OSError' and 'unsafe' not in json.dumps(record))
    class RunningProcess:
        pid = 99999998
        returncode = None
        def __init__(self):
            self.stdout, self.stderr = io.BytesIO(b''), io.BytesIO(b'')
        def poll(self):
            return self.returncode
        def wait(self, timeout=None):
            return self.returncode
    for mode in ('scope_budget', 'cancellation'):
        process = RunningProcess()
        def stop(*args):
            process.returncode = -15
        with patch.object(guard, 'available_memory', return_value=80 * guard.GIB), \
             patch.object(guard.subprocess, 'Popen', return_value=process), \
             patch.object(guard, 'scope_control_group', return_value='/mock/scope'), \
             patch.object(guard, 'scope_stopped', side_effect=lambda *args: process.returncode is not None), \
             patch.object(guard, 'stop_scope', side_effect=stop) as stopping, \
             patch.object(guard, 'group_rss', side_effect=InterruptedError('cancel') if mode == 'cancellation' else None,
                          return_value=0), \
             patch.object(guard, 'scope_memory', return_value=None), \
             patch.object(guard, 'scope_rss', return_value=27 * guard.GIB), \
             patch.object(guard.time, 'sleep'):
            record, output = guard.run_anonymous(['/not-executed'], '/tmp', {})
            check(record['exit_code'] == 75 and record['cleanup_complete'] and stopping.called)
            check(record['stop_reason'] == ('direct_memory_budget' if mode == 'scope_budget' else 'guard_cancelled'))
            if mode == 'scope_budget':
                check(record['peak_scope_usage_bytes'] == 27 * guard.GIB and record['peak_group_rss_bytes'] == 0)

    # Source and receipt verification refuse mixed revisions and stale objects.
    with tempfile.TemporaryDirectory(prefix='rbo-direct-contract-') as temporary:
        folder = Path(temporary)
        artifact = folder / 'production-object.o'
        artifact.write_bytes(b'production-object-placeholder')
        graph = folder / 'production-graph.json'
        graph.write_text('{}')
        receipt = {'schema_version': metadata.VERSION, 'revision': 'a' * 40,
                   'worktree': str(folder.resolve()), 'build': {'exit_code': 0},
                   'files': {str(artifact): metadata.digest_file(artifact)}, 'graph': metadata.digest_file(graph)}
        with patch.object(metadata, 'source_revision', return_value='a' * 40):
            check(metadata.verify_receipt(receipt, folder, 'a' * 40))
            rejects(lambda: metadata.verify_receipt(receipt, folder, 'b' * 40))
            artifact.write_bytes(b'changed-production-object')
            rejects(lambda: metadata.verify_receipt(receipt, folder, 'a' * 40))
        with patch.object(metadata, 'source_revision', return_value='b' * 40):
            rejects(lambda: metadata.verify_receipt(receipt, folder, 'a' * 40))
    # Adapt a small synthetic production graph using the real, read-only ya
    # link-wrapper option parser. No source file, command or build is executed.
    worktree = Path(__file__).resolve().parent / 'worktrees/verify'
    with tempfile.TemporaryDirectory(prefix='rbo-graph-contract-') as temporary:
        folder = Path(temporary).resolve()
        output, tools = folder / 'outputs', folder / 'tools'
        files = ('yql/essentials/ast/ut/yql_expr_ut.cpp.o',
                 'yql/essentials/ast/libast.a', 'library/cpp/testing/unittest_main/main.cpp.o')
        for name in files:
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'production-artifact-placeholder')
        tools.mkdir()
        for name in ('clang++', 'ld.lld'):
            (tools / name).write_bytes(b'non-executable-tool-placeholder')
        sample = '$(SOURCE_ROOT)/yql/essentials/ast/ut/yql_expr_ut.cpp'
        obj = '$(BUILD_ROOT)/' + files[0]
        graph = {'graph': [
            {'uid': 'compile-node', 'outputs': [obj], 'cmds': [{'cmd_args': [
                '$(TOOLS)/clang++', '-I$(SOURCE_ROOT)', '-c', sample, '-o', obj]}]},
            {'uid': 'library-node', 'outputs': ['$(BUILD_ROOT)/' + files[1]], 'cmds': []},
            {'uid': 'test-main-node', 'outputs': ['$(BUILD_ROOT)/' + files[2]], 'cmds': []},
            {'uid': 'link-node', 'outputs': ['$(BUILD_ROOT)/yql/essentials/ast/ut/ut'], 'cmds': [{'cmd_args': [
                '/usr/bin/python3', '$(SOURCE_ROOT)/build/scripts/link_exe.py', '--start-plugins', '--end-plugins', '--arch', 'LINUX',
                '$(TOOLS)/clang++', '-fuse-ld=lld', files[0], files[1],
                files[2], '-o', 'yql/essentials/ast/ut/ut']}]}]}
        adapted = metadata.extract_metadata(graph, worktree, output, 'ast', {'TOOLS': str(tools)})
        check(adapted['compile_node_uid'] == 'compile-node' and adapted['link_node_uid'] == 'link-node')
        check(len(adapted['removed_test_inputs']) == 2 and adapted['graph_output_count'] == 3)
        check(str(tools / 'ld.lld') in adapted['files'] and str(output / files[1]) in adapted['files'])
        rejects(lambda: metadata.extract_metadata(graph, worktree, output, 'ast', {}))
        duplicate = copy.deepcopy(graph)
        duplicate['graph'].append(copy.deepcopy(duplicate['graph'][0]))
        rejects(lambda: metadata.extract_metadata(duplicate, worktree, output, 'ast', {'TOOLS': str(tools)}))
        changed_env = copy.deepcopy(graph)
        changed_env['graph'][0]['cmds'][0]['env'] = {'CPATH': '/unknown'}
        rejects(lambda: metadata.extract_metadata(changed_env, worktree, output, 'ast', {'TOOLS': str(tools)}))
        graph_path = folder / 'production-graph.json'
        graph_path.write_text(json.dumps(graph))
        build = {'revision': 'a' * 40, 'profile': 'ast', 'target': 'yql/essentials/ast/ut',
                 'exit_code': 0, 'export_root': str(output), 'graph_path': str(graph_path)}
        with patch.object(metadata, 'source_revision', return_value='a' * 40):
            receipt = metadata.issue_receipt(worktree, 'a' * 40, build, graph_path, output, 'ast', {'TOOLS': str(tools)})
            check(metadata.verify_receipt(receipt, worktree, 'a' * 40))
            rejects(lambda: metadata.issue_receipt(worktree, 'a' * 40, dict(build, exit_code=1), graph_path, output, 'ast', {'TOOLS': str(tools)}))
            rejects(lambda: metadata.issue_receipt(worktree, 'a' * 40, dict(build, target='unrelated'), graph_path, output, 'ast', {'TOOLS': str(tools)}))
            (tools / 'ld.lld').write_bytes(b'changed-tool')
            rejects(lambda: metadata.verify_receipt(receipt, worktree, 'a' * 40))
        # The suffix deliberately is not exported/captured as generated input.
        auxiliary = output / 'auxiliary.link-input'
        for option in metadata.LINKER_INPUT_OPTIONS:
            for forwarding in ([f'-Wl,{option}=auxiliary.link-input'],
                               [f'-Wl,{option},auxiliary.link-input'],
                               [f'-Wl,{option}', '-Wl,auxiliary.link-input'],
                               ['-Xlinker', option, '-Xlinker', 'auxiliary.link-input']):
                auxiliary.write_bytes(b'production-input-placeholder')
                with_auxiliary = copy.deepcopy(graph)
                with_auxiliary['graph'][-1]['cmds'][0]['cmd_args'].extend(forwarding)
                graph_path.write_text(json.dumps(with_auxiliary))
                with patch.object(metadata, 'source_revision', return_value='a' * 40):
                    receipt = metadata.issue_receipt(worktree, 'a' * 40, build, graph_path, output, 'ast', {'TOOLS': str(tools)})
                    check(str(auxiliary) in receipt['files'])
                    check(metadata.verify_receipt(receipt, worktree, 'a' * 40))
                    auxiliary.write_bytes(b'changed-production-input')
                    rejects(lambda: metadata.verify_receipt(receipt, worktree, 'a' * 40))
                    auxiliary.unlink()
                    rejects(lambda: metadata.issue_receipt(worktree, 'a' * 40, build, graph_path, output, 'ast', {'TOOLS': str(tools)}))
            rejects(lambda option=option: metadata._normalize_paths(['-Wl,' + option], output))
            rejects(lambda option=option: metadata._normalize_paths(['-Wl,' + option + '='], output))
    return {'status': 'mocked_contracts_passed', 'assertions': assertions,
            'compiler_processes_started': 0, 'linker_processes_started': 0,
            'native_processes_started': 0, 'production_builds_started': 0,
            'generated_cpp_sources_persisted': False}


if __name__ == '__main__':
    print(json.dumps(run_contracts(), sort_keys=True))
