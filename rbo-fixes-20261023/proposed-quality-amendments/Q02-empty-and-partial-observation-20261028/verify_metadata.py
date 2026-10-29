"""Q02 metadata/fake-process checks; never persist rendered workload or graph."""
import ast
import copy
import hashlib
import importlib
import json
from pathlib import Path
import subprocess
import sys
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROJECT = ROOT.parent
sys.path[:0] = [str(ROOT), str(PROJECT / 'rbo-review-20261021')]


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path, name=None):
    module = ModuleType(name or path.stem)
    module.__file__ = str(ROOT / path.name)
    raw = path.read_bytes()
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    module._verification_source_sha256 = sha(raw)
    return module


def graph(scalar=False, label='AggrCountInit', live=True):
    initial = [label, ['Member', '$i', "'input"]]
    updated = ['AggrCountUpdate', ['Member', '$u', "'input"], '$s' if scalar else ['Member', '$s', "'acc"]]
    if not scalar:
        initial, updated = ['AsStruct', ["'acc", initial]], ['AsStruct', ["'acc", updated]]
    fold = ['Fold1', ['WinFrame', '$q', '0', 'false', 'false', 'false', ['DependsOn', '$row']],
            ['lambda', ['$i'], initial], ['lambda', ['$u', '$s'], updated]]
    root = ['block', ['let', '$q', ['QueueCreate']], ['let', '$c', ['WinFramesCollector', 'input', '$q', 'params']],
            ['return', ['DqPhyStage', ['$c', fold] if live else ['Void']]]]
    def render(value):
        return '(' + ' '.join(render(item) for item in value) + ')' if isinstance(value, list) else value
    return render(root)


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    images = json.loads((HERE / 'source-images.json').read_bytes())
    for name in images['before']:
        check(sha((ROOT / name).read_bytes()) == images['before'][name], 'exact_installed60_before:' + name)
        check(sha((HERE / name).read_bytes()) == images['after'][name], 'exact_candidate_after:' + name)
    original = load(ROOT / 'quality_window_checks.py', 'quality_window_checks')
    window = load(HERE / 'quality_window_checks.py', 'quality_window_checks')
    native = load(HERE / 'native_verify.py', 'native_verify')
    pairing = load(HERE / 'quality_pairing.py', 'quality_pairing')
    cases = [case for case in window.recipes(24) if case.startswith('q02_')]
    check(len(cases) == 4, 'exact_original_four_cases')
    for case in cases:
        check(original.recipes(24)[case] == window.recipes(24)[case], 'unchanged_query:' + case)
        check(original.schema_and_seeds(24, case) == window.schema_and_seeds(24, case), 'unchanged_schema_fixture:' + case)
        check(original.expected_rows(case, 24) == window.expected_rows(case, 24), 'unchanged_row_oracle:' + case)
        metadata = copy.deepcopy(window.metadata(case, 24))
        token = metadata.pop('q02_empty_result_witness', None)
        check(metadata == original.metadata(case, 24), 'unchanged_settings_metadata:' + case)
        check((token == 'q02_empty_result_metadata_v1') == (case == 'q02_empty'), 'special_transport_exact_case:' + case)

    valid = {'columns':[{'name':'id','type':{'type_id':3}}]}
    good = json.dumps(valid).encode()
    for payload in (good, json.dumps(dict(valid, format=1)).encode(), b' \n' + good + b'\n'):
        rows, sets, known, witness = window.parse_empty_result_witness('q02_empty', payload, 24)
        check(rows == [] and sets == [[]] and known is True, 'explicit_empty_result_set_proved')
        check(witness['empty_result_set_payload_sha256'] == sha(payload), 'exact_payload_hash_bound')
    negatives = {'blank':b'', 'space':b' ', 'multiple':good + good, 'heading':b'Result set 1:\n' + good,
                 'oversize':b' ' * 16385, 'duplicate':good.replace(b'"type_id": 3', b'"type_id": 3, "type_id": 3')}
    for label, value in (('wrong_name', {'columns':[{'name':'other','type':{'type_id':3}}]}),
            ('wrong_type', {'columns':[{'name':'id','type':{'type_id':4}}]}),
            ('bool_type', {'columns':[{'name':'id','type':{'type_id':True}}]}),
            ('float_type', {'columns':[{'name':'id','type':{'type_id':3.0}}]}),
            ('optional_type', {'columns':[{'name':'id','type':{'optional_type':{'item':{'type_id':3}}}}]}),
            ('missing_columns', {}), ('duplicate_columns', {'columns':valid['columns'] * 2}),
            ('rows_even_empty', dict(valid, rows=[])), ('rows_present', dict(valid, rows=[{}])),
            ('truncated', dict(valid, truncated=True)), ('false_truncated', dict(valid, truncated=False)),
            ('arrow', dict(valid, format=2)), ('bool_format', dict(valid, format=True)),
            ('float_format', dict(valid, format=1.0)), ('data', dict(valid, data=''))):
        negatives[label] = json.dumps(value).encode()
    for label, payload in negatives.items():
        try:
            window.parse_empty_result_witness('q02_empty', payload, 24)
        except (ValueError, TypeError, KeyError, IndexError):
            check(True, 'rejects_ambiguous_or_malformed_metadata:' + label)
        else:
            check(False, 'rejects_ambiguous_or_malformed_metadata:' + label)
    for case, rows in [('q02_partial',24), ('q02_empty',True), ('q02_empty',23), ('q02_empty',25)]:
        try:
            window.parse_empty_result_witness(case, good, rows)
        except ValueError:
            check(True, 'rejects_out_of_scope_witness:' + case + ':' + str(rows))
        else:
            check(False, 'scope_rejection_required')

    # The new inventory cannot convert scalarized or otherwise rejected state into proof.
    for scalar, label in ((False,'AggrCountInit'), (True,'AggrCountInit'), (True,'sensitive_unlisted_atom')):
        text = graph(scalar, label)
        old = original._window_frame_fold_state(text)
        new = window._window_frame_fold_state(text)
        check({key:new[key] for key in old} == old, 'old_partial_proof_fields_unchanged:' + str(scalar) + ':' + str(label == 'AggrCountInit'))
        inventory = new['shape_inventory']
        check(inventory['fold_count'] == 1 and inventory['observation_only'] is True
              and inventory['state_binding_proved'] is False, 'inventory_cannot_admit_proof')
        check('sensitive_unlisted_atom' not in json.dumps(new), 'inventory_omits_unlisted_source_atoms')
        if scalar:
            check(new['complete'] is False and new['shape_rejection_reason'] == 'unresolved_frame_state',
                  'scalar_state_remains_pending_with_specific_reason')
    empty_ast = graph(live=False)
    empty_summary = window.check_physical_ast('q02_empty', empty_ast)
    check(empty_summary['structural_assertions'] == {'unused_frame_state_removed':True}
          and empty_summary['structural_oracle_pass'] is True, 'empty_control_checks_intended_pipeline_removal')
    check(window.check_physical_ast('q02_empty', graph())['structural_oracle_pass'] is False, 'empty_control_rejects_retained_window_state')

    actual_path = ROOT / 'verification/guarded-fixed-Q02-1b50d28716f7-1793162330132081306.jsonl'
    actual_raw = actual_path.read_bytes()
    actual = {row['case']:row for row in map(json.loads, actual_raw.splitlines())}
    outputs, commands = {}, {}
    def run_case(case, payload_override=None):
        descriptors, closed = {}, []
        def memory(label, value):
            fd = 1000 + len(descriptors)
            descriptors[fd] = {'label':label, 'payload':value.encode() if isinstance(value,str) else value}
            return fd
        def invoke(command, **kwargs):
            commands[case] = list(command)
            payload = good if case == 'q02_empty' else b'\n'.join(json.dumps(row).encode() for row in window.expected_rows(case,24))
            if payload_override is not None:
                payload = payload_override
            for item in descriptors.values():
                if item['label'] == 'results':item['payload'] = payload
                if item['label'] == 'target-physical-ast':item['payload'] = empty_ast.encode()
            return SimpleNamespace(returncode=0,stdout=b'',stderr=b'')
        def close(fd):
            if fd in closed or fd not in descriptors:raise AssertionError('fake ownership mismatch')
            closed.append(fd)
        with patch.object(native.g,'anonymous_text',memory), patch.object(native.subprocess,'run',invoke), \
             patch.object(native.os,'lseek',lambda *args:0), patch.object(native.os,'read',lambda fd,n:descriptors[fd]['payload'][:n]), \
             patch.object(native.os,'close',close):
            result = native.run_generated(window,case,'new',24)
        check(len(closed) == len(descriptors), 'fake_native_descriptors_closed:' + case)
        return result
    for case in cases:
        record = run_case(case)
        outputs[case] = record
        check(record['semantic_oracle_pass'] is True, 'mock_preserves_exact_row_oracle:' + case)
        for key in ('query_sha256','schema_sha256','fixture_sha256','settings_sha256','node_count'):
            check(record[key] == actual[case][key], 'actual_saved_workload_identity_preserved:' + case + ':' + key)
        check(('--result-format' in commands[case]) == (case == 'q02_empty'), 'format_change_is_case_scoped:' + case)
    witness_record = outputs['q02_empty']
    check(witness_record['result_set_boundaries_known'] is True and witness_record['result_set_count'] == 1
          and witness_record['row_count'] == 0 and witness_record['empty_result_set_payload_sha256'] == witness_record['result_sha256'],
          'native_transport_records_complete_empty_witness')
    check(run_case('q02_empty', b'')['semantic_oracle_pass'] is False, 'blank_native_output_still_rejected')
    for label, name, case, mode, count, token, identity in (
            ('module','unrelated','q02_empty','new',24,'q02_empty_result_metadata_v1','a'*64),
            ('case','quality_window_checks','q02_partial','new',24,'q02_empty_result_metadata_v1','a'*64),
            ('mode','quality_window_checks','q02_empty','legacy',24,'q02_empty_result_metadata_v1','a'*64),
            ('count','quality_window_checks','q02_empty','new',25,'q02_empty_result_metadata_v1','a'*64),
            ('boolean_count','quality_window_checks','q02_empty','new',True,'q02_empty_result_metadata_v1','a'*64),
            ('token','quality_window_checks','q02_empty','new',24,'unreviewed','a'*64),
            ('snapshot','quality_window_checks','q02_empty','new',24,'q02_empty_result_metadata_v1',None)):
        module = SimpleNamespace(__name__=name, _verification_source_sha256=identity,
            metadata=lambda *unused, token=token:{'q02_empty_result_witness':token})
        try:
            native.run_generated(module,case,mode,count)
        except ValueError:
            check(True, 'actual_native_special_dispatch_rejects:' + label)
        else:
            check(False, 'special_dispatch_scope_failure')
    check(pairing._fixed_acceptance(witness_record,witness_record) is True, 'empty_fixed_acceptance_requires_both_completed_witnesses')
    check(pairing._improvement('q02_empty',witness_record,witness_record) is False, 'empty_control_not_reported_as_improvement')
    for key,value in [('result_set_boundaries_known',False),('result_set_count',0),('result_set_count',True),
            ('row_count',False),('row_count',1),('result_output_format','rows'),
            ('empty_result_set_witness_version','unrelated'),('empty_result_set_metadata_verified',False),
            ('empty_result_set_payload_sha256','0'*64),('result_sha256',None)]:
        bad = dict(witness_record, **{key:value})
        check(pairing._fixed_acceptance(bad,witness_record) is False and pairing._fixed_acceptance(witness_record,bad) is False,
              'both_phase_empty_witness_gate:' + key + ':' + str(type(value).__name__))
    for case in cases:
        if case != 'q02_empty':
            old_pairing = load(ROOT / 'quality_pairing.py')
            try:old_verdict = old_pairing._fixed_acceptance(actual[case],actual[case])
            except ValueError:old_verdict = 'pending'
            try:new_verdict = pairing._fixed_acceptance(actual[case],actual[case])
            except ValueError:new_verdict = 'pending'
            check(old_verdict == new_verdict, 'historical_nonempty_acceptance_unchanged:' + case)
    check(actual_path.read_bytes() == actual_raw, 'original_failed_evidence_preserved')

    # Every changed dependency pin is exact, with no policy or unrelated function edits.
    virtual = {(ROOT/name).resolve():(HERE/name).read_bytes() for name in images['after']}
    for name in ('quality_spill_bounds.py','c06_c50_bounded_integration_checks.py','c06_c50_integration_runner.py'):
        old = ast.parse((ROOT/name).read_bytes());new = ast.parse((HERE/name).read_bytes())
        functions = lambda tree:{node.name:ast.dump(node) for node in tree.body if isinstance(node,ast.FunctionDef)}
        check(functions(old) == functions(new), 'binding_only_preserves_functions:' + name)
        key = '_SOURCE_BINDINGS' if name == 'quality_spill_bounds.py' else 'SOURCE_BINDINGS'
        pins = next(ast.literal_eval(node.value) for node in new.body if isinstance(node,ast.Assign)
                    and any(isinstance(target,ast.Name) and target.id == key for target in node.targets))
        for dependency,expected in pins.items():
            path = (PROJECT/dependency).resolve()
            check(sha(virtual[path] if path in virtual else path.read_bytes()) == expected, 'exact_dependency:' + name + ':' + dependency)
    # Snapshot the exact actual formatter/type sources without rendering their input.
    fixed = '1b50d28716f74eb02571e7490181132b14c37bff'
    paths = ('ydb/tests/tools/kqprun/src/kqp_runner.cpp','ydb/tests/tools/kqprun/runlib/utils.cpp',
             'ydb/tests/tools/kqprun/kqprun.cpp','ydb/public/api/protos/ydb_value.proto',
             'contrib/tools/protoc/plugins/cpp_styleguide/cpp_styleguide.cpp',
             'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_window_builder.cpp',
             'yql/essentials/core/common_opt/yql_co_flow1.cpp')
    production = {path:subprocess.check_output(['git','show',fixed+':'+path],cwd=PROJECT,stderr=subprocess.PIPE) for path in paths}
    check(b'resultSet.PrintJSON(output);' in production[paths[1]] and b'INT64          = 0x0003;' in production[paths[3]],
          'actual_formatter_and_Int64_type_contract')
    check(b'ExtractOneItemStructFromFold1' in production[paths[-1]], 'actual_scalarization_source_exists')
    report = {'schema':'q02_empty_witness_partial_observation_metadata_v1','checks_passed':len(checks),'checks':checks,
        'patch_sha256':images['patch_sha256'],'before':images['before'],'after':images['after'],
        'original_four_cases':cases,'original_evidence_sha256':sha(actual_raw),
        'production_source_sha256':{path:sha(raw) for path,raw in production.items()},
        'partial_acceptance_changed':False,'actual_partial_shape_claimed':False,'full_quality_admission':False,
        'native_build_toolchain_executed':False,'generated_SQL_CPP_rows_fixtures_AST_persisted':False}
    output = ROOT/'verification/Q02-empty-witness-partial-observation-metadata-20261028.json'
    output.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    print(json.dumps({'checks_passed':len(checks),'report_sha256':sha(output.read_bytes())}))


if __name__ == '__main__':
    main()
