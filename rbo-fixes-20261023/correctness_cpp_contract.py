"""Fixed-only local C31/C47 evidence, separately from query semantics.

Only reviewed generator source is evaluated. Generated translation units are
returned in memory; this module neither runs processes nor saves their output.
"""
import hashlib
import json
from pathlib import Path
import re
from types import ModuleType

from anonymous_cpp_metadata import VERSION as CLOSURE_VERSION, MetadataError

ROOT = Path(__file__).resolve().parent
MODULES = {'C31': 'c31_combined_cpp_generator', 'C47': 'c47_combined_cpp_generator'}
PROFILE_VERSION = 'reviewed_correctness_cpp_profiles_v1'
PROFILE_KEYS = {'module', 'revision', 'base', 'profile', 'generator_sha256', 'generated_source_sha256',
                'generated_source_bytes', 'case_count', 'proof_scope'}


def _digest(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def profile(bug, revision):
    document = json.loads((ROOT/'correctness_cpp_profiles.json').read_bytes())
    value = document.get('profiles', {}).get(bug)
    if (document.get('schema_version') != PROFILE_VERSION or bug not in MODULES
        or not isinstance(value, dict) or set(value) != (PROFILE_KEYS | ({'case_assertions', 'vectors'} if bug in ('C31', 'C47') else set()))
        or value.get('module') != MODULES[bug] or value.get('revision') != revision
        or not isinstance(revision, str) or not re.fullmatch('[0-9a-f]{40}', revision)
        or value.get('profile') != 'rbo' or value.get('proof_scope') != 'linked_production_local_contracts'
        or type(value.get('case_count')) is not int or value['case_count'] <= 0
        or type(value.get('generated_source_bytes')) is not int or value['generated_source_bytes'] <= 0
        or not _digest(value.get('generator_sha256')) or not _digest(value.get('generated_source_sha256'))):
        raise MetadataError('unreviewed_correctness_cpp_profile')
    if bug in ('C31', 'C47'):
        rows = value['case_assertions']
        if (not isinstance(rows, list) or len(rows) != value['case_count']
            or not isinstance(value['vectors'], list) or len(value['vectors']) != 2):
            raise MetadataError('invalid_correctness_cpp_case_vector')
        for index, row in enumerate(rows):
            if (not isinstance(row, dict) or set(row) != {'case_index', 'vector', 'case', 'assertion_count'}
                or type(row['case_index']) is not int or row['case_index'] != index
                or row['vector'] not in (('identity', 'pipeline') if bug == 'C31' else ('copy', 'covering'))
                or not isinstance(row['case'], str) or not row['case']
                or type(row['assertion_count']) is not int or row['assertion_count'] <= 0):
                raise MetadataError('invalid_correctness_cpp_case_vector')
    case = json.loads((ROOT/'cases'/(bug+'.json')).read_bytes())
    if case.get('commit') != revision or case.get('base') != value.get('base'):
        raise MetadataError('correctness_cpp_profile_case_revision_changed')
    return value


def profile_digest(bug, revision):
    return hashlib.sha256(json.dumps(profile(bug, revision), sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def render_suite(bug, revision):
    expected = profile(bug, revision)
    path = ROOT/(expected['module']+'.py')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected['generator_sha256']:
        raise MetadataError('correctness_cpp_generator_requires_new_source_review')
    # Evaluate the exact bytes whose digest was checked, not an import cache.
    module = ModuleType(expected['module'])
    module.__file__ = str(path)
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    if bug in ('C31', 'C47') and (module.specs() != expected['case_assertions']
                         or module.vector_manifest() != expected['vectors']):
        raise MetadataError('correctness_cpp_component_vector_changed')
    source = module.render_cpp_suite().encode()
    if (len(module.cases()) != expected['case_count'] or len(source) != expected['generated_source_bytes']
        or hashlib.sha256(source).hexdigest() != expected['generated_source_sha256']):
        raise MetadataError('correctness_cpp_rendered_contract_changed')
    return source, expected['case_count'], expected['generator_sha256']


def vector_evidence(bug, revision):
    expected = profile(bug, revision)
    if bug not in ('C31', 'C47'):
        return {}
    return {'direct_case_vector_sha256': hashlib.sha256(json.dumps(
                expected['case_assertions'], sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
            'direct_contract_vectors': expected['vectors']}


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise MetadataError('duplicate_correctness_cpp_verdict_field')
        value[key] = item
    return value


def parse_verdicts(stdout, count, exit_code, bug=None):
    if type(count) is not int or count <= 0 or not stdout or len(stdout) > 1024*1024:
        raise MetadataError('invalid_correctness_cpp_verdict_size')
    expected_rows = None
    if bug in ('C31', 'C47'):
        revision = json.loads((ROOT/'cases'/(bug+'.json')).read_bytes())['commit']
        expected_rows = profile(bug, revision)['case_assertions']
        if len(expected_rows) != count:
            raise MetadataError('incorrect_correctness_cpp_case_count')
    records = []
    for line in stdout.splitlines():
        value = json.loads(line, object_pairs_hook=_unique_object)
        if (not isinstance(value, dict) or set(value) != {'case_index', 'checks_executed', 'passed'}
            or type(value.get('case_index')) is not int or not 0 <= value['case_index'] < count
            or type(value.get('checks_executed')) is not int or value['checks_executed'] <= 0
            or type(value.get('passed')) is not bool):
            raise MetadataError('invalid_correctness_cpp_case_verdict')
        if expected_rows is not None:
            expected_count = expected_rows[value['case_index']]['assertion_count']
            if (value['checks_executed'] > expected_count
                or (value['passed'] and value['checks_executed'] != expected_count)):
                raise MetadataError('incorrect_correctness_cpp_assertion_count')
        records.append(value)
    if len(records) != count or sorted(value['case_index'] for value in records) != list(range(count)):
        raise MetadataError('incomplete_correctness_cpp_case_verdicts')
    failed = any(not value['passed'] for value in records)
    if type(exit_code) is not int or exit_code != int(failed):
        raise MetadataError('correctness_cpp_exit_disagrees_with_verdicts')
    return sorted(records, key=lambda value: value['case_index'])


def validate_fixed_local_contracts(evidence, bug, revision):
    """Require the complete actual guarded toolchain; never grant SQL acceptance."""
    if not isinstance(evidence, dict):
        return False
    try:
        expected = profile(bug, revision)
        source, count, generator_hash = render_suite(bug, revision)
        del source
        if (evidence.get('schema_version') != 'anonymous_direct_cpp_v1'
            or evidence.get('bug') != bug or evidence.get('phase') != 'fixed'
            or evidence.get('revision') != revision or evidence.get('status') != 'direct_local_contracts_passed'
            or evidence.get('direct_contract_profile_sha256') != profile_digest(bug, revision)
            or evidence.get('generator_module') != expected['module']
            or evidence.get('generator_sha256') != generator_hash
            or evidence.get('generated_source_sha256') != expected['generated_source_sha256']
            or type(evidence.get('generated_source_bytes')) is not int
            or evidence['generated_source_bytes'] != expected['generated_source_bytes']
            or type(evidence.get('case_count')) is not int or evidence['case_count'] != count
            or evidence.get('proof_scope') != expected['proof_scope']
            or evidence.get('closure_provenance_verified') is not True
            or evidence.get('generated_files_persisted') is not False
            or evidence.get('query_semantics_verified') is not False
            or evidence.get('quality_improvement_verified') is not False):
            return False
        if any(evidence.get(key) != value for key, value in vector_evidence(bug, revision).items()):
            return False
        raw_receipt = Path(evidence['closure_receipt_path']).read_bytes()
        receipt = json.loads(raw_receipt)
        if (hashlib.sha256(raw_receipt).hexdigest() != evidence.get('closure_receipt_sha256')
            or receipt.get('schema_version') != CLOSURE_VERSION or receipt.get('revision') != revision
            or type(receipt.get('build', {}).get('exit_code')) is not int or receipt['build']['exit_code'] != 0
            or receipt.get('build', {}).get('revision') != revision
            or receipt.get('build', {}).get('profile') != 'rbo'
            or receipt.get('build', {}).get('target') != 'ydb/core/kqp/ut/rbo'
            or receipt.get('metadata', {}).get('profile') != 'rbo'
            or not isinstance(receipt.get('files'), dict) or not receipt['files']):
            return False
        from production_version_object import validate_evidence as validate_version_evidence
        if not validate_version_evidence(evidence.get('production_version_infrastructure', {}), receipt, raw_receipt):
            return False
        for key in ('generated_object', 'generated_executable'):
            item = evidence.get(key, {})
            if not _digest(item.get('sha256')) or type(item.get('size_bytes')) is not int or item['size_bytes'] <= 0:
                return False
        for name in ('compile', 'link', 'native'):
            process = evidence.get(name, {})
            if (type(process.get('exit_code')) is not int or process['exit_code'] != 0
                or process.get('cleanup_complete') is not True or process.get('watchdog_stopped') is not False
                or process.get('stdout_overflow') is not False):
                return False
        records = parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in evidence['cases']), count, 0, bug=bug)
        return all(value['passed'] for value in records)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, MetadataError):
        return False
