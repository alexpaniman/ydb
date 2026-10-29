"""Independent source/metadata checks. No compiler, server, or target process."""
import ast
from contextlib import ExitStack
import hashlib
import importlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
PROPOSED = ROOT / 'proposed-quality-amendments'
PATCH = PROPOSED / 'Q28-sdk-compilation-pilot-20261028.patch'
EXPECTED = '9269565adb8713961c864208020b41c0f58dadb1440d347b5834a9fa4bfd8483'
CHECKS = 0


def check(value):
    global CHECKS
    if value is not True:
        raise AssertionError('Independent metadata check ordinal ' + str(CHECKS + 1))
    CHECKS += 1


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    check(digest(PATCH.read_bytes()) == EXPECTED)
    before = (ROOT / 'anonymous_cpp_verify.py').read_bytes()
    check(digest(before) == 'fce1a48f08006a3ab7d3872bd4d8bb47f87be918e3d02b53b9cc75a52f11c20b')
    plan = ROOT / 'verification/idle-artifact-application-plan-20261028-applied30.json'
    plan_data = json.loads(plan.read_bytes())
    check(len(plan_data['patches']) == 30)
    for item in plan_data['patches']:
        check(digest((ROOT / item['patch']).read_bytes()) == item['sha256'])
    with tempfile.TemporaryDirectory(prefix='q28-independent-metadata-') as directory:
        base = Path(directory)
        local = base / ROOT.name
        local.mkdir()
        (base / 'rbo-review-20261021').symlink_to(ROOT.parent / 'rbo-review-20261021', target_is_directory=True)
        for item in ROOT.glob('*.py'):
            shutil.copyfile(item, local / item.name)
        shutil.copyfile(ROOT / 'cases/Q28.json', local / 'q28-case-metadata.json')
        # Apply only to disposable copies; no generated workloads are written.
        result = subprocess.run(['git', 'apply', str(PATCH.resolve())], cwd=base,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(result.returncode == 0)
        after = (local / 'anonymous_cpp_verify.py').read_bytes()
        check(digest(after) == 'e047fa63bb6707c02ce165a2768a892a3a9a77f40cc6a8db75848055c442bc53')
        original = {node.name: ast.dump(node) for node in ast.parse(before).body if isinstance(node, ast.FunctionDef)}
        updated = {node.name: ast.dump(node) for node in ast.parse(after).body if isinstance(node, ast.FunctionDef)}
        check({k: v for k, v in original.items() if k != 'main'} == {k: v for k, v in updated.items() if k != 'main'})
        sys.path[:0] = [str(local), str(base / 'rbo-review-20261021')]
        module = importlib.import_module('q28_compilation_measurement')
        direct = importlib.import_module('anonymous_cpp_verify')
        metadata = importlib.import_module('anonymous_cpp_metadata')
        pairing = importlib.import_module('quality_pairing')
        check(direct.GENERATORS['Q28'] == 'quality_scheduler_cpp_checks')
        check(len(importlib.import_module(direct.GENERATORS['Q28']).cases()) == 26)
        inputs = module._snapshot_inputs()
        check(tuple(item[1]['case'] for item in inputs) == module.CASES)
        for payload, descriptor in inputs:
            check(list(payload)[:3] == ['config', 'schema', 'query'])
            check(1 <= len(payload) - 3 <= 3)
            check(all(digest(value) == descriptor['input_sha256'][key] for key, value in payload.items()))
            check(descriptor['optimizer_mode_config_verified'])
            check(descriptor['requested_stats_mode'] == 'Basic' and descriptor['requested_exec_mode'] == 'Execute')
        source = module.render_cpp().encode()
        check(all(payload['query'] not in source and payload['schema'] not in source for payload, _ in inputs))
        check(source == module.render_cpp().encode())
        sample = dict(completed=True, phase='complete', stats_present=True, compilation_present=True,
                      from_cache=False, server_compilation_duration_us=1001, server_compilation_cpu_us=103,
                      result_set_count=1)
        valid = module.parse_measurement(json.dumps(sample).encode(), 0)
        check(valid['measurement_valid'])
        for key in ('query_semantics_verified', 'optimizer_stage_timings_verified', 'quality_improvement_verified'):
            check(valid[key] is False)
        bad = [dict(sample, **{key: value}) for key, value in (
            ('completed', 1), ('stats_present', 1), ('compilation_present', 1), ('from_cache', 0),
            ('from_cache', True), ('stats_present', False), ('compilation_present', False),
            ('phase', 'startup'), ('phase', 1), ('server_compilation_cpu_us', 0),
            ('server_compilation_cpu_us', -1), ('server_compilation_cpu_us', 2 ** 64),
            ('server_compilation_cpu_us', True), ('server_compilation_duration_us', 0),
            ('server_compilation_duration_us', 1.5), ('result_set_count', 0), ('result_set_count', 2))]
        bad.extend([{}, [], None, dict(sample, unknown=True)])
        for record in bad:
            try:
                module.parse_measurement(json.dumps(record).encode(), 0)
            except metadata.MetadataError:
                check(True)
            else:
                check(False)
        for payload, code in ((b' ' * 4097, 0), (b'{"a":1,"a":2}', 0), (b'bad', 0),
                              (json.dumps(sample).encode(), True), (json.dumps(sample).encode(), 2),
                              (json.dumps(sample).encode(), -9), (json.dumps(sample).encode(), 1)):
            try:
                module.parse_measurement(payload, code)
            except metadata.MetadataError:
                check(True)
            else:
                check(False)
        receipt_path = local / 'fake-receipt.json'
        receipt = dict(metadata=dict(profile='rbo', compile_argv=['compile'], link_argv=['link'],
                                    compile_cwd=str(local), link_cwd=str(local),
                                    compile_environment={}, link_environment={}),
                       build=dict(target='ydb/core/kqp/ut/rbo'), files={})
        args = SimpleNamespace(command='measure-compilation', bug='Q28', phase='fixed',
                               worktree=local, sample_index=0, receipt=receipt_path,
                               compile_timeout=3, link_timeout=4, native_timeout=5)
        scenario_summary = {}
        scenarios = ('valid', 'cold_failure', 'compile_guard', 'compile_env', 'link_guard', 'link_env',
                     'native_guard', 'report_oversize', 'input_mutated', 'executable_mutated',
                     'receipt_mutated', 'profile_mutated', 'wrong_profile', 'wrong_target')
        for scenario in scenarios:
            values = json.loads(json.dumps(receipt))
            if scenario == 'wrong_profile':
                values['metadata']['profile'] = 'ast'
            if scenario == 'wrong_target':
                values['build']['target'] = 'invalid'
            receipt_path.write_text(json.dumps(values))
            fds, closed, calls, receipts = {}, [], [], []

            def memfd(name, data=b'', executable=False):
                fd = len(fds) + 100
                fds[fd] = bytes(data)
                return fd

            def fd_digest(fd):
                return dict(sha256=digest(fds[fd]), size_bytes=len(fds[fd]))

            def mocked_run(argv, cwd, mapping, **options):
                stage = argv[0] if argv[0] in ('compile', 'link') else 'native'
                calls.append((stage, dict(options)))
                record = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False,
                              stdout_overflow=False, environment_sha256=direct._json_digest({}))
                if stage in ('compile', 'link'):
                    fds[mapping[options['stdout_fd']]] = stage.encode()
                    if scenario == stage + '_guard':
                        record['cleanup_complete'] = False
                    if scenario == stage + '_env':
                        record['environment_sha256'] = '0' * 64
                else:
                    observed = dict(sample)
                    if scenario == 'cold_failure':
                        observed.update(completed=False, phase='cold_sample', from_cache=True)
                        record['exit_code'] = 1
                    fds[mapping['report']] = json.dumps(observed).encode()
                    if scenario == 'native_guard':
                        record['watchdog_stopped'] = True
                    if scenario == 'report_oversize':
                        fds[mapping['report']] = b' ' * 4097
                    if scenario == 'input_mutated':
                        fds[mapping['query']] = b'changed'
                    if scenario == 'executable_mutated':
                        fds[mapping['executable']] = b'changed'
                    if scenario == 'receipt_mutated':
                        receipt_path.write_text('{}')
                return record, b''

            fingerprints = iter(['a' * 64, ('b' if scenario == 'profile_mutated' else 'a') * 64])
            with ExitStack() as stack:
                for obj, key, value in (
                    (direct, '_expected_revision', lambda *unused: 'f' * 40),
                    (metadata, 'verify_receipt', lambda *call: receipts.append(call)),
                    (pairing, 'profile_fingerprint', lambda *unused: next(fingerprints)),
                    (direct, '_memfd', memfd), (direct, '_fd_digest', fd_digest),
                    (direct, '_stdout_link_arguments', lambda argv: (argv, {})),
                    (direct, '_archive_response_arguments', lambda argv, *unused: (argv, {'responses': []})),
                    (direct, 'run_anonymous', mocked_run),
                    (module.os, 'fchmod', lambda *unused: None),
                    (module.os, 'close', lambda fd: closed.append(fd)),
                    (module.os, 'pread', lambda fd, size, offset: fds[fd][offset:offset + size]),
                ):
                    stack.enter_context(patch.object(obj, key, value))
                try:
                    evidence, status = module.execute_measurements(args)
                except metadata.MetadataError:
                    check(scenario not in ('valid', 'cold_failure'))
                else:
                    check(scenario in ('valid', 'cold_failure'))
                    check(status == (0 if scenario == 'valid' else 2))
                    check(len(evidence['records']) == (8 if scenario == 'valid' else 1))
                    for key in ('full_q28_admission', 'query_semantics_verified', 'quality_improvement_verified',
                                'optimizer_stage_timings_verified', 'structural_query_scaling_measured'):
                        check(evidence[key] is False)
                    check(evidence['closure_receipt_sha256'] == digest(receipt_path.read_bytes()))
                    check(evidence['adapter_sha256'] == digest(Path(module.__file__).read_bytes()))
            check(len(closed) == len(fds) and len(set(closed)) == len(fds))
            if calls:
                check(calls[0][0] == 'compile' and calls[0][1]['timeout'] == 3)
            for stage, options in calls:
                if stage == 'link':
                    check(options['max_scope_gib'] == 36 and options['timeout'] == 4)
                if stage == 'native':
                    check(options['max_scope_gib'] == 12 and options['timeout'] == 5)
                    check(options['compiler_environment'] is False and options['retain_stdout'] == 0)
            scenario_summary[scenario] = dict(mock_process_calls=len(calls), mocked_descriptors_closed=len(closed))
        report = dict(status='independent_metadata_only_passed', assertions=CHECKS,
                      patch_sha256=EXPECTED, applied30_plan_sha256=digest(plan.read_bytes()),
                      scenarios=scenario_summary, rendered_inputs_persisted=False,
                      native_runs=0, compiler_runs=0, link_runs=0, build_runs=0,
                      source=Path(__file__).name, source_sha256=digest(Path(__file__).read_bytes()))
        output = ROOT / 'verification/Q28-sdk-compilation-independent-metadata-20261028.json'
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
        print(json.dumps(dict(assertions=CHECKS, report_sha256=digest(output.read_bytes()))))


if __name__ == '__main__':
    main()
