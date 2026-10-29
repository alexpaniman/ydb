"""Verify independent branches using anonymous inputs and sanitized evidence."""
import argparse
import hashlib
import importlib
import importlib.util
import inspect
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REVIEW = HERE.parent / 'rbo-review-20261021'
sys.path.insert(0, str(REVIEW))
import generate_and_verify as g
import correlation_oracle_generator as correlation
import partial_lookup_key_generator as lookup
from verification_contract import failure_stage, parse_rows_output
from optimizer_mode_contract import optimizer_mode_configuration
from binary_provenance import ProvenanceError, unchanged_since_verification, verify_build_receipt


def canonical(rows):
    return sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in rows)


def import_generator_snapshot(name):
    # Compile exactly the bytes we fingerprint. Artifact generators may be
    # improved while another native worker still owns an earlier snapshot.
    spec = importlib.util.find_spec(name)
    source = Path(spec.origin).read_bytes()
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    exec(compile(source, spec.origin, 'exec'), module.__dict__)
    module._verification_source_sha256 = hashlib.sha256(source).hexdigest()
    return module


def config_with(settings, values):
    for key, value in values.items():
        if key == 'EnableSpillingNodes' and isinstance(value, str):
            value = json.dumps(value.strip('"'))
        else:
            value = str(value).lower() if isinstance(value, bool) else str(value)
        pattern = r'(?m)^\s*' + re.escape(key) + r':\s*[^\n]+'
        if re.search(pattern, settings):
            settings = re.sub(pattern, '\n ' + key + ': ' + value, settings)
        else:
            settings = settings.replace('TableServiceConfig {', 'TableServiceConfig {\n ' + key + ': ' + value, 1)
    return settings


def module_metadata(module, case, mode, n):
    if hasattr(module, 'metadata'):
        if 'mode' in inspect.signature(module.metadata).parameters:
            return module.metadata(case, n, mode=mode)
        return module.metadata(case, n)
    if hasattr(module, 'requirements'):
        return module.requirements(case, mode)
    if hasattr(module, 'CONFIG_NEEDS'):
        c = module.CONFIG_NEEDS
        return {'table_service_overrides': c['table_service'], 'pragmas': list(c['pragmas'].items()), 'node_count': c['nodes']}
    return {}


