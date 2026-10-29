"""Fixed-only, reviewed Q11 structural admission; SQL pairing stays separate."""
import hashlib
import json
from pathlib import Path
import re
from types import ModuleType

from anonymous_cpp_metadata import VERSION as CLOSURE_VERSION, MetadataError
from correctness_cpp_contract import parse_verdicts as _parse_verdicts

ROOT = Path(__file__).resolve().parent
MODULE = 'quality_union_cpp_generator'
PROFILE_KEYS = {'schema_version', 'module', 'revision', 'base', 'profile', 'generator_sha256',
                'generated_source_sha256', 'generated_source_bytes', 'case_count',
                'case_names_sha256', 'expected_checks', 'proof_scope'}


def _digest(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def _json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def profile(revision):
    value = json.loads((ROOT / 'quality_union_cpp_profile.json').read_bytes())
    if (not isinstance(value, dict) or set(value) != PROFILE_KEYS
        or value.get('schema_version') != 'reviewed_union_cpp_profile_v1'
        or value.get('module') != MODULE or value.get('revision') != revision
        or not isinstance(revision, str) or not re.fullmatch('[0-9a-f]{40}', revision)
        or value.get('profile') != 'rbo' or value.get('proof_scope') != 'linked_production_local_contracts'
        or type(value.get('case_count')) is not int or value['case_count'] <= 0
        or type(value.get('generated_source_bytes')) is not int or value['generated_source_bytes'] <= 0
        or any(not _digest(value.get(key)) for key in
               ('generator_sha256', 'generated_source_sha256', 'case_names_sha256'))
        or not isinstance(value.get('expected_checks'), list)
        or len(value['expected_checks']) != value['case_count']
        or any(type(count) is not int or count <= 0 for count in value['expected_checks'])):
        raise MetadataError('unreviewed_union_cpp_profile')
    case = json.loads((ROOT / 'cases' / 'Q11.json').read_bytes())
    if case.get('commit') != revision or case.get('base') != value.get('base'):
        raise MetadataError('union_cpp_case_revision_changed')
    return value


def profile_digest(revision):
    return _json_digest(profile(revision))


def render_suite(revision):
    expected = profile(revision)
    path = ROOT / (MODULE + '.py')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected['generator_sha256']:
        raise MetadataError('union_cpp_generator_requires_source_review')
    module = ModuleType(MODULE)
    module.__file__ = str(path)
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    source = module.render_cpp_suite().encode()
    if (len(module.cases()) != expected['case_count']
        or _json_digest(module.cases()) != expected['case_names_sha256']
        or module.expected_checks() != expected['expected_checks']
        or len(source) != expected['generated_source_bytes']
        or hashlib.sha256(source).hexdigest() != expected['generated_source_sha256']):
        raise MetadataError('union_cpp_rendered_contract_changed')
    return source, expected['case_count'], expected['generator_sha256']


def parse_verdicts(stdout, count, exit_code):
    """Closed numeric verdict schema; the fixed gate also pins assertion counts."""
    return _parse_verdicts(stdout, count, exit_code)


def validate_fixed_local_contracts(evidence, revision):
    if not isinstance(evidence, dict):
        return False
    try:
        expected = profile(revision)
        source, count, generator_hash = render_suite(revision)
        del source
        if (evidence.get('schema_version') != 'anonymous_direct_cpp_v1'
            or evidence.get('bug') != 'Q11' or evidence.get('phase') != 'fixed'
            or evidence.get('revision') != revision or evidence.get('status') != 'direct_local_contracts_passed'
            or evidence.get('direct_contract_profile_sha256') != profile_digest(revision)
            or evidence.get('generator_module') != MODULE
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
            if name in ('compile', 'link'):
                environment = receipt['metadata'].get(name + '_environment')
                if not isinstance(environment, dict) or process.get('environment_sha256') != _json_digest(environment):
                    return False
        records = parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in evidence['cases']), count, 0)
        return (all(value['passed'] for value in records)
                and [value['checks_executed'] for value in records] == expected['expected_checks'])
    except (OSError, ValueError, TypeError, KeyError, AttributeError, MetadataError):
        return False
