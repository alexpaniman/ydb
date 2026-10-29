"""Closed local Q09/Q20 coverage admission; SQL/quality evidence stays separate."""
import hashlib
import json
from pathlib import Path
import re
from types import ModuleType

from anonymous_cpp_metadata import MetadataError

ROOT = Path(__file__).resolve().parent
MODULE = 'quality_statistics_cpp_generator'


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def current_contract(bug):
    if bug not in ('Q09', 'Q20'):
        raise MetadataError('unknown_statistics_contract')
    profile = json.loads((ROOT / 'quality_statistics_cpp_profiles.json').read_bytes())
    expected = profile[bug]
    case = json.loads((ROOT / 'cases' / (bug + '.json')).read_bytes())
    if (profile.get('schema_version') != 'reviewed_statistics_local_profiles_v1'
        or expected.get('base') != case.get('base') or expected.get('revision') != case.get('commit')):
        raise MetadataError('statistics_contract_revision_changed')
    path = ROOT / (MODULE + '.py')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected.get('generator_sha256'):
        raise MetadataError('statistics_generator_requires_review')
    module = ModuleType(MODULE)
    module.__file__ = str(path)
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    specifications = module.specs(bug)
    names = [spec['name'] for spec in specifications]
    counts = module.expected_checks(bug)
    source = module.render_cpp_suite(bug).encode()
    if (not names or len(set(names)) != len(names) or names != expected.get('case_names')
        or len(specifications) != expected.get('case_count')
        or _digest(specifications) != expected.get('specifications_sha256')
        or counts != expected.get('expected_checks') or any(type(value) is not int or value <= 0 for value in counts)
        or hashlib.sha256(source).hexdigest() != expected.get('generated_source_sha256')
        or len(source) != expected.get('generated_source_bytes')):
        raise MetadataError('statistics_coverage_requires_review')
    return module, expected['generator_sha256'], {'case_names': names, 'specifications': specifications}


def check_records(records, bug):
    module, _, _ = current_contract(bug)
    counts = module.expected_checks(bug)
    if len(records) != len(counts):
        raise MetadataError('incomplete_statistics_cases')
    for index, (record, expected) in enumerate(zip(records, counts)):
        if (record.get('case_index') != index or type(record.get('checks_executed')) is not int
            or record['checks_executed'] <= 0 or record['checks_executed'] > expected
            or record.get('passed') is True and record['checks_executed'] != expected):
            raise MetadataError('statistics_assertion_count_mismatch')


def validate_fixed(evidence, bug, revision):
    if not isinstance(evidence, dict):
        return False
    try:
        from anonymous_cpp_verify import (_validate_saved_pair_contract, _validate_saved_pair_process,
                                          _saved_pair_receipt, parse_verdicts)
        module, generator_hash, vector = current_contract(bug)
        case = json.loads((ROOT / 'cases' / (bug + '.json')).read_bytes())
        if (revision != case['commit'] or evidence.get('schema_version') != 'anonymous_direct_cpp_v1'
            or evidence.get('bug') != bug or evidence.get('phase') != 'fixed' or evidence.get('revision') != revision
            or evidence.get('status') != 'direct_local_contracts_passed'
            or evidence.get('closure_provenance_verified') is not True
            or evidence.get('generated_files_persisted') is not False
            or evidence.get('query_semantics_verified') is not False
            or evidence.get('quality_improvement_verified') is not False
            or evidence.get('proof_scope') != 'linked_production_local_contracts'
            or evidence.get('generator_module') != MODULE):
            return False
        _validate_saved_pair_contract(evidence, bug, revision, module, generator_hash, vector)
        receipt = _saved_pair_receipt(evidence, bug, revision)
        for phase in ('compile', 'link', 'native'):
            record = evidence.get(phase)
            _validate_saved_pair_process(record, phase)
            if record['exit_code'] != 0:
                return False
            if phase != 'native':
                environment = receipt['metadata'].get(phase + '_environment')
                if not isinstance(environment, dict) or record.get('environment_sha256') != _digest(environment):
                    return False
        for name in ('generated_object', 'generated_executable'):
            value = evidence.get(name, {})
            if (not isinstance(value.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', value['sha256'])
                or type(value.get('size_bytes')) is not int or value['size_bytes'] <= 0):
                return False
        records = parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in evidence['cases']),
                                len(vector['case_names']), 0, bug)
        return all(record['passed'] for record in records)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, MetadataError):
        return False
