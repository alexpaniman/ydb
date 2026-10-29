"""Mock the dormant Q01 evidence gate; no production process starts."""
import copy
import hashlib
import json
from pathlib import Path
from unittest.mock import patch

import quality_range_runtime_contract as contract
import quality_range_runtime_cpp_generator as generator

ROOT = Path(__file__).resolve().parent


def _fixture():
    records = []
    for index, spec in enumerate(generator.specs()):
        cycles = 4 if spec['lifecycle'] == 'reset' else 1
        complete = spec['lifecycle'] in ('complete', 'yield', 'reset')
        limited = spec['lifecycle'] == 'limit'
        record = dict.fromkeys(generator.METRIC_FIELDS, 0)
        record.update(case_index=index, checks_executed=20, passed=True, operator_instances=1,
                      cycles=cycles, limit_rejections=int(limited))
        record['input_rows'] = cycles * spec['rows'] if complete else spec.get('limit_at', 1) if limited else 10
        record['output_rows'] = cycles * spec['rows'] if complete else 7 if spec['lifecycle'] == 'cancel_output' else 0
        if spec['lifecycle'] in ('yield', 'cancel_yield'):
            record['input_yields'] = record['output_yields'] = 2
        record['input_fetches'] = record['input_rows'] + record['input_yields'] + int(complete)
        record['peak_live_rows'] = min(record['input_rows'], 8)
        if spec['rows'] and not limited:
            record['allocator_offloads'] = 4
            record['allocator_offloaded_bytes'] = 1024
        records.append(record)
    source = generator.render_cpp_suite().encode()
    receipt = {'schema_version': contract.CLOSURE_VERSION, 'revision': generator.SOURCE_COMMIT,
               'build': {'revision': generator.SOURCE_COMMIT, 'exit_code': 0,
                         'target': 'ydb/core/kqp/runtime/streaming/ut'},
               'metadata': {'profile': generator.PROFILE}, 'files': [{'synthetic': True}]}
    raw_receipt = json.dumps(receipt).encode()
    digest = '1' * 64
    evidence = {'schema_version': 'anonymous_direct_cpp_v1', 'bug': 'Q01', 'phase': 'fixed',
                'revision': generator.SOURCE_COMMIT, 'status': 'direct_local_contracts_passed',
                'closure_provenance_verified': True, 'generated_files_persisted': False,
                'proof_scope': contract.PROOF_SCOPE, 'generator_module': 'quality_range_runtime_cpp_generator',
                'generator_sha256': generator.execution_manifest()['generator_sha256'],
                'generated_source_sha256': hashlib.sha256(source).hexdigest(),
                'generated_source_bytes': len(source), 'case_count': len(records), 'cases': records,
                'closure_receipt_path': '/anonymous-mocked-receipt',
                'closure_receipt_sha256': hashlib.sha256(raw_receipt).hexdigest(),
                'generated_object': {'sha256': digest, 'size_bytes': 1},
                'generated_executable': {'sha256': digest, 'size_bytes': 1}}
    for phase in ('compile', 'link', 'native'):
        evidence[phase] = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False, stdout_overflow=False)
    return evidence, receipt, raw_receipt


def main():
    evidence, receipt, raw_receipt = _fixture()
    assertions = 0
    def check(value):
        nonlocal assertions
        assert value
        assertions += 1
    original_read = Path.read_bytes
    def mocked_read(path):
        return raw_receipt if str(path) == '/anonymous-mocked-receipt' else original_read(path)
    with patch.object(Path, 'read_bytes', mocked_read):
        check(contract.validate_fixed_runtime(evidence, generator.SOURCE_COMMIT))
        mutations = [
            ('generator_sha256', '2' * 64), ('generated_source_sha256', '2' * 64),
            ('generated_source_bytes', 0), ('case_count', True), ('phase', 'baseline'),
            ('revision', '2' * 40), ('status', 'prepared_unverified'),
            ('generated_files_persisted', True), ('closure_provenance_verified', False),
            ('closure_receipt_sha256', '2' * 64), ('proof_scope', 'linked_production_local_contracts'),
            ('generator_module', 'quality_minmax_checks')]
        for field, value in mutations:
            changed = copy.deepcopy(evidence); changed[field] = value
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        for phase in ('compile', 'link', 'native'):
            for field, value in (('exit_code', 1), ('cleanup_complete', False), ('watchdog_stopped', True), ('stdout_overflow', True)):
                changed = copy.deepcopy(evidence); changed[phase][field] = value
                check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
            changed = copy.deepcopy(evidence); del changed[phase]
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        for field in ('generated_object', 'generated_executable'):
            changed = copy.deepcopy(evidence); changed[field]['size_bytes'] = 0
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        for field in generator.METRIC_FIELDS:
            for value in (False, -1):
                changed = copy.deepcopy(evidence); changed['cases'][0][field] = value
                check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
            changed = copy.deepcopy(evidence); del changed['cases'][0][field]
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        for field, value in (('passed', False), ('checks_executed', 0), ('cleanup_live_rows', 1),
                             ('operator_instances', 0), ('output_rows', 1), ('unapproved', 1)):
            changed = copy.deepcopy(evidence); changed['cases'][0][field] = value
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        changed = copy.deepcopy(evidence); changed['cases'][1]['case_index'] = 0
        check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        changed = copy.deepcopy(evidence); changed['cases'].pop()
        check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        for field, value in (('revision', '2' * 40), ('files', [])):
            altered_receipt = copy.deepcopy(receipt); altered_receipt[field] = value
            raw_receipt = json.dumps(altered_receipt).encode()
            changed = copy.deepcopy(evidence); changed['closure_receipt_sha256'] = hashlib.sha256(raw_receipt).hexdigest()
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
        for field, value in (('exit_code', 1), ('exit_code', False), ('exit_code', 0.0),
                             ('revision', '2' * 40), ('target', 'ydb/core/kqp/ut/rbo')):
            altered_receipt = copy.deepcopy(receipt); altered_receipt['build'][field] = value
            raw_receipt = json.dumps(altered_receipt).encode()
            changed = copy.deepcopy(evidence); changed['closure_receipt_sha256'] = hashlib.sha256(raw_receipt).hexdigest()
            check(not contract.validate_fixed_runtime(changed, generator.SOURCE_COMMIT))
    result = dict(mock_contracts_passed=True, assertions=assertions, case_count=len(generator.cases()),
                  generator_sha256=generator.execution_manifest()['generator_sha256'],
                  validator_sha256=hashlib.sha256(Path(contract.__file__).read_bytes()).hexdigest(),
                  native_runs=0, compiler_runs=0, generated_source_saved=False,
                  synthetic_evidence_saved=False, production_edited=False)
    (ROOT/'verification/Q01-runtime-evidence-mocked-contracts.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
