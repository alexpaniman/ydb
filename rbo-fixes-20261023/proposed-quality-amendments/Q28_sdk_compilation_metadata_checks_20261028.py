"""Prepare/check a dormant Q28 pilot using only Python and in-memory inputs."""
import ast
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import types

ROOT = Path(__file__).resolve().parents[1]
PROPOSED = Path(__file__).resolve().parent
PLAN = ROOT / 'verification/idle-artifact-application-plan-20261028-latest.json'
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]


def stacked_sources():
    plan = json.loads(PLAN.read_text())
    virtual = {}
    for record in plan['patches']:
        data = (ROOT / record['patch']).read_bytes()
        assert hashlib.sha256(data).hexdigest() == record['sha256']
        lines, index = data.decode().splitlines(keepends=True), 0
        while index < len(lines):
            if not lines[index].startswith('--- '):
                index += 1
                continue
            old, new = lines[index][4:].strip(), lines[index + 1][4:].strip()
            index += 2
            path = new.removeprefix('b/').removeprefix('rbo-fixes-20261023/')
            content = virtual[path] if path in virtual else [] if old == '/dev/null' else (ROOT / path).read_text().splitlines(keepends=True)
            cursor = 0
            while index < len(lines) and lines[index].startswith('@@'):
                index += 1
                before, after = [], []
                while index < len(lines) and not lines[index].startswith(('@@', '--- ', 'diff --git ')):
                    line = lines[index]
                    index += 1
                    if line.startswith(' '):
                        before.append(line[1:]); after.append(line[1:])
                    elif line.startswith('-'):
                        before.append(line[1:])
                    elif line.startswith('+'):
                        after.append(line[1:])
                    elif line.startswith('\\'):
                        continue
                    else:
                        break
                matches = [at for at in range(cursor, len(content) + 1) if content[at:at + len(before)] == before] if before else [cursor]
                assert len(matches) == 1, 'Dormant patch context is ambiguous'
                at = matches[0]
                content = content[:at] + after + content[at + len(before):]
                cursor = at + len(after)
            virtual[path] = content
    result = {name: ''.join(lines) for name, lines in virtual.items()}
    for name, expected in plan['files'].items():
        if name in result:
            assert hashlib.sha256(result[name].encode()).hexdigest() == expected['after_sha256']
    return plan, result


