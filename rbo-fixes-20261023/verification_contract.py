"""Strict parsing and verdict contracts; contains no workload inputs."""
import json
import re

from optimizer_mode_contract import optimizer_mode_matches_record


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate result object key')
        result[key] = value
    return result


def parse_rows_output(payload):
    """Read kqprun's rows format, preserving result-set headings and order."""
    rows, result_sets = [], []
    headed = False
    for raw in payload.decode().splitlines():
        if not raw.strip():
            continue
        clean = re.sub(r'\x1b\[[0-9;]*m', '', raw).strip()
        heading = re.fullmatch(r'Result set ([1-9][0-9]*):', clean)
        if heading:
            if rows and not headed:
                raise ValueError('Result heading followed unheaded rows')
            headed = True
            if int(heading.group(1)) != len(result_sets) + 1:
                raise ValueError('Nonconsecutive result-set headings')
            result_sets.append([])
            continue
        row = json.loads(raw, object_pairs_hook=_unique_object)
        if not isinstance(row, dict):
            raise ValueError('Expected one result row object per line')
        rows.append(row)
        if headed:
            result_sets[-1].append(row)
    if not headed and rows:
        result_sets = [rows]
    # With no headings and no rows, rows format cannot distinguish an empty
    # result set from no result sets. Do not claim exact boundary coverage.
    return rows, result_sets, headed or bool(rows)


def failure_stage(diagnostics, setup_count, validation_count=0):
    requests = setup_count + 1 + validation_count
    # The caller supplies stdout, where the runner writes request progress.
    # Require a complete progress line, so quoted diagnostics cannot fabricate
    # a later request boundary.
    diagnostics = re.sub(r'\x1b\[[0-9;]*m', '', diagnostics)
    executions = list(re.finditer(
        r'(?m)^(?:[^\n]*?\s)?Executing query(?: ([0-9]+))?(?:, loop [0-9]+)?\.\.\.\s*$',
        diagnostics))
    if not executions:
        return 'setup_or_startup'
    ordinal = executions[-1].group(1)
    if ordinal is None:
        if requests != 1:
            return 'unknown'
        index = 0
    else:
        index = int(ordinal)
    if index >= requests:
        return 'unknown'
    if index < setup_count:
        return 'setup'
    return 'target' if index == setup_count else 'validation'


def record_verdict(record):
    """Return functional/guard/execution-only/failure, never merge the claims."""
    if not optimizer_mode_matches_record(record):
        return 'failure'
    if record.get('native_batch_guard_pass') is False:
        return 'failure'
    if record.get('correctness_physical_ast_required') and (
            record.get('structural_oracle_pass') is not True
            or record.get('physical_ast_parsed') is not True
            or not re.fullmatch('[0-9a-f]{64}', record.get('physical_ast_sha256', '') or '')):
        return 'failure'
    if record.get('correctness_query_plan_required') and (
            record.get('query_plan_structural_oracle_pass') is not True
            or record.get('query_plan_parsed') is not True
            or not isinstance(record.get('query_plan_sha256'), str)
            or not re.fullmatch('[0-9a-f]{64}', record['query_plan_sha256'])):
        return 'failure'
    if (record.get('phase') in ('fixed', 'baseline') or record.get('binary_provenance_required')):
        if record.get('binary_provenance_verified') is not True:
            return 'failure'
    if record.get('timeout') or record.get('harness_exception'):
        return 'failure'
    if any(record.get(key) is False for key in (
            'column_order_oracle_pass', 'result_binding_order_pass',
            'result_set_oracle_pass', 'result_set_order_pass',
            'result_set_ownership_pass', 'effect_persistence_pass')):
        return 'failure'
    guarded = (record.get('correctness_guard_observed') is True
               and record.get('exit_code', 0) > 0
               and record.get('failure_stage') == 'target')
    if record.get('expected_rejection_configured'):
        return 'guard' if guarded else 'failure'
    if guarded:
        return 'guard'
    if record.get('semantic_oracle_pass') is True:
        if record.get('exit_code') == 0:
            return 'functional'
        if (record.get('expected_failure_observed') is True
                and record.get('exit_code', 0) > 0
                and record.get('failure_stage') == 'target'):
            return 'functional'
        return 'failure'
    if (record.get('exit_code') == 0 and record.get('execution_oracle_pass') is True
            and record.get('result_shape_valid') is True):
        return 'execution_only'
    return 'failure'


