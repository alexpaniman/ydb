"""Strict empty-metadata and mocked native-driver checks; no native execution."""
import ast
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROJECT = ROOT.parent
REVISION = '2bc207a991cc036e1a74616d7ea3e7df6137d8dd'
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(PROJECT / 'rbo-review-20261021'))


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path, name=None):
    module = ModuleType(name or path.stem)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    module._verification_source_sha256 = sha(path.read_bytes())
    return module


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    images = json.loads((HERE / 'source-images.json').read_bytes())
    for name in images['before']:
        check(sha((ROOT / name).read_bytes()) == images['before'][name], 'live_before_image:' + name)
        check(sha((HERE / name).read_bytes()) == images['after'][name], 'candidate_after_image:' + name)
    original = load(ROOT / 'join_key_checks.py')
    wrapper = load(HERE / 'c06_c50_bounded_integration_checks.py')
    old_wrapper = load(ROOT / 'c06_c50_bounded_integration_checks.py')
    native = load(HERE / 'native_verify.py')
    native.HERE, native.REVIEW = ROOT, PROJECT / 'rbo-review-20261021'
    runner = load(HERE / 'c06_c50_integration_runner.py')
    wrapper._cell = lambda case, n, mode='new': original
    old_wrapper._cell = lambda case, n, mode='new': original
    check(wrapper.RECORDS == old_wrapper.RECORDS and wrapper.CASES == old_wrapper.CASES,
          'exact_original16_vector_and_all_fingerprint_slots_unchanged')
    for case in wrapper.CASES:
        new_meta, old_meta = wrapper.metadata(case, 24), old_wrapper.metadata(case, 24)
        check(new_meta.pop('generator_version') == wrapper.VERSION, 'new_transport_version:' + case)
        old_meta.pop('generator_version')
        witness = new_meta.pop('c06_c50_empty_result_witness', None)
        check(new_meta == old_meta, 'all_existing_configuration_metadata_unchanged:' + case)
        check((witness == wrapper.EMPTY_WITNESS_VERSION) is (case in wrapper.EMPTY_WITNESS_CASES),
              'only_two_case_families_request_metadata:' + case)
    check(all(original.expected_rows(case, 24) == [] for case in wrapper.EMPTY_WITNESS_CASES),
          'unchanged_independent_oracle_expects_empty')
    valid = {'columns': [{'name': 'li', 'type': {'type_id': 3}}]}
    positives = [json.dumps(valid).encode(), json.dumps(dict(valid, format=1)).encode()]
    for case in wrapper.EMPTY_WITNESS_CASES:
        for index, payload in enumerate(positives):
            rows, result_sets, known, proof = wrapper.parse_empty_result_witness(case, payload, 24)
            check(rows == [] and result_sets == [[]] and known is True,
                  'actual_nonempty_metadata_object_supplies_one_empty_set:' + case + ':' + str(index))
            check(proof['empty_result_set_payload_sha256'] == sha(payload) and proof['empty_result_set_metadata_verified'] is True,
                  'raw_payload_hash_and_witness_bound:' + case + ':' + str(index))
    negatives = {'empty': b'', 'whitespace': b' \n', 'missing_set': b'{}', 'array': b'[]',
                 'multiple_sets': positives[0] + b'\n' + positives[0],
                 'heading': b'Result set 1:\n' + positives[0],
                 'oversize': b' ' * (16 * 1024 + 1), 'nonbytes': positives[0].decode()}
    for label, value in (
        ('no_columns', {}), ('empty_columns', {'columns': []}),
        ('duplicate_columns', {'columns': valid['columns'] * 2}),
        ('wrong_name', {'columns': [{'name': 'other', 'type': {'type_id': 3}}]}),
        ('wrong_type', {'columns': [{'name': 'li', 'type': {'type_id': 4}}]}),
        ('floating_type_id', {'columns': [{'name': 'li', 'type': {'type_id': 3.0}}]}),
        ('boolean_type_id', {'columns': [{'name': 'li', 'type': {'type_id': True}}]}),
        ('optional_type', {'columns': [{'name': 'li', 'type': {'optional_type': {'item': {'type_id': 3}}}}]}),
        ('unknown_column_metadata', {'columns': [dict(valid['columns'][0], extra=1)]}),
        ('explicit_rows_even_empty', dict(valid, rows=[])),
        ('nonempty_rows', dict(valid, rows=[{}])),
        ('truncated', dict(valid, truncated=True)), ('explicit_false_truncation', dict(valid, truncated=False)),
        ('arrow_format', dict(valid, format=2)), ('bool_format', dict(valid, format=True)),
        ('float_format', dict(valid, format=1.0)), ('unknown_format', dict(valid, format=0)),
        ('arrow_metadata', dict(valid, arrow_format_meta={})), ('binary_data', dict(valid, data=''))):
        negatives[label] = json.dumps(value).encode()
    negatives['duplicate_root_key'] = b'{"columns":[],"columns":' + json.dumps(valid['columns']).encode() + b'}'
    negatives['duplicate_nested_key'] = positives[0].replace(b'"type_id": 3', b'"type_id": 3,"type_id": 3')
    for label, payload in negatives.items():
        try:
            wrapper.parse_empty_result_witness(wrapper.EMPTY_WITNESS_CASES[0], payload, 24)
        except (ValueError, TypeError, KeyError, IndexError):
            check(True, 'rejects_unproved_metadata:' + label)
        else:
            check(False, 'rejects_unproved_metadata:' + label)
    for case in set(wrapper.CASES) - set(wrapper.EMPTY_WITNESS_CASES):
        try:
            wrapper.parse_empty_result_witness(case, positives[0], 24)
        except ValueError:
            check(True, 'witness_rejects_unrelated_case:' + case)
        else:
            check(False, 'witness_rejects_unrelated_case:' + case)
    # Exercise the real run_generated function with fake descriptors and a fake
    # process only. All generated queries and rows remain in these dictionaries.
    outputs, commands = {}, {}
    def run_case(case, payload_override=None, exit_code=0, module=wrapper):
        descriptors, closed = {}, []
        def memory(label, value):
            fd = 1000 + len(descriptors)
            descriptors[fd] = {'label': label, 'payload': value.encode() if isinstance(value, str) else value}
            return fd
        def invoke(command, **kwargs):
            commands[case] = list(command)
            is_empty = case in wrapper.EMPTY_WITNESS_CASES
            expected_rows = original.expected_rows(case, 24)
            payload = positives[0] if is_empty else b'\n'.join(json.dumps(x).encode() for x in expected_rows)
            if payload_override is not None:
                payload = payload_override
            for value in descriptors.values():
                if value['label'] == 'results':
                    value['payload'] = payload
            return SimpleNamespace(returncode=exit_code, stdout=b'', stderr=b'')
        def close(fd):
            if fd in closed or fd not in descriptors:
                raise AssertionError('bad fake descriptor ownership')
            closed.append(fd)
        with patch.object(native.g, 'anonymous_text', memory), patch.object(native.subprocess, 'run', invoke), \
             patch.object(native.os, 'lseek', lambda *unused: 0), \
             patch.object(native.os, 'read', lambda fd, size: descriptors[fd]['payload'][:size]), \
             patch.object(native.os, 'close', close):
            record = native.run_generated(module, case, 'new', 24)
        check(len(closed) == len(descriptors), 'mock_native_descriptors_closed:' + case + ':' + str(exit_code))
        return record
    for cell in wrapper.RECORDS:
        case = cell['case']
        record = run_case(case)
        outputs[(case, cell['repetition'])] = record
        check(record['semantic_oracle_pass'] is True and record['result_set_boundaries_known'] is True
              and record['result_set_count'] == 1, 'mocked_boundary_and_independent_oracle:' + case + ':' + str(cell['repetition']))
        for key in ('query_sha256', 'schema_sha256', 'fixture_sha256', 'settings_sha256', 'node_count'):
            check(record[key] == cell[key], 'unchanged_workload_fingerprint:' + case + ':' + str(cell['repetition']) + ':' + key)
        command = commands[case]
        check(('--result-format' in command) is (case in wrapper.EMPTY_WITNESS_CASES),
              'actual_command_format_scoped:' + case + ':' + str(cell['repetition']))
        if case in wrapper.EMPTY_WITNESS_CASES:
            check(command[command.index('--result-format') + 1] == 'full-json'
                  and record['result_output_format'] == 'full-json'
                  and record['empty_result_set_payload_sha256'] == record['result_sha256'],
                  'actual_command_and_payload_witness_bound:' + case + ':' + str(cell['repetition']))
    bad_empty = run_case(wrapper.EMPTY_WITNESS_CASES[0], payload_override=b'')
    check(bad_empty['semantic_oracle_pass'] is False and bad_empty.get('harness_exception') == 'ValueError',
          'old_ambiguous_empty_stream_still_rejected')
    bad_nonempty = run_case(wrapper.EMPTY_WITNESS_CASES[0], payload_override=json.dumps(dict(valid, rows=[{}])).encode())
    check(bad_nonempty['semantic_oracle_pass'] is False, 'unexpected_actual_rows_never_normalized_to_empty')
    # Restriction checks run before fake descriptors or a fake process are needed.
    wrong = SimpleNamespace(__name__='unrelated', metadata=lambda *unused: {'c06_c50_empty_result_witness': wrapper.EMPTY_WITNESS_VERSION})
    try:
        native.run_generated(wrong, wrapper.EMPTY_WITNESS_CASES[0], 'new', 24)
    except ValueError:
        check(True, 'other_generator_cannot_request_special_transport')
    else:
        check(False, 'other_generator_cannot_request_special_transport')
    identity = {'scope': 'metadata-only'}
    provenance = {'binary_provenance': {'resolved_path': '/mock/runner'}, 'build_receipt_sha256': '1' * 64}
    scope = {'module_sha256': wrapper._verification_source_sha256, 'revision': REVISION, 'provenance': provenance}
    wrapper._require_scope = lambda: scope
    wrapper.integration_identity = lambda: identity
    complete = []
    for cell in wrapper.RECORDS:
        record = copy.deepcopy(outputs[(cell['case'], cell['repetition'])])
        record.update(repetition=cell['repetition'], rows=24, revision=REVISION, phase='integration',
                      integration_scope=identity, binary_provenance_verified=True, **copy.deepcopy(provenance))
        check(wrapper.validate_record(record, '/mock/runner') is True,
              'fresh_witness_validates_with_original_record_contract:' + cell['case'] + ':' + str(cell['repetition']))
        complete.append(record)
    sample = next(x for x in complete if x['case'] in wrapper.EMPTY_WITNESS_CASES)
    for label, changes in (
        ('missing_version', {'empty_result_set_witness_version': None}),
        ('missing_metadata', {'empty_result_set_metadata_verified': False}),
        ('unknown_boundaries', {'result_set_boundaries_known': False}),
        ('wrong_format', {'result_output_format': 'rows'}),
        ('payload_mismatch', {'empty_result_set_payload_sha256': '2' * 64}),
        ('missing_hashes', {'empty_result_set_payload_sha256': None, 'result_sha256': None}),
        ('invalid_hash', {'empty_result_set_payload_sha256': 'X' * 64, 'result_sha256': 'X' * 64}),
        ('bool_row_count', {'row_count': False}), ('bool_set_count', {'result_set_count': True}),
        ('no_result_set', {'result_set_count': 0}), ('multiple_sets', {'result_set_count': 2}),
        ('unexpected_rows', {'row_count': 1})):
        bad = dict(sample, **changes)
        try:
            wrapper.validate_record(bad, '/mock/runner')
        except ValueError:
            check(True, 'record_rejects:' + label)
        else:
            check(False, 'record_rejects:' + label)
    nonempty = next(x for x in complete if x['case'] not in wrapper.EMPTY_WITNESS_CASES)
    try:
        wrapper.validate_record(dict(nonempty, empty_result_set_metadata_verified=True), '/mock/runner')
    except ValueError:
        check(True, 'nonempty_control_cannot_borrow_empty_witness')
    else:
        check(False, 'nonempty_control_cannot_borrow_empty_witness')
    for field in ('result_output_format', 'empty_result_set_witness_version',
                  'empty_result_set_metadata_verified', 'empty_result_set_payload_sha256'):
        check(field in runner.SAFE_FIELDS, 'witness_survives_sanitized_runner:' + field)
    check(runner.SOURCE_BINDINGS['rbo-fixes-20261023/native_verify.py'] == images['after']['native_verify.py']
          and wrapper.SOURCE_BINDINGS['rbo-fixes-20261023/native_verify.py'] == images['after']['native_verify.py']
          and runner.SOURCE_BINDINGS['rbo-fixes-20261023/c06_c50_bounded_integration_checks.py'] == images['after']['c06_c50_bounded_integration_checks.py'],
          'exact_dependency_refresh_order')
    old_runner_ast = ast.parse((ROOT / 'c06_c50_integration_runner.py').read_bytes())
    new_runner_ast = ast.parse((HERE / 'c06_c50_integration_runner.py').read_bytes())
    functions = lambda tree: {n.name: ast.dump(n, include_attributes=False) for n in tree.body if isinstance(n, ast.FunctionDef)}
    check(functions(old_runner_ast) == functions(new_runner_ast), 'runner_function_bodies_byte_semantics_unchanged')
    source_paths = ('ydb/tests/tools/kqprun/src/kqp_runner.cpp', 'ydb/tests/tools/kqprun/runlib/utils.cpp',
                    'ydb/tests/tools/kqprun/kqprun.cpp', 'ydb/public/api/protos/ydb_value.proto',
                    'contrib/tools/protoc/plugins/cpp_styleguide/cpp_styleguide.cpp')
    production = {name: subprocess.check_output(['git', 'show', REVISION + ':' + name], cwd=PROJECT) for name in source_paths}
    check(b'ResultSets_.size() > 1' in production[source_paths[0]], 'single_set_rows_format_has_no_heading')
    check(b'resultSet.PrintJSON(output);' in production[source_paths[1]], 'full_json_uses_actual_ResultSet_metadata')
    check(b'"full-json", EResultOutputFormat::FullJson' in production[source_paths[2]], 'real_runner_full_json_option')
    proto = production[source_paths[3]]
    check(b'INT64          = 0x0003;' in proto and b'repeated Column columns = 1;' in proto
          and b'repeated Value rows = 2;' in proto and b'FORMAT_VALUE = 1;' in proto, 'actual_ResultSet_and_Int64_schema')
    check(b'GenerateYaStyle' not in proto, 'actual_proto_uses_original_lowercase_JSON_field_names')
    plugin = production[source_paths[4]]
    check(b'if ($rname$Size() > 0)' in plugin and b'if (Get$rname$())' in plugin,
          'zero_rows_and_default_scalars_are_omitted_by_actual_generator')
    check(b'out << (int)Get$rname$();' in plugin, 'enum_fields_print_as_integers')
    patch_path = ROOT / images['patch']
    with tempfile.TemporaryDirectory(prefix='c06-c50-empty-install-') as td:
        for name in images['before']:
            target = Path(td) / ROOT.name / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / name).read_bytes())
        subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(patch_path)], cwd=td,
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(['git', 'apply', str(patch_path)], cwd=td, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for name, expected in images['after'].items():
            check(sha((Path(td) / ROOT.name / name).read_bytes()) == expected, 'exact_disposable_application:' + name)
    for name, expected in images['before'].items():
        check(sha((ROOT / name).read_bytes()) == expected, 'live_source_unchanged:' + name)
    report = {'schema': 'c06_c50_empty_metadata_mock_contracts_v1', 'checks_passed': len(checks), 'checks': checks,
              'source_images': images, 'production_revision': REVISION,
              'production_source_bindings': {name: sha(raw) for name, raw in production.items()},
              'original_vector_count': 16, 'all_workload_fingerprint_slots_preserved': True,
              'full_json_case_count': 2, 'full_json_record_count': 4, 'unchanged_rows_format_record_count': 12,
              'native_build_toolchain_runs': 0, 'generated_queries_fixtures_rows_AST_or_diagnostics_persisted': False,
              'native_empty_metadata_transport_still_pending': True}
    output = ROOT / 'verification/C06-C50-empty-result-metadata-contracts-20261028.json'
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'checks_passed': len(checks), 'report_sha256': sha(output.read_bytes())}))


if __name__ == '__main__':
    main()
