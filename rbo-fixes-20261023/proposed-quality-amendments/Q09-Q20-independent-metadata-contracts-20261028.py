"""Independent dormant metadata checks; no production/native admission evidence.

Generated C++ is only hashed in RAM. In-memory verdict stand-ins exercise
parsing and rejection paths, never compilation or a claim of a native pass.
"""
import ast
import copy
import hashlib
import importlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    sys.dont_write_bytecode = True
    root = Path(__file__).resolve().parent.parent
    prep = json.loads((root / 'verification/Q09-Q20-statistics-source-preparation-20261028.json').read_bytes())
    plan_path = root / prep['after_idle_plan']
    assert hashlib.sha256(plan_path.read_bytes()).hexdigest() == prep['after_idle_plan_sha256']
    plan = json.loads(plan_path.read_bytes())
    patch = root / prep['patch']
    assert hashlib.sha256(patch.read_bytes()).hexdigest() == prep['patch_sha256']
    assertions = []

    def check(value, label):
        if not value:
            raise AssertionError(label)
        assertions.append(label)

    with tempfile.TemporaryDirectory(prefix='q09-q20-independent-metadata-') as folder:
        base = Path(folder)
        here = base / root.name
        here.mkdir()
        for path in root.glob('*.py'):
            shutil.copyfile(path, here / path.name)
        for name in plan['files']:
            path = root / name
            if path.is_file():
                shutil.copyfile(path, here / name)
        expectation_patch = root / 'proposed-quality-amendments/Q09-Q20-direct-statistics-harness-expectations-20261028.patch'
        for entry in plan['patches'] + [{'patch': prep['patch'], 'sha256': prep['patch_sha256']},
                                       {'patch': str(expectation_patch), 'sha256': '721a2b79b34c7f52c0e9fc7137047d3c52a84735f05046bfe96c884947950763'}]:
            path = root / entry['patch']
            check(hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'], 'patch_identity')
            process = subprocess.run(['git', 'apply', str(path)], cwd=base, capture_output=True)
            check(process.returncode == 0, 'temporary_patch_application')
        for name, digest in prep['after_files'].items():
            check(hashlib.sha256((here / name).read_bytes()).hexdigest() == digest, 'after_file_identity')
            if name.endswith('.py'):
                ast.parse((here / name).read_bytes())
                check(True, 'python_syntax')
        (here / 'cases').mkdir()
        for bug in ('Q09', 'Q20'):
            shutil.copyfile(root / 'cases' / (bug + '.json'), here / 'cases' / (bug + '.json'))
        sys.path[:0] = [str(here), str(root)]
        helper = importlib.import_module('anonymous_cpp_verify')
        contract = importlib.import_module('quality_statistics_cpp_contract')

        def forbidden(*args, **kwargs):
            raise AssertionError('metadata check attempted native/toolchain execution')

        helper.run_anonymous = forbidden
        helper._read_tool = forbidden

        def reject(call, label, reason=None):
            try:
                call()
            except helper.MetadataError as exc:
                if reason is not None and str(exc) != reason:
                    raise AssertionError(label + '_wrong_rejection_reason') from exc
                check(True, label)
            else:
                raise AssertionError(label)

        profile_path = here / 'quality_statistics_cpp_profiles.json'
        original_profile = profile_path.read_bytes()
        original_receipt = helper._saved_pair_receipt
        original_contract = helper._validate_saved_pair_contract
        coverage = {}
        for bug in ('Q09', 'Q20'):
            module, digest, vector = contract.current_contract(bug)
            specs = vector['specifications']
            counts = module.expected_checks(bug)
            count = len(specs)
            controls = sum(spec['control'] for spec in specs)
            coverage[bug] = {'cases': count, 'controls': controls, 'assertions': sum(counts)}
            check(count == prep['findings'][bug]['case_count'], bug + '_case_count')
            check(controls == prep['findings'][bug]['controls'], bug + '_control_count')
            payload, observed_count, observed_hash = helper.render_suite(bug, root.parent, '0' * 40)
            check(observed_count == count and observed_hash == digest, bug + '_dynamic_vector')
            check(hashlib.sha256(payload).hexdigest() == prep['findings'][bug]['source_sha256'], bug + '_source_identity')
            del payload
            records = [{'case_index': index, 'checks_executed': value, 'passed': True}
                       for index, value in enumerate(counts)]

            def parse(values, exit_code=0):
                return helper.parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in values), count, exit_code, bug)

            check(parse(records) == records, bug + '_metadata_parser_complete')
            for index in range(count):
                for value in (True, 0, counts[index] + 1, counts[index] - 1):
                    invalid = copy.deepcopy(records)
                    invalid[index]['checks_executed'] = value
                    reject(lambda: parse(invalid), bug + '_invalid_assertion_count')
                invalid = copy.deepcopy(records)
                invalid[index]['passed'] = False
                invalid[index]['checks_executed'] = 1
                check(parse(invalid, 1)[index]['passed'] is False, bug + '_bounded_failure_metadata')
            reject(lambda: parse(records[:-1]), bug + '_incomplete_vector')
            reject(lambda: parse(records + [records[0]]), bug + '_duplicate_vector')
            reject(lambda: parse(records, 1), bug + '_exit_disagreement')
            for key, value in (('case_index', True), ('passed', 1), ('unknown', True)):
                invalid = copy.deepcopy(records)
                invalid[0][key] = value
                reject(lambda: parse(invalid), bug + '_closed_record_schema')
            for key, value in (('generator_sha256', '0' * 64), ('specifications_sha256', '0' * 64),
                               ('generated_source_sha256', '0' * 64), ('generated_source_bytes', 1),
                               ('case_count', count - 1), ('expected_checks', counts[::-1] + [1]),
                               ('case_names', vector['case_names'][::-1]), ('revision', '0' * 40),
                               ('base', '0' * 40)):
                invalid = json.loads(original_profile)
                invalid[bug][key] = value
                profile_path.write_text(json.dumps(invalid))
                reject(lambda: contract.current_contract(bug), bug + '_profile_revalidation')
            profile_path.write_bytes(original_profile)

            # Invalid fixed evidence reaches the actual guard implementation,
            # with only receipt file access replaced by an explicit RAM fixture.
            # No complete fixed evidence is accepted or retained.
            case = json.loads((here / 'cases' / (bug + '.json')).read_bytes())
            environment = {'METADATA_ONLY': 'stand-in'}
            receipt = {'metadata': {'compile_environment': environment, 'link_environment': environment}}
            helper._saved_pair_receipt = lambda *args: receipt
            source = module.render_cpp_suite(bug).encode()
            process = {'exit_code': 0, 'watchdog_stopped': False, 'cleanup_complete': True,
                       'stdout_overflow': False, 'environment_sha256': helper._json_digest(environment)}
            evidence = {'schema_version': 'anonymous_direct_cpp_v1', 'bug': bug, 'phase': 'fixed',
                        'revision': case['commit'], 'status': 'direct_local_contracts_passed',
                        'closure_provenance_verified': True, 'generated_files_persisted': False,
                        'query_semantics_verified': False, 'quality_improvement_verified': False,
                        'proof_scope': 'linked_production_local_contracts', 'generator_module': contract.MODULE,
                        'generator_sha256': digest, 'case_count': count,
                        'closure_receipt_path': str(base / 'absent-metadata-only-receipt.json'),
                        'closure_receipt_sha256': '3' * 64,
                        'direct_case_vector_sha256': helper._json_digest(vector),
                        'generated_source_sha256': hashlib.sha256(source).hexdigest(),
                        'generated_source_bytes': len(source), 'compile': dict(process), 'link': dict(process),
                        'native': dict(process), 'cases': records,
                        'generated_object': {'sha256': '1' * 64, 'size_bytes': 1},
                        'generated_executable': {'sha256': '2' * 64, 'size_bytes': 1}}
            del source
            mutations = [(('phase',), 'baseline'), (('revision',), case['base']),
                         (('generator_sha256',), '0' * 64), (('case_count',), True),
                         (('direct_case_vector_sha256',), '0' * 64),
                         (('generated_source_sha256',), '0' * 64), (('generated_source_bytes',), True),
                         (('query_semantics_verified',), True), (('quality_improvement_verified',), True),
                         (('proof_scope',), 'unreviewed'), (('status',), 'unreviewed')]
            for phase in ('compile', 'link', 'native'):
                mutations.extend([((phase, key), value) for key, value in (
                    ('exit_code', True), ('exit_code', 1), ('watchdog_stopped', True),
                    ('cleanup_complete', False), ('stdout_overflow', True))])
                if phase != 'native':
                    mutations.append(((phase, 'environment_sha256'), '0' * 64))
            for name in ('generated_object', 'generated_executable'):
                mutations += [((name, 'size_bytes'), True), ((name, 'size_bytes'), 0), ((name, 'sha256'), None)]
            for keys, value in mutations:
                invalid = copy.deepcopy(evidence)
                target = invalid
                for key in keys[:-1]:
                    target = target[key]
                target[keys[-1]] = value
                check(contract.validate_fixed(invalid, bug, case['commit']) is False, bug + '_invalid_fixed_admission')

            # A failed baseline control must reject the pair even when the
            # complete fixed vector and every other baseline case are positive.
            for index, spec in enumerate(specs):
                if not spec['control']:
                    continue
                before = copy.deepcopy(evidence)
                before.update(phase='baseline', revision=case['base'], status='direct_local_contracts_failed')
                before['cases'][index].update(passed=False, checks_executed=1)
                before['native']['exit_code'] = 1
                reject(lambda: helper.pair_direct(bug, before, evidence, case), bug + '_baseline_control_required',
                       'baseline_statistics_control_failed')
            helper._saved_pair_receipt = original_receipt
            # The actual receipt path gate remains closed, independently of
            # the preceding local guard stand-in.
            check(contract.validate_fixed(evidence, bug, case['commit']) is False, bug + '_missing_real_receipt_rejected')
            del evidence, records

        helper._validate_saved_pair_contract = original_contract
        helper._saved_pair_receipt = original_receipt
        check(helper._closure_profile('Q09') == 'rbo' and helper._closure_profile('Q20') == 'rbo', 'rbo_profile_binding')
        legacy_contracts = importlib.import_module('quality_pairing_contracts')
        legacy_result = legacy_contracts.run_contract_checks()
        check(True, 'existing_stacked_quality_metadata_contracts')
        for bug in ('Q09', 'Q20'):
            for explicit_flag in (None, False):
                before = legacy_contracts._record('Q09', 'baseline')
                after = legacy_contracts._record('Q09', 'fixed')
                name = bug.lower() + '_metadata_gate'
                for record in (before, after):
                    record.update(bug=bug, case=name)
                if explicit_flag is not None:
                    after['additional_ir_admission_required'] = explicit_flag
                paired, accepted, status = legacy_contracts.pairing.pair_quality_records(
                    [before], [after], bug, 'b' * 40, 'f' * 40,
                    required_case_owners={name: after['generator_module']}, required_profile_sha256='d' * 64)
                check(not accepted and status == 'verified_semantics_quality_pending'
                      and paired[0]['additional_ir_admission_required'] is True
                      and 'additional_ir_admission_pending' in paired[0]['quality_pairing_reasons']
                      and paired[0]['quality_acceptance_pass'] is False,
                      bug + '_unconditional_fixed_direct_gate')
    return {'schema_version': 'q09_q20_independent_metadata_review_v1', 'assertions_passed': len(assertions),
            'coverage': coverage, 'patch_sha256': prep['patch_sha256'],
            'harness_expectation_patch_sha256': hashlib.sha256(expectation_patch.read_bytes()).hexdigest(),
            'temporary_17_stack_plus_candidate_application': True,
            'scope': 'source identities, RAM rendering, parser/profile and negative admission guards only',
            'native_runs': 0, 'toolchain_runs': 0, 'builds': 0, 'graphs': 0,
            'generated_sources_persisted': False, 'native_evidence_fabricated_or_accepted': False,
            'synthetic_verdicts_used_only_for_metadata_checks': True,
            'existing_stacked_quality_metadata_contracts': legacy_result}


if __name__ == '__main__':
    print(json.dumps(main(), sort_keys=True))