def run_generated(module, case, mode, n, *, measurement_identity=None, spill_scope=None):
    meta = module_metadata(module, case, mode, n)
    from quality_spill_bounds import bound_quality_spill_metadata
    meta = bound_quality_spill_metadata(module, case, mode, n, meta, scope=spill_scope, runner=g.BINARY)
    if measurement_identity is not None:
        if (module.__name__ != 'quality_q01_scaling_checks' or mode != 'new'
                or meta.get('q01_resource_scaling_cell') != module.cell(case, n)
                or not getattr(module, '_verification_source_sha256', None)):
            raise ValueError('Invalid Q01 measurement generator or mode')
        dependency_bindings = module.source_bindings()
    empty_result_witness = meta.get('c06_c50_empty_result_witness')
    if empty_result_witness is not None:
        if (empty_result_witness != 'c06_c50_empty_result_metadata_v1'
            or module.__name__ != 'c06_c50_bounded_integration_checks'
            or case not in ('c50_left_anti', 'c50_left_anti_same_type_control')
            or mode != 'new' or type(n) is not int or n != 24
            or not getattr(module, '_verification_source_sha256', None)):
            raise ValueError('Unreviewed integration empty-result witness request')
    q02_empty_witness = meta.get('q02_empty_result_witness')
    if q02_empty_witness is not None:
        if (empty_result_witness is not None
            or q02_empty_witness != 'q02_empty_result_metadata_v1'
            or module.__name__ != 'quality_window_checks' or case != 'q02_empty'
            or mode != 'new' or type(n) is not int or n != 24
            or not getattr(module, '_verification_source_sha256', None)):
            raise ValueError('Unreviewed Q02 empty-result witness request')
        empty_result_witness = q02_empty_witness
    query = module.recipes(n)[case]
    pragmas = dict(meta.get('pragmas', []))
    pragmas['YqlSelect'] = 'force' if mode == 'new' else 'disable'
    syntax = meta.get('query_syntax', 'yql')
    if syntax == 'pg':
        if meta.get('pragmas') or meta.get('flag_pragmas'):
            raise ValueError('Pg targets require configuration overrides instead of YQL pragmas')
        query = '--!syntax_pg\n' + query + ';'
    elif syntax == 'yql':
        query = ''.join('PRAGMA ' + key + '=' + g.quote(value) + '; ' for key, value in pragmas.items()) + ''.join(
            'PRAGMA ' + key + '; ' for key in meta.get('flag_pragmas', [])) + query + ';'
    else:
        raise ValueError('Unknown query syntax')
    validations = module.validation_queries(case, n) if hasattr(module, 'validation_queries') else []
    if len(validations) > 1:
        raise ValueError('The runner exposes only the final validation request')
    if empty_result_witness is not None and validations:
        raise ValueError('Empty-result metadata witness requires the original single target')
    if validations and not hasattr(module, 'check_validation_results'):
        raise ValueError('Validation requests require a result-set oracle')
    validations = ['PRAGMA YqlSelect=' + g.quote(pragmas['YqlSelect']) + '; ' + statement + ';'
                   for statement in validations]
    if module.__name__ in ('logical_checks', 'expression_checks') or getattr(module, 'CASE_SPECIFIC_SCHEMA', False):
        schema, seeds = module.schema_and_seeds(n, case)
    elif hasattr(module, 'schema_and_seeds'):
        schema, seeds = module.schema_and_seeds(n)
    else:
        schema, seeds = g.schema_and_seeds(n, case)
    settings = (REVIEW / (mode + '.conf')).read_text()
    settings = config_with(settings, meta.get('table_service_overrides', meta.get('table_service_config', {})))
    fallback_requested = meta.get('table_service_overrides', meta.get('table_service_config', {})).get('EnableFallbackToYqlOptimizer', False)
    mode_configuration = optimizer_mode_configuration(settings, mode, syntax, pragmas, fallback_requested)
    descriptors = []
    def memory(label, value):
        fd = g.anonymous_text(label, value)
        descriptors.append(fd)
        return '/proc/self/fd/' + str(fd)
    command = [str(g.BINARY), '-c', memory('config', settings), '-u', str(g.MATH), '-s', memory('schema', schema)]
    nodes = meta.get('node_count', 1)
    if nodes != 1:
        command += ['-N', str(nodes)]
    for seed in seeds:
        command += ['-p', memory('fixture', seed + ';')]
    command += ['-p', memory('check', query)]
    for validation in validations:
        command += ['-p', memory('validation', validation)]
    command += ['-C', 'query', '--log-file', '/dev/null', '--result-file', memory('results', '')]
    if empty_result_witness is not None:
        command += ['--result-format', 'full-json']
    result_fd = descriptors[-1]
    ast_fd = None
    correctness_ast_required = bool(meta.get('correctness_requires_physical_ast'))
    if meta.get('quality_requires_separate_plan_evidence') or correctness_ast_required:
        if not hasattr(module, 'check_physical_ast'):
            raise ValueError('Requested physical AST evidence requires a plan oracle')
        for _ in seeds:
            command += ['--script-ast-file', '/dev/null']
        command += ['--script-ast-file', memory('target-physical-ast', '')]
        ast_fd = descriptors[-1]
        for _ in validations:
            command += ['--script-ast-file', '/dev/null']
    plan_fd = None
    correctness_plan_required = bool(meta.get('correctness_requires_query_plan_evidence'))
    if meta.get('quality_requires_query_plan_evidence') or correctness_plan_required:
        if not hasattr(module, 'check_query_plan'):
            raise ValueError('Query plan evidence requires a plan oracle')
        command += ['--plan-format', 'json']
        for _ in seeds:
            command += ['--script-plan-file', '/dev/null']
        command += ['--script-plan-file', memory('target-query-plan', '')]
        plan_fd = descriptors[-1]
        for _ in validations:
            command += ['--script-plan-file', '/dev/null']
    record = {'case': case, 'mode': mode,
              'generator_module': module.__name__, 'node_count': nodes,
              'query_syntax': syntax,
              'quality_evidence_required': bool(meta.get('quality_requires_separate_plan_evidence') or meta.get('quality_requires_query_plan_evidence')),
              'correctness_query_plan_required': correctness_plan_required,
              'correctness_physical_ast_required': correctness_ast_required,
              'verification_contract_version': 'strict_results_and_request_boundaries_v1',
              'generator_version': meta.get('generator_version', 'unversioned'),
              'generator_sha256': getattr(module, '_verification_source_sha256', None),
              'generator_hash_source': 'loaded_source_snapshot' if hasattr(module, '_verification_source_sha256') else 'unavailable_direct_import',
              'settings_sha256': hashlib.sha256(settings.encode()).hexdigest(),
              'schema_sha256': hashlib.sha256(schema.encode()).hexdigest(),
              'fixture_sha256': [hashlib.sha256((statement + ';').encode()).hexdigest() for statement in seeds],
              'query_sha256': hashlib.sha256(query.encode()).hexdigest(),
              'validation_sha256': [hashlib.sha256(statement.encode()).hexdigest() for statement in validations],
              'setup_request_count': len(seeds), 'target_request_count': 1,
              'validation_request_count': len(validations), 'target_results_checked': not validations,
              'expected_rejection_configured': mode == 'new' and bool(meta.get('expected_rejection_token')),
              'table_service_overrides': meta.get('table_service_overrides', meta.get('table_service_config', {}))}
    if empty_result_witness is not None:
        record['result_output_format'] = 'full-json'
    if 'spill_payload_file_policy' in meta:
        record['spill_payload_file_policy'] = meta['spill_payload_file_policy']
    record.update(mode_configuration)
    identity = None
    try:
        if record['optimizer_mode_config_verified'] is not True:
            record.update(harness_exception='OptimizerModeConfigurationMismatch', semantic_oracle_pass=False)
            return record
        if measurement_identity is None:
            completed = subprocess.run(command, pass_fds=descriptors, capture_output=True, timeout=120,
                                       preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        else:
            if validations or nodes != 1:
                raise ValueError('Q01 measurement requires one target without validation requests')
            from q01_native_child_measurement import run_measured_kqprun
            identity = dict(measurement_identity, case=case, mode=mode, rows=n,
                            cell=module.cell(case, n), dependency_source_sha256=dependency_bindings,
                            native_driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                            workload={key: record[key] for key in ('generator_sha256', 'settings_sha256',
                                      'schema_sha256', 'fixture_sha256', 'query_sha256', 'validation_sha256')})
            record['q01_measurement_identity'] = identity
            completed, observation = run_measured_kqprun(command, descriptors, identity, timeout=120)
            record['q01_child_observation'] = observation
        record['exit_code'] = completed.returncode
        os.lseek(result_fd, 0, os.SEEK_SET)
        payload = os.read(result_fd, 16 * 1024 * 1024 + 1)
        if len(payload) > 16 * 1024 * 1024:
            raise ValueError('Result buffer exceeds the checked size limit')
        record.update(row_count=len(payload.splitlines()), result_sha256=hashlib.sha256(payload).hexdigest())
        if completed.returncode:
            diagnostics = (completed.stdout + completed.stderr).decode(errors='replace')
            record['failure_category'] = g.failure_category(diagnostics)
            record['diagnostic_source_locations'] = sorted(set(re.findall(r'(?:ydb|yql|util)/[a-zA-Z0-9_/.-]+\.(?:cpp|h):[0-9]+', diagnostics)))[:16]
            record['failure_stage'] = failure_stage(completed.stdout.decode(errors='replace'), len(seeds), len(validations))
            record['semantic_oracle_pass'] = False
            expected = getattr(module, 'EXPECTED_FAILURES', {}).get(case)
            if expected == 'scalar cardinality':
                record['expected_failure_observed'] = (
                    completed.returncode > 0
                    and record.get('failure_stage') == 'target'
                    and record['failure_category'] == 'scalar cardinality check')
                record['semantic_oracle_pass'] = record['expected_failure_observed']
            if meta.get('expected_rejection_token'):
                record['correctness_guard_observed'] = (
                    completed.returncode > 0
                    and record.get('failure_stage') == 'target'
                    and meta['expected_rejection_token'] in diagnostics)
        else:
            if empty_result_witness is not None:
                rows, result_sets, boundaries_known, witness = module.parse_empty_result_witness(case, payload, n)
                record.update(witness)
            else:
                rows, result_sets, boundaries_known = parse_rows_output(payload)
            if ast_fd is not None:
                os.lseek(ast_fd, 0, os.SEEK_SET)
                ast_payload = os.read(ast_fd, 16 * 1024 * 1024 + 1)
                if len(ast_payload) > 16 * 1024 * 1024:
                    raise ValueError('Physical AST exceeds the checked size limit')
                record.update(module.check_physical_ast(case, ast_payload.decode()))
            if plan_fd is not None:
                os.lseek(plan_fd, 0, os.SEEK_SET)
                plan_payload = os.read(plan_fd, 16 * 1024 * 1024 + 1)
                if len(plan_payload) > 16 * 1024 * 1024:
                    raise ValueError('Query plan exceeds the checked size limit')
                record.update(module.check_query_plan(case, plan_payload.decode(), n))
            record.update(row_count=len(rows), result_set_count=len(result_sets),
                          result_set_boundaries_known=boundaries_known)
            record['unordered_result_sha256'] = hashlib.sha256('\n'.join(canonical(rows)).encode()).hexdigest()
            if validations and hasattr(module, 'check_validation_results'):
                verdict = module.check_validation_results(case, result_sets, n)
            elif hasattr(module, 'check_rows'):
                verdict = module.check_rows(case, rows, n)
            elif hasattr(module, 'semantic_oracle'):
                verdict = module.semantic_oracle(case, rows, n)
            else:
                verdict = canonical(rows) == canonical(module.oracles(n)[case])
            record.update(verdict if isinstance(verdict, dict) else {'semantic_oracle_pass': bool(verdict)})
            if not validations and hasattr(module, 'check_result_sets'):
                verdict = module.check_result_sets(case, result_sets, n)
                if isinstance(verdict, dict):
                    semantic = record.get('semantic_oracle_pass')
                    record.update(verdict)
                    if semantic is not None:
                        record['semantic_oracle_pass'] = bool(semantic and record.get('semantic_oracle_pass'))
                else:
                    record['result_set_oracle_pass'] = bool(verdict)
    except subprocess.TimeoutExpired as exc:
        record.update(timeout=True, semantic_oracle_pass=False)
        if measurement_identity is not None:
            record['q01_child_observation'] = getattr(exc, 'q01_child_observation', {'status': 'unavailable'})
    except Exception as exc:
        record.update(harness_exception=type(exc).__name__, semantic_oracle_pass=False)
        if measurement_identity is not None:
            record['q01_child_observation'] = getattr(exc, 'q01_child_observation', {'status': 'unavailable'})
    finally:
        for fd in descriptors:
            os.close(fd)
    if measurement_identity is not None and identity is not None:
        try:
            unchanged = (module.source_bindings() == dependency_bindings
                and hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest() == record['generator_sha256']
                and hashlib.sha256(Path(__file__).read_bytes()).hexdigest() == identity['native_driver_sha256'])
        except OSError:
            unchanged = False
        if not unchanged:
            record['q01_child_observation']['status'] = 'source_binding_rejected'
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--worktree', type=Path, required=True)
    parser.add_argument('--phase', choices=['baseline', 'fixed'], required=True)
    parser.add_argument('--bugs', nargs='+', required=True)
    parser.add_argument('--cases', nargs='*')
    parser.add_argument('--rows', type=int, default=32)
    parser.add_argument('--modes', nargs='+', choices=['new', 'legacy'], default=['new', 'legacy'])
    parser.add_argument('--repetitions', type=int)
    parser.add_argument('--generator-module', action='append', default=[])
    parser.add_argument('--build-receipt', type=Path)
    parser.add_argument('--measure-q01-child-resources', action='store_true')
    parser.add_argument('--q01-run-id')
    parser.add_argument('--q01-sample-kind', choices=['pilot', 'warmup', 'measurement'])
    parser.add_argument('--q01-sample-index', type=int)
    args = parser.parse_args()
    measurement_identity = None
    if args.measure_q01_child_resources:
        from quality_q01_scaling_checks import validate_sample
        if (args.bugs != ['Q01'] or args.modes != ['new'] or args.repetitions != 1
                or args.generator_module != ['quality_q01_scaling_checks']
                or not args.cases or len(args.cases) != 1):
            raise ValueError('Q01 measurement requires exactly one cell, mode and repetition')
        measurement_identity = validate_sample({'case': args.cases[0], 'rows': args.rows,
            'run_id': args.q01_run_id, 'kind': args.q01_sample_kind, 'index': args.q01_sample_index})
    elif any(value is not None for value in (args.q01_run_id, args.q01_sample_kind, args.q01_sample_index)):
        raise ValueError('Q01 sample coordinates require measurement opt-in')
    g.BINARY = args.worktree / 'ydb/tests/tools/kqprun/kqprun'
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=args.worktree, text=True).strip()
    preflight_path = HERE / 'verification' / ('preflight-' + args.phase + '-' + revision[:12] + '.jsonl')
    try:
        provenance = verify_build_receipt(args.worktree, revision, args.build_receipt)
    except (ProvenanceError, OSError, ValueError, subprocess.SubprocessError) as exc:
        failure = {'purpose': 'binary_provenance_preflight', 'phase': args.phase,
                   'revision': revision, 'binary_provenance_verified': False,
                   'failure_reason': str(exc) if isinstance(exc, ProvenanceError) else type(exc).__name__}
        with preflight_path.open('a') as stream:
            stream.write(json.dumps(failure, sort_keys=True) + '\n')
        print(json.dumps(failure, sort_keys=True), flush=True)
        raise SystemExit(3)

    from quality_spill_bounds import validated_quality_spill_scope
    spill_scope = validated_quality_spill_scope(args.bugs, args.phase, revision, provenance)

    if measurement_identity is not None:
        measurement_identity.update(phase=args.phase, revision=revision,
            executable_sha256=provenance['binary_provenance']['sha256'],
            build_receipt_sha256=provenance['build_receipt_sha256'],
            resource_design_sha256=hashlib.sha256((HERE / 'verification' /
                'Q01-resource-scaling-admission-plan-20261027.json').read_bytes()).hexdigest())

    def attach_provenance(record):
        record.update(provenance)
        record['binary_provenance_required'] = True
        if not unchanged_since_verification(args.worktree, revision, provenance):
            record.update(binary_provenance_verified=False,
                          provenance_failure_reason='runner_or_sources_changed_during_verification')
        if measurement_identity is not None and record.get('binary_provenance_verified') is not True:
            if 'q01_child_observation' in record:
                record['q01_child_observation']['status'] = 'binary_provenance_rejected'
        return record

    module_names = ['logical_checks', 'window_checks', 'metadata_checks', 'aggregate_checks', 'subsystem_checks', 'aggregate_window_checks', 'write_checks', 'join_key_checks', 'access_effect_checks', 'correlation_controls', 'distribution_checks', 'sharing_lifecycle_checks']
    if measurement_identity is not None:
        # Avoid constructing every unrelated correctness recipe at scaling N.
        module_names = []
    if measurement_identity is None and (HERE / 'expression_checks.py').exists():
        module_names.append('expression_checks')
    for module in args.generator_module:
        if not re.fullmatch(r'[a-z][a-z0-9_]*', module) or not (HERE / (module + '.py')).is_file():
            raise ValueError('Extra generator must be a local artifact module')
        if module not in module_names:
            module_names.append(module)
    modules = [import_generator_snapshot(name) for name in module_names]
    available = {case: module for module in modules for case in module.recipes(args.rows)}
    # A known legacy scalar control checks local server startup, configuration,
    # UDF loading and result decoding before a batch can accumulate misleading
    # failures. It is kept outside per-bug evidence and has no rendered artifact.
    preflight_module = (import_generator_snapshot('subsystem_checks') if measurement_identity is not None
                        else available['c29_one_arm_control'])
    preflight_rows = 32 if measurement_identity is not None else args.rows
    preflight = run_generated(preflight_module, 'c29_one_arm_control', 'legacy', preflight_rows)
    preflight.update(purpose='runtime_preflight', phase=args.phase, revision=revision, rows=preflight_rows)
    attach_provenance(preflight)
    with preflight_path.open('a') as stream:
        stream.write(json.dumps(preflight, sort_keys=True) + '\n')
    if (preflight.get('exit_code') != 0 or preflight.get('semantic_oracle_pass') is not True
            or preflight.get('binary_provenance_verified') is not True):
        print(json.dumps(preflight, sort_keys=True), flush=True)
        raise SystemExit(2)
    for bug in args.bugs:
        selected = [case for case in available if case.startswith(bug.lower() + '_') and (not args.cases or case in args.cases)]
        path = HERE / 'verification' / (args.phase + '-' + bug + '-' + revision[:12] + '.jsonl')
        def save(record):
            record.update(bug=bug, phase=args.phase, revision=revision, rows=args.rows)
            attach_provenance(record)
            line = json.dumps(record, sort_keys=True)
            with path.open('a') as stream:
                stream.write(line + '\n')
            print(line, flush=True)
            if record.get('binary_provenance_verified') is not True:
                raise SystemExit(3)
        for case in selected:
            module = available[case]
            repeats = args.repetitions or module_metadata(module, case, 'new', args.rows).get('repetitions', 1)
            for mode in args.modes:
                for repeat in range(repeats):
                    record = run_generated(module, case, mode, args.rows,
                                           measurement_identity=measurement_identity, spill_scope=spill_scope)
                    record['repetition'] = repeat
                    save(record)
        if bug in ('C05', 'C06', 'C07', 'C08'):
            correlation_cases = {
                'C05': ('demand_extra_domains', 'demand_requested_only_control'),
                'C06': ('typed_capture_collision', 'typed_same_type_control'),
                'C07': ('nan_domain_equality', 'nan_finite_control'),
                'C08': ('empty_scalar_projection', 'empty_count_restore_control'),
            }
            for case in correlation_cases[bug]:
                for mode in args.modes:
                    save(correlation.run(case, mode, args.rows))
        elif bug == 'C49':
            for full_key in (False, True):
                for mode in args.modes:
                    save(lookup.run(mode, max(4, args.rows // 4), full_key, 'LEFT'))
        elif not selected:
            raise ValueError('No generator registered for ' + bug)


if __name__ == '__main__':
    main()
