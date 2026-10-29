"""Source and mocked process checks; never invoke a compiler, linker or server."""
import ast
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import types
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
PROPOSED = ROOT / 'proposed-correctness-amendments'
sys.path.insert(0, str(ROOT))
import anonymous_cpp_metadata as metadata
import anonymous_cpp_verify as direct

COUNT = 0


def check(value):
    global COUNT
    if value is not True:
        raise AssertionError('Production version metadata check ordinal ' + str(COUNT + 1))
    COUNT += 1


def sha(value):
    return hashlib.sha256(value).hexdigest()


def main():
    candidate = PROPOSED / 'production_version_object_20261028.candidate.py'
    module = types.ModuleType('production_version_object')
    module.__file__ = str(candidate)
    exec(compile(candidate.read_bytes(), str(candidate), 'exec'), module.__dict__)
    receipt_path = ROOT / 'verification/cpp-closures/24e1aae9a067-1792736142124693334/receipt.json'
    raw_receipt = receipt_path.read_bytes()
    receipt = json.loads(raw_receipt)
    plan = module.prepare_plan(receipt, receipt['worktree'])
    check(plan['identity']['vcs_resource_sha256'] == '8ef6f728fa6ab8197cd6a32856e34cc6069ab650886126cc88f5263db29671aa')
    check(plan['identity']['vcs_resource_bytes'] == 1543)
    check(plan['identity']['metadata_strings_used_as_source_provenance'] is False)
    check(plan['identity']['target_resource_objects_changed'] is False)
    check(plan['link_argv'].count('@FD:production_version_object@') == 1)
    check([arg for arg in plan['link_argv'] if arg != '@FD:production_version_object@'] == receipt['metadata']['link_argv'])
    check(plan['compile_argv'].count('@FD:production_version_source@') == 1)
    check(plan['compile_argv'][-2:] == ['-o', '-'])
    check('-x' in plan['compile_argv'] and plan['compile_argv'][plan['compile_argv'].index('-x') + 1] == 'c')
    check(plan['generate_argv'][1] == '-B')
    for name in ('vcs_info.py', 'svn_interface.c', 'svnversion.h', 'link_exe.py'):
        check(any(Path(item).name == name for item in plan['files']))
    check(len(plan['files']) == 16)
    check(any(Path(item).name == 'clang' for item in plan['files']))
    conf, node = module.selected_graph_data(receipt['graph']['resolved_path'], receipt['metadata']['link_node_uid'])
    for change in ('missing_resource', 'malformed_resource', 'extra_generate_argument', 'changed_generate_env',
                   'missing_compile', 'changed_link', 'removed_version_not_declared'):
        changed_conf, changed_node, changed_receipt = json.loads(json.dumps(conf)), json.loads(json.dumps(node)), json.loads(json.dumps(receipt))
        if change == 'missing_resource':
            changed_conf['resources'] = [item for item in changed_conf['resources'] if item.get('pattern') != 'VCS']
        elif change == 'malformed_resource':
            next(item for item in changed_conf['resources'] if item.get('pattern') == 'VCS')['resource'] = 'base64:vcs.json:!'
        elif change == 'extra_generate_argument':
            changed_node['cmds'][0]['cmd_args'].append('unexpected')
        elif change == 'changed_generate_env':
            changed_node['cmds'][0]['env']['unexpected'] = 'value'
        elif change == 'missing_compile':
            del changed_node['cmds'][1]
        elif change == 'changed_link':
            changed_receipt['metadata']['link_argv'].append('unexpected')
        else:
            changed_receipt['metadata']['removed_test_inputs'] = []
        with patch.object(module, 'selected_graph_data', return_value=(changed_conf, changed_node)):
            try:
                module.prepare_plan(changed_receipt, receipt['worktree'])
            except metadata.MetadataError:
                check(True)
            else:
                check(False)
    old = ast.parse((ROOT / 'anonymous_cpp_verify.py').read_bytes())
    new = ast.parse((PROPOSED / 'anonymous_cpp_verify_production_version_20261028.candidate.py').read_bytes())
    functions = lambda tree: {item.name: ast.dump(item) for item in tree.body if isinstance(item, ast.FunctionDef) and item.name != 'execute'}
    check(functions(old) == functions(new))
    with tempfile.TemporaryDirectory(prefix='version-object-metadata-') as directory:
        copied = Path(directory) / 'receipt.json'
        dummy = dict(revision='a' * 40, worktree=receipt['worktree'])
        copied.write_text(json.dumps(dummy))
        snapshots = {name: {'sha256': sha(name.encode())} for name in plan['files']}
        mock_plan = dict(plan, identity=dict(plan['identity'], expected_production_source={
            'sha256': sha(b'bounded mocked output'), 'size_bytes': len(b'bounded mocked output')}))
        scenarios = {}
        for scenario in ('valid', 'generate_failure', 'compile_failure', 'generate_overflow',
                         'compile_guard', 'compile_environment', 'empty_source', 'oversized_object',
                         'dependency_changed', 'seal_failure', 'generated_source_differs'):
            fds, contents, seals, closed, calls = {}, {}, {}, [], []
            file_calls = [0]

            def fake_memfd(name, value=b''):
                fd = 100 + len(contents)
                contents[fd] = bytes(value)
                return fd

            def fake_digest(fd):
                data = contents[fd]
                return dict(sha256=sha(data), size_bytes=len(data))

            def fake_fcntl(fd, operation, value=None):
                if operation == module.fcntl.F_ADD_SEALS:
                    seals[fd] = value
                return 0 if scenario == 'seal_failure' else seals.get(fd, 0)

            def fake_files(paths):
                file_calls[0] += 1
                result = dict(snapshots)
                if scenario == 'dependency_changed' and file_calls[0] > 1:
                    result[next(iter(result))] = {'sha256': 'b' * 64}
                return result

            def fake_run(argv, cwd, descriptors, **settings):
                stage = 'generate' if settings['stdout_fd'] is None else 'compile'
                calls.append(stage)
                check(settings['max_scope_gib'] == 12 and settings['timeout'] == 7 and settings['retain_stdout'] == 0)
                check(set(descriptors) == {'production_version_metadata', 'production_version_source', module.OBJECT})
                check(seals[descriptors['production_version_metadata']] == module.SEALS)
                if stage == 'compile':
                    check(seals[descriptors['production_version_source']] == module.SEALS)
                destination = 'production_version_source' if stage == 'generate' else module.OBJECT
                contents[descriptors[destination]] = b'bounded mocked output'
                if scenario == 'empty_source' and stage == 'generate':
                    contents[descriptors[destination]] = b''
                if scenario == 'generated_source_differs' and stage == 'generate':
                    contents[descriptors[destination]] = b'different mocked output'
                if scenario == 'oversized_object' and stage == 'compile':
                    contents[descriptors[destination]] = b'x' * (16 * 1024 * 1024 + 1)
                result = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False, stdout_overflow=False,
                              max_scope_gib=12, min_host_available_gib=36,
                              environment_sha256=module._json_hash(settings['environment']))
                if scenario == stage + '_failure':
                    result['exit_code'] = 1
                if scenario == stage + '_overflow':
                    result['stdout_overflow'] = True
                if scenario == stage + '_guard':
                    result['cleanup_complete'] = False
                if scenario == stage + '_environment':
                    result['environment_sha256'] = '0' * 64
                return result, b''

            with ExitStack() as stack:
                for obj, name, value in (
                    (module, 'prepare_plan', lambda *unused: mock_plan), (module, '_files', fake_files),
                    (module, '_memfd', fake_memfd), (module.fcntl, 'fcntl', fake_fcntl),
                    (module.os, 'close', lambda fd: closed.append(fd)),
                    (metadata, 'verify_receipt', lambda *unused: None),
                    (direct, 'run_anonymous', fake_run), (direct, '_fd_digest', fake_digest),
                ):
                    stack.enter_context(patch.object(obj, name, value))
                try:
                    link, evidence = module.prepare_object(dummy, copied, dummy['worktree'], fds, timeout=7)
                except metadata.MetadataError:
                    check(scenario != 'valid')
                else:
                    check(scenario == 'valid')
                    check(link == plan['link_argv'])
                    check(module.validate_evidence(evidence, dummy, copied.read_bytes()))
                    module.verify_object(evidence, fds)
                    for key, replacement in (('sealed_inputs_and_object', False), ('receipt_sha256', '0' * 64),
                                             ('restored_input_position', -1), ('bound_files', {}), ('compile', {}),
                                             ('production_version_source', {'sha256': '0' * 64, 'size_bytes': 7})):
                        changed = dict(evidence, **{key: replacement})
                        check(module.validate_evidence(changed, dummy, copied.read_bytes()) is False)
                    contents[fds[module.OBJECT]] += b'changed'
                    try:
                        module.verify_object(evidence, fds)
                    except metadata.MetadataError:
                        check(True)
                    else:
                        check(False)
                check(set(fds) == {module.OBJECT})
                check(len(closed) == 2 and len(set(closed)) == 2)
                check(fds[module.OBJECT] not in closed)
            scenarios[scenario] = dict(mocked_guarded_steps=len(calls), temporary_descriptors_closed=len(closed))
    check(receipt_path.read_bytes() == raw_receipt)
    result = dict(status='source_prepared_native_pending', assertions=COUNT, scenarios=scenarios,
                  exact_existing_graph_plan_prepared=True, production_version_input=plan['identity'],
                  production_file_count=len(plan['files']), immutable_receipt_sha256=sha(raw_receipt),
                  native_runs=0, toolchain_runs=0, build_runs=0, live_helper_edits=False,
                  generated_source_object_executable_or_diagnostics_persisted=False)
    output = ROOT / 'verification/C31-production-version-object-metadata-checks-20261028.json'
    output.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps(dict(assertions=COUNT, report_sha256=sha(output.read_bytes()))))


if __name__ == '__main__':
    main()
