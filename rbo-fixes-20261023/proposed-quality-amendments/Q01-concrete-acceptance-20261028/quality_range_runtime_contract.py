"""Closed fixed-only acceptance for real Q01 runtime evidence.

No compilation, execution or generated fixture persistence occurs here.
"""
import hashlib
import json
from pathlib import Path
import re

from anonymous_cpp_metadata import VERSION as CLOSURE_VERSION, MetadataError, digest_file
import quality_range_runtime_cpp_generator as generator

PROOF_SCOPE = 'linked_production_runtime_lifecycle_contracts'


def _digest(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def _case_observations(record, spec):
    for stem, delta in (('allocations', 'allocator_allocations'),
                        ('offloads', 'allocator_offloads'),
                        ('offloaded_bytes', 'allocator_offloaded_bytes'),
                        ('peak_used', 'peak_used_delta'),
                        ('peak_allocated', 'peak_allocated_delta')):
        before = record['allocator_' + stem + '_before']
        after = record['allocator_' + stem + '_after']
        # Legacy peaks were sampled before graph invalidation; the new absolute
        # after snapshot also includes release/invalidation. Keep that scope
        # difference explicit instead of requiring cleanup to allocate nothing.
        if after < before or (after - before < record[delta] if stem.startswith('peak_')
                              else after - before != record[delta]):
            return False
    for stem in ('peak_used', 'peak_allocated'):
        consumed = record['allocator_' + stem + '_after_consumption']
        if (consumed - record['allocator_' + stem + '_before'] != record[stem + '_delta']
                or consumed > record['allocator_' + stem + '_after']):
            return False
    if (record['allocator_peak_used_before'] < record['allocator_used_before']
            or record['allocator_peak_used_after'] < record['allocator_used_after_release']
            or record['allocator_peak_used_after_consumption'] < record['allocator_used_after_consumption']):
        return False
    cycles = 4 if spec['lifecycle'] == 'reset' else 1
    complete = spec['lifecycle'] in ('complete', 'yield', 'reset')
    limit = spec['lifecycle'] == 'limit'
    expected_outputs = spec['rows'] * cycles if complete else 7 if spec['lifecycle'] == 'cancel_output' else 0
    if (record['operator_instances'] != 1 or record['cleanup_live_rows'] != 0
        or record['cycles'] != cycles or record['output_rows'] != expected_outputs
        or record['limit_rejections'] != int(limit)
        or record['input_fetches'] > cycles * (3 * spec['rows'] + 3)
        or record['input_fetches'] < record['input_rows'] + record['input_yields']):
        return False
    if complete and record['input_rows'] != cycles * spec['rows']:
        return False
    if limit and record['input_rows'] != spec.get('limit_at', 1):
        return False
    if spec['lifecycle'] in ('yield', 'cancel_yield') and not (record['input_yields'] > 0 and record['output_yields'] > 0):
        return False
    if not spec['rows']:
        return record['peak_live_rows'] == 0 and record['allocator_offloads'] == 0
    if record['peak_live_rows'] == 0:
        return False
    if not limit and not (record['allocator_offloads'] > 0 and record['allocator_offloaded_bytes'] > 0):
        return False
    return True


def parse_verdicts(stdout, count, exit_code):
    specifications = generator.specs()
    if type(count) is not int or count != len(specifications) or not stdout or len(stdout) > 1024 * 1024:
        raise MetadataError('invalid_runtime_suite_size')
    records = []
    allowed = {'case_index', 'checks_executed', 'passed', *generator.METRIC_FIELDS}
    for line in stdout.splitlines():
        value = json.loads(line)
        if not isinstance(value, dict) or set(value) != allowed:
            raise MetadataError('unexpected_runtime_verdict_fields')
        if (type(value['case_index']) is not int or not 0 <= value['case_index'] < count
            or type(value['passed']) is not bool
            or type(value['checks_executed']) is not int or value['checks_executed'] <= 0
            or any(type(value[key]) is not int or value[key] < 0 for key in generator.METRIC_FIELDS)):
            raise MetadataError('invalid_runtime_verdict_types')
        if value['passed'] and not _case_observations(value, specifications[value['case_index']]):
            raise MetadataError('runtime_success_disagrees_with_observations')
        records.append(value)
    if len(records) != count or sorted(value['case_index'] for value in records) != list(range(count)):
        raise MetadataError('incomplete_or_duplicate_runtime_cases')
    failures = sum(not value['passed'] for value in records)
    if type(exit_code) is not int or exit_code != int(bool(failures)):
        raise MetadataError('runtime_exit_status_disagrees_with_verdicts')
    return sorted(records, key=lambda value: value['case_index'])


def validate_fixed_runtime(evidence, revision):
    """Require successful actual compile/link/native evidence for this generator."""
    if not isinstance(evidence, dict):
        return False
    try:
        if (evidence.get('schema_version') != 'anonymous_direct_cpp_v1'
            or evidence.get('bug') != 'Q01' or evidence.get('phase') != 'fixed'
            or evidence.get('revision') != revision or not re.fullmatch('[0-9a-f]{40}', revision)
            or evidence.get('status') != 'direct_local_contracts_passed'
            or evidence.get('closure_provenance_verified') is not True
            or evidence.get('generated_files_persisted') is not False
            or evidence.get('proof_scope') != PROOF_SCOPE
            or evidence.get('generator_module') != 'quality_range_runtime_cpp_generator'
            or type(evidence.get('case_count')) is not int or evidence['case_count'] != len(generator.cases())
            or evidence.get('generator_sha256') != digest_file(generator.__file__)['sha256']):
            return False
        source = generator.render_cpp_suite().encode()
        if (evidence.get('generated_source_sha256') != hashlib.sha256(source).hexdigest()
            or type(evidence.get('generated_source_bytes')) is not int
            or evidence['generated_source_bytes'] != len(source)):
            return False
        del source
        if not _digest(evidence.get('closure_receipt_sha256')):
            return False
        raw_receipt = Path(evidence['closure_receipt_path']).read_bytes()
        receipt = json.loads(raw_receipt)
        if (hashlib.sha256(raw_receipt).hexdigest() != evidence['closure_receipt_sha256']
            or receipt.get('schema_version') != CLOSURE_VERSION
            or receipt.get('revision') != revision
            or type(receipt.get('build', {}).get('exit_code')) is not int
            or receipt['build']['exit_code'] != 0
            or receipt.get('build', {}).get('revision') != revision
            or receipt.get('build', {}).get('target') != 'ydb/core/kqp/runtime/streaming/ut'
            or receipt.get('metadata', {}).get('profile') != generator.PROFILE or not receipt.get('files')):
            return False
        for field in ('generated_object', 'generated_executable'):
            item = evidence.get(field, {})
            if not _digest(item.get('sha256')) or type(item.get('size_bytes')) is not int or item['size_bytes'] <= 0:
                return False
        for phase in ('compile', 'link', 'native'):
            process = evidence.get(phase, {})
            if (type(process.get('exit_code')) is not int or process['exit_code'] != 0
                or process.get('cleanup_complete') is not True or process.get('watchdog_stopped') is not False
                or process.get('stdout_overflow') is not False):
                return False
        records = parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in evidence['cases']),
                                 evidence['case_count'], evidence['native']['exit_code'])
        return all(value['passed'] for value in records)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, MetadataError):
        return False