def summarize_verification(records, bug):
    if bug.startswith('Q'):
        semantic_success = bool(records) and all(record_verdict(record) == 'functional' for record in records)
        if not semantic_success:
            return False, 'native_failed'
        # Native row equality proves preservation, not optimization quality.
        # A separate paired plan/cost analysis must establish improvement and
        # validate the expected unchanged behavior of negative controls.
        quality_success = (all(record.get('quality_acceptance_pass') is True for record in records)
                           and any(record.get('quality_improvement_verified') is True for record in records))
        return quality_success, 'verified_quality' if quality_success else 'verified_semantics_quality_pending'
    if bug in ('C37', 'C38'):
        grouped = {}
        for record in records:
            key = (record.get('case'), record.get('repetition', 0))
            mode = record.get('mode')
            if mode in grouped.setdefault(key, {}):
                return False, 'native_failed'
            grouped[key][mode] = record
        success = bool(grouped)
        for pair in grouped.values():
            if set(pair) != {'new', 'legacy'}:
                return False, 'native_failed'
            left, right = pair['new'], pair['legacy']
            digest = left.get('unordered_result_sha256')
            success &= (optimizer_mode_matches_record(left, require_exclusive=True)
                        and optimizer_mode_matches_record(right, require_exclusive=True)
                        and record_verdict(left) == record_verdict(right) == 'functional'
                        and left.get('exit_code') == right.get('exit_code') == 0
                        and left.get('semantic_oracle_pass') is True
                        and right.get('semantic_oracle_pass') is True
                        and not left.get('timeout') and not right.get('timeout')
                        and not left.get('harness_exception') and not right.get('harness_exception')
                        and isinstance(digest, str) and bool(re.fullmatch('[0-9a-f]{64}', digest))
                        and digest == right.get('unordered_result_sha256'))
        return success, 'verified' if success else 'native_failed'
    verdicts = [record_verdict(record) for record in records]
    if not verdicts or 'failure' in verdicts:
        return False, 'native_failed'
    # These SQL batches do not execute the separate production-IR contracts.
    # Direct evidence and the recorded scope limits require separate review.
    if bug in ('C31', 'C47'):
        if any(verdict not in ('functional', 'guard') for verdict in verdicts):
            return False, 'native_failed'
        return False, 'sql_checks_passed_direct_pending'
    if bug == 'C02':
        for case in {r['case'] for r in records if 'nonvacuous_group_observed' in r}:
            if not any(r.get('nonvacuous_group_observed') for r in records if r['case'] == case):
                return False, 'native_failed'
    if bug == 'C18':
        pairs = {}
        for record in records:
            pair = record.get('metamorphic_pair')
            if pair is None:
                continue
            role = record.get('metamorphic_role')
            if role in pairs.setdefault(pair, {}):
                return False, 'native_failed'
            pairs[pair][role] = record
        if set(pairs) != {'c18_empty', 'c18_nonempty'}:
            return False, 'native_failed'
        for pair in pairs.values():
            if set(pair) != {'retained', 'pruned'}:
                return False, 'native_failed'
            digest = pair['retained'].get('metamorphic_projection_sha256')
            if (not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest)
                    or digest != pair['pruned'].get('metamorphic_projection_sha256')):
                return False, 'native_failed'
        return True, 'verified_new_parser_invariant'
    if 'execution_only' in verdicts:
        return True, 'verified_execution_only'
    if 'guard' in verdicts:
        return True, 'verified_guard'
    return True, 'verified'