def main():
    plan, virtual = stacked_sources()
    before = virtual['anonymous_cpp_verify.py']
    after = before
    replacements = [
        ("    for action in ('prepare', 'export-graph', 'capture', 'run'):",
         "    for action in ('prepare', 'export-graph', 'capture', 'run', 'measure-compilation'):"),
        ("            child.add_argument('--native-timeout', type=int, default=120)",
         "            child.add_argument('--native-timeout', type=int, default=120)\n            if action == 'measure-compilation':\n                child.add_argument('--sample-index', type=int, default=0)"),
        ("                else:\n                    evidence, code = execute(args)",
         "                elif args.command == 'measure-compilation':\n                    from q28_compilation_measurement import execute_measurements\n                    evidence, code = execute_measurements(args)\n                else:\n                    evidence, code = execute(args)")]
    for old, new in replacements:
        assert after.count(old) == 1
        after = after.replace(old, new)
    compile(after, 'anonymous_cpp_verify.py', 'exec')
    original = {node.name: ast.dump(node) for node in ast.parse(before).body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}
    updated = {node.name: ast.dump(node) for node in ast.parse(after).body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}
    assert {key: value for key, value in original.items() if key != 'main'} == {key: value for key, value in updated.items() if key != 'main'}
    direct_path = PROPOSED / 'anonymous_cpp_verify_Q28_compilation_20261028.candidate.py'
    direct_path.write_text(after)
    module_path = PROPOSED / 'q28_compilation_measurement_20261028.candidate.py'
    module = types.ModuleType('q28_compilation_measurement')
    module.__file__ = str(module_path)
    exec(compile(module_path.read_bytes(), str(module_path), 'exec'), module.__dict__)
    # The profile snapshot loader reads the stacked source from this memfd.
    fd = os.memfd_create('q28-profile-source-check', os.MFD_CLOEXEC)
    os.write(fd, virtual['quality_scheduler_semantic_checks.py'].encode())
    semantic = types.ModuleType('quality_scheduler_semantic_checks')
    semantic.__spec__ = importlib.util.spec_from_loader(semantic.__name__, loader=None, origin='/proc/self/fd/' + str(fd))
    sys.modules[semantic.__name__] = semantic
    for name in ('anonymous_cpp_metadata', 'verification_contract', 'binary_provenance', 'native_verify', 'quality_pairing'):
        helper = types.ModuleType(name)
        helper.__file__ = str(ROOT / (name + '.py'))
        sys.modules[name] = helper
        exec(compile(virtual[name + '.py'], helper.__file__, 'exec'), helper.__dict__)
    direct = types.ModuleType('anonymous_cpp_verify')
    direct.__file__ = str(ROOT / 'anonymous_cpp_verify.py')
    sys.modules[direct.__name__] = direct
    exec(compile(after, direct.__file__, 'exec'), direct.__dict__)
    checks = 0
    def check(condition):
        nonlocal checks
        if condition is not True:
            raise AssertionError('Q28 source check failed at ordinal ' + str(checks + 1))
        checks += 1
    try:
        payloads = module._snapshot_inputs()
        check(tuple(item[1]['case'] for item in payloads) == module.CASES)
        descriptors = []
        for payload, record in payloads:
            check(record['optimizer_mode_config_verified'])
            check(record['node_count'] == 1 and record['rows'] == 24)
            check(record['requested_stats_mode'] == 'Basic' and record['requested_exec_mode'] == 'Execute')
            check(record['query_structure']['fixture_rows_do_not_scale_compile_graph'])
            check(record['query_structure']['select_tokens'] > 0)
            check(1 <= len(payload) - 3 <= 3)
            check(all(hashlib.sha256(value).hexdigest() == record['input_sha256'][key] for key, value in payload.items()))
            descriptors.append(record)
        source = module.render_cpp().encode()
        check(source == module.render_cpp().encode())
        check(b'GetCompilation()' in source and b'GetDurationUs()' in source and b'GetCpuTimeUs()' in source)
        check(b'Check(!cached && duration > 0 && cpu > 0 && resultSets == 1)' in source)
        check(all(payload['query'] not in source and payload['schema'] not in source for payload, _ in payloads))
        check('quality_scheduler_cpp_checks' == direct.GENERATORS['Q28'])
        from quality_scheduler_cpp_checks import cases
        check(len(cases()) == 26)
        sample = dict(completed=True, phase='complete', stats_present=True, compilation_present=True,
                      from_cache=False, server_compilation_duration_us=1000, server_compilation_cpu_us=100,
                      result_set_count=1)
        parsed = module.parse_measurement(json.dumps(sample).encode(), 0)
        check(parsed['measurement_valid'])
        for field in ('query_semantics_verified', 'optimizer_stage_timings_verified', 'quality_improvement_verified'):
            check(parsed[field] is False)
        from anonymous_cpp_metadata import MetadataError
        bad_samples = [dict(sample, **{field: value}) for field, value in (
            ('from_cache', True), ('stats_present', False), ('compilation_present', False),
            ('server_compilation_duration_us', 0), ('server_compilation_cpu_us', 0),
            ('server_compilation_cpu_us', True), ('server_compilation_cpu_us', -1),
            ('server_compilation_duration_us', 2 ** 64), ('result_set_count', 2), ('phase', 'unknown'))]
        bad_samples.extend([dict(sample, unexpected=True), None, []])
        for bad in bad_samples:
            try:
                module.parse_measurement(json.dumps(bad).encode(), 0)
            except MetadataError:
                check(True)
            else:
                check(False)
        for payload, exit_code in ((b'', 0), (b'{"completed":true,"completed":false}', 0),
                                   (json.dumps(sample).encode(), True), (json.dumps(sample).encode(), 1)):
            try:
                module.parse_measurement(payload, exit_code)
            except MetadataError:
                check(True)
            else:
                check(False)
        cold_failure = dict(sample, completed=False, phase='cold_sample', from_cache=True)
        check(module.parse_measurement(json.dumps(cold_failure).encode(), 1)['measurement_valid'] is False)
        # A pilot schema is never a direct-rule admission record.
        try:
            direct.pair_direct('Q28', {'schema_version': module.VERSION}, {'schema_version': module.VERSION},
                               json.loads((ROOT / 'cases/Q28.json').read_text()))
        except (MetadataError, KeyError):
            check(True)
        else:
            check(False)
        patch_path = PROPOSED / 'Q28-sdk-compilation-pilot-20261028.patch'
        patch_path.write_text(''.join(difflib.unified_diff(before.splitlines(keepends=True), after.splitlines(keepends=True),
            fromfile='a/rbo-fixes-20261023/anonymous_cpp_verify.py', tofile='b/rbo-fixes-20261023/anonymous_cpp_verify.py')) +
            ''.join(difflib.unified_diff([], module_path.read_text().splitlines(keepends=True),
                fromfile='/dev/null', tofile='b/rbo-fixes-20261023/q28_compilation_measurement.py')))
        result = dict(status='dormant_source_prepared_native_pending', metadata_checks_passed=checks,
            patch=str(patch_path.relative_to(ROOT)), patch_sha256=hashlib.sha256(patch_path.read_bytes()).hexdigest(),
            module_sha256=hashlib.sha256(module_path.read_bytes()).hexdigest(),
            helper_before_sha256=hashlib.sha256(before.encode()).hexdigest(),
            helper_after_sha256=hashlib.sha256(after.encode()).hexdigest(),
            source_checks_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            apply_after=str(PLAN.relative_to(ROOT)), stack_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(),
            stack_patch_count=len(plan['patches']), direct_functions_unchanged_except_command_dispatch=True,
            direct_q28_case_count=26, pilot_case_vector=list(module.CASES), input_descriptors=descriptors,
            generated_cpp_sha256=hashlib.sha256(source).hexdigest(), generated_cpp_bytes=len(source),
            rendered_inputs_persisted=False, native_runs=0, build_runs=0, graph_exports=0, toolchain_runs=0,
            live_helper_edits=False, pilot=True, full_q28_admission=False,
            structural_query_scaling_measured=False, query_semantics_verified=False,
            quality_improvement_verified=False, optimizer_stage_timings_verified=False,
            limits=['API compatibility and compiler/link/native execution remain unverified.',
                    'Eight existing semantic query shapes provide a transport/measurement pilot; fixture row counts do not scale compilation structure.',
                    'Final Q28 optimization assessment still requires structural query-size cells plus direct scheduler work counters and separate end-to-end semantics.',
                    'Timings are server-reported compilation wall/CPU, with parse/queue/scoping limits recorded in the prior API report; they are not pure optimizer or per-stage timers.',
                    'No quality admission or automatic timing comparison is added.',
                    'TKikimrRunner defaults differ from kqprun; both phases must use this same runner and configuration, not mix their timing samples.'])
        report = ROOT / 'verification/Q28-sdk-compilation-pilot-source-preparation-20261028.json'
        report.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
        print(json.dumps({key: result[key] for key in ('metadata_checks_passed', 'patch_sha256', 'module_sha256',
            'helper_before_sha256', 'helper_after_sha256', 'generated_cpp_sha256', 'stack_patch_count')}))
        print(json.dumps({'report_sha256': hashlib.sha256(report.read_bytes()).hexdigest()}))
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
