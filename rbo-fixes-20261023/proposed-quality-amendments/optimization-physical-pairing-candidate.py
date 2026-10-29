"""Pair sanitized quality evidence; never load or reconstruct SQL/AST/plans."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

from verification_contract import record_verdict

HERE = Path(__file__).resolve().parent
REVIEW = HERE.parent / 'rbo-review-20261021'
PROFILES = {
    'window': ('quality_window_checks',), 'sum': ('quality_sum_checks',),
    'minmax': ('quality_minmax_checks',), 'range_minmax': ('quality_range_minmax_checks',),
    'rules': ('quality_rule_checks',), 'partition': ('quality_partition_checks',),
    'partition_subset': ('quality_partition_subset_checks',),
    'lookup': ('quality_lookup_checks',), 'union': ('quality_union_checks',),
    'statistics': ('quality_statistics_checks',),
}
AUTO = {
    **dict.fromkeys(('Q01', 'Q02', 'Q10', 'Q13', 'Q22'), ('window',)),
    **dict.fromkeys(('Q03', 'Q04', 'Q06', 'Q07', 'Q17', 'Q23', 'Q24', 'Q29'), ('lookup',)),
    **dict.fromkeys(('Q05', 'Q12', 'Q21'), ('rules',)),
    **dict.fromkeys(('Q08', 'Q09', 'Q20', 'Q26'), ('statistics',)),
    'Q01': ('window', 'sum', 'minmax', 'range_minmax'), 'Q11': ('union',), 'Q15': ('partition_subset',),
}


def generator_modules(bug, profiles=(), extras=()):
    chosen = profiles or AUTO.get(bug, ())
    modules = [module for profile in chosen for module in PROFILES[profile]]
    modules.extend(extras)
    for module in modules:
        if not re.fullmatch('[a-z][a-z0-9_]*', module) or not (HERE / (module + '.py')).is_file():
            raise ValueError('Generator module must be a local artifact')
    return list(dict.fromkeys(modules))


def selected_cases(bug, modules, rows):
    """Resolve generator ownership before an expensive build, keeping SQL in RAM."""
    if str(REVIEW) not in sys.path:
        sys.path.insert(0, str(REVIEW))
    owners = {}
    for name in modules:
        path = HERE / (name + '.py')
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
        for case in module.recipes(rows):
            if not case.startswith(bug.lower() + '_'):
                continue
            if case in owners:
                raise ValueError('Selected quality profiles have ambiguous case ownership')
            owners[case] = name
    if not owners:
        raise ValueError('Selected quality profiles contain no cases for ' + bug)
    return owners


def profile_fingerprint(modules):
    """Fingerprint local generator imports and harness inputs without importing."""
    todo = [HERE / (name + '.py') for name in modules]
    todo += [HERE / name for name in ('native_verify.py', 'verification_contract.py', 'quality_pairing.py')]
    files = {}
    while todo:
        path = todo.pop().resolve()
        if str(path) in files:
            continue
        data = path.read_bytes()
        files[str(path)] = hashlib.sha256(data).hexdigest()
        for node in ast.walk(ast.parse(data)):
            imported = ([alias.name for alias in node.names] if isinstance(node, ast.Import)
                        else [node.module] if isinstance(node, ast.ImportFrom) and node.module else [])
            for module in imported:
                for folder in (HERE, REVIEW):
                    candidate = folder / (module.split('.')[0] + '.py')
                    if candidate.is_file():
                        todo.append(candidate)
    for name in ('new.conf', 'legacy.conf'):
        path = REVIEW / name
        files[str(path.resolve())] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def record_key(record):
    return tuple(record.get(key) for key in ('case', 'mode', 'rows', 'repetition'))


def _digest(value):
    return isinstance(value, str) and bool(re.fullmatch('[0-9a-f]{64}', value))


def _provenance(record, phase, revision):
    return (record.get('phase') == phase and record.get('revision') == revision
            and record.get('binary_provenance_verified') is True
            and _digest(record.get('build_receipt_sha256'))
            and isinstance(record.get('binary_provenance'), dict)
            and _digest(record['binary_provenance'].get('sha256')))


def _matching(before, after):
    fields = ('case', 'mode', 'rows', 'repetition', 'generator_module', 'generator_sha256',
              'generator_version', 'settings_sha256', 'schema_sha256', 'fixture_sha256',
              'query_sha256', 'validation_sha256', 'query_syntax', 'node_count',
              'table_service_overrides', 'quality_profile_sha256')
    return (all(field in before and field in after and before[field] == after[field] for field in fields)
            and all(_digest(after.get(field)) for field in (
                'generator_sha256', 'settings_sha256', 'schema_sha256', 'query_sha256', 'quality_profile_sha256'))
            and type(after.get('node_count')) is int and after['node_count'] > 0)


def _count(record, name):
    value = record.get('callables', {}).get(name)
    if type(value) is not int or value < 0:
        raise ValueError('Missing or invalid callable counter')
    return value


def _range_calls(record):
    value = record.get('range_minmax_calls')
    if type(value) is not int or value < 0:
        raise ValueError('Missing or invalid RANGE callable count')
    return value


def _sum_field(record, collection, field):
    entries = record.get(collection)
    if not isinstance(entries, list):
        raise ValueError('Missing descriptor list')
    values = [entry[field] for entry in entries]
    if any(type(value) not in (int, bool) or value < 0 for value in values):
        raise ValueError('Invalid descriptor count')
    return sum(values)


def _index_count(record, collection):
    entries = record.get(collection)
    if not isinstance(entries, list):
        raise ValueError('Missing source descriptors')
    return sum(item.get('table_class') == 'right_index' for item in entries)


def _topn(record):
    names = record.get('row_read_setting_names')
    if not isinstance(names, list):
        raise ValueError('Missing storage settings')
    return sum('ItemsLimit' in item and ('Sorted' in item or 'Reverse' in item) for item in names)


def _partial_window_state_width(record):
    summary = record.get('window_frame_fold_state')
    if not isinstance(summary, dict) or summary.get('version') != 'window_frame_fold_state_v1' or summary.get('complete') is not True:
        raise ValueError('Missing complete window state descriptor')
    folds = summary.get('folds')
    if not isinstance(folds, list) or len(folds) != 1 or not isinstance(folds[0], dict):
        raise ValueError('Expected one retained frame fold')
    fold = folds[0]
    init, update, optional = (fold.get(name) for name in ('init_fields', 'update_fields', 'optional_count_fields'))
    if (any(type(value) is not int or value < 1 for value in (init, update, optional))
            or init != update or optional > init or fold.get('state_fields_match') is not True
            or fold.get('collector_queue_bound') is not True
            or type(fold.get('init_arity')) is not int or type(fold.get('update_arity')) is not int
            or fold.get('init_arity') != 1 or fold.get('update_arity') != 2):
        raise ValueError('Invalid retained window state descriptor')
    return init


def _same_callable_shape(before, after):
    left, right = before.get('callables'), after.get('callables')
    return (isinstance(left, dict) and bool(left) and isinstance(right, dict) and left == right
            and all(type(value) is int and value >= 0 for counts in (left, right) for value in counts.values()))


def _improvement(case, before, after):
    """Known measurable contracts only; unknown/missing evidence stays pending."""
    bug = case[:3].upper()
    if case.endswith(('_control', '_empty', '_all_null')) and case != 'q01_sum_native_control':
        return False
    b, a = before.get('optimizer_estimates', []), after.get('optimizer_estimates', [])
    if bug == 'Q01' and after.get('structural_assertions', {}).get('count_has_no_frame_fold'):
        return _count(before, 'Fold') + _count(before, 'Fold1') > 0
    if case.startswith('q01_range_minmax_'):
        return (after.get('optimization_acceptance_case') is True
                and after.get('structural_assertions', {}).get('incremental_range_callable_present') is True
                and _range_calls(before) == 0 and _range_calls(after) > 0
                and _count(before, 'Fold1') > 0)
    if case.startswith('q01_minmax_') and after.get('structural_assertions', {}).get('no_general_frame_fold'):
        return _count(before, 'Fold1') > _count(after, 'Fold1')
    if case == 'q02_partial':
        return _partial_window_state_width(before) > _partial_window_state_width(after) == 1
    if bug == 'Q02' and case == 'q02_all_dead':
        return _count(before, 'QueueCreate') + _count(before, 'WinFramesCollector') > 0
    if bug in ('Q03', 'Q04'):
        return _index_count(after, 'row_sources') > _index_count(before, 'row_sources')
    if bug == 'Q05':
        return _topn(after) > _topn(before)
    if bug == 'Q06':
        return _index_count(before, 'stream_lookups') > _index_count(after, 'stream_lookups')
    if bug == 'Q07' and case.endswith('_large_right'):
        return _count(before, 'MapJoinCore') > _count(after, 'MapJoinCore')
    if bug == 'Q08' and case == 'q08_nonunit_input':
        return (len(a) == len(b) == 1 and b[0]['input_rows'] == a[0]['input_rows']
                and before.get('quality_assertions', {}).get('local_selectivity_applied_once') is False
                and b[0]['output_rows'] != a[0]['output_rows'])
    if bug == 'Q09' and case.startswith('q09_scalar_'):
        return bool(a) and bool(b) and any(item['rows'] != 1 for item in b) and all(item['rows'] == 1 for item in a)
    if bug == 'Q10':
        return sum(before['switch_output_counts']) > sum(after['switch_output_counts'])
    if bug == 'Q11':
        return _sum_field(after, 'row_sources', 'range_expression_present') > _sum_field(before, 'row_sources', 'range_expression_present')
    if bug == 'Q12':
        return any(_count(after, name) > _count(before, name) for name in ('KqpOlapJsonValue', 'KqpOlapJsonExists'))
    if bug == 'Q13' and case == 'q13_parallel':
        return before.get('sink_flags') != after.get('sink_flags')
    if bug == 'Q15':
        return (case in ('q15_same_partition', 'q15_intervening_filter', 'q15_override_enabled', 'q15_alias_filter', 'q15_payload_map',
                        'q15_strict_superset', 'q15_extra_computed', 'q15_chain_three', 'q15_after_aggregate', 'q15_source_subset')
                and _count(before, 'DqCnHashShuffle') > _count(after, 'DqCnHashShuffle'))
    if bug == 'Q17':
        return _index_count(after, 'stream_lookups') > _index_count(before, 'stream_lookups')
    if bug == 'Q20' and not case.endswith('_control'):
        field = 'bytes' if case.endswith('composite_bytes') else 'rows'
        return len(a) == len(b) == 1 and a[0][field] != b[0][field]
    if bug == 'Q21' and not case.endswith('_control'):
        return sum(_count(before, name) for name in ('ChainMap', 'Chain1Map', 'WideChainMap', 'WideChain1Map', 'QueueCreate')) > 0
    if bug == 'Q22':
        return _count(after, 'Contains') > _count(before, 'Contains')
    if bug == 'Q23':
        return _sum_field(before, 'stream_lookups', 'payload_fields') > _sum_field(after, 'stream_lookups', 'payload_fields')
    if bug == 'Q24':
        return _sum_field(before, 'row_sources', 'columns') > _sum_field(after, 'row_sources', 'columns')
    if bug == 'Q26' and '_unset_' not in case:
        return before.get('sequential_read_settings') != after.get('sequential_read_settings')
    if bug == 'Q29' and case in ('q29_primitive_residual', 'q29_optional_residual'):
        return before['comparison_dag_nodes'] > after['comparison_dag_nodes']
    return False


def _fixed_acceptance(before, after):
    case = after.get('case', '')
    if case.startswith('q01_sum_') and case.endswith('_control') and case != 'q01_sum_native_control':
        return (before.get('physical_ast_parsed') is True and after.get('physical_ast_parsed') is True
                and after.get('structural_assertions', {}).get('control_physical_plan_parsed') is True
                and _same_callable_shape(before, after))
    if case in ('q29_primitive_residual', 'q29_optional_residual'):
        left, right = before.get('comparison_dag_nodes'), after.get('comparison_dag_nodes')
        if type(left) is not int or type(right) is not int or not left > right > 0:
            return False
    if case == 'q02_partial' and not _partial_window_state_width(before) > _partial_window_state_width(after) == 1:
        return False
    if re.fullmatch(r'q22_(?:ansi|legacy)_(?:tuple|list)_5_(?:nullable|required)', case):
        left, right = before.get('scalar_equality_dag_nodes'), after.get('scalar_equality_dag_nodes')
        if (type(left) is not int or type(right) is not int or left <= 0 or left != right
                or not _same_callable_shape(before, after)):
            return False
    # The native hooks see only the fixed plan. Preserve the paired effect
    # controls here before accepting their otherwise nonempty assertions.
    if case == 'q11_fallible_control':
        left, right = before.get('ensure_dag_nodes'), after.get('ensure_dag_nodes')
        if type(left) is not int or type(right) is not int or left <= 0 or left != right:
            return False
    if case in ('q29_arithmetic_control', 'q29_guard_control', 'q29_no_range_control'):
        left, right = before.get('comparison_dag_nodes'), after.get('comparison_dag_nodes')
        if type(left) is not int or type(right) is not int or left < 0 or left != right:
            return False
        if case == 'q29_guard_control' and (
                _count(before, 'Ensure') <= 0 or _count(before, 'Ensure') != _count(after, 'Ensure')):
            return False
    if case == 'q01_minmax_range_control' or case.startswith('q01_range_minmax_'):
        if after.get('optimization_acceptance_case') is True:
            assertions = after.get('structural_assertions')
            return (isinstance(assertions, dict) and bool(assertions)
                    and all(value is True for value in assertions.values())
                    and assertions.get('incremental_range_callable_present') is True
                    and _range_calls(before) == 0 and _range_calls(after) > 0
                    and _count(before, 'Fold1') > 0)
        if case.startswith(tuple('q01_range_minmax_' + name + '_' for name in ('running', 'whole', 'float_value'))):
            return (after.get('structural_assertions', {}).get('physical_plan_parsed') is True
                    and _range_calls(before) == _range_calls(after) == 0
                    and isinstance(before.get('callables'), dict) and bool(before['callables'])
                    and before['callables'] == after.get('callables'))
    if after.get('case') in {'q01_minmax_' + kind + '_control' for kind in ('running', 'whole', 'suffix')}:
        # These negative controls promise that unsupported frame shapes keep
        # their existing path. The native hook receives only one AST; perform
        # the paired shape comparison here rather than accepting parsing alone.
        return (after.get('structural_assertions', {}).get('physical_plan_parsed') is True
                and isinstance(before.get('callables'), dict) and bool(before['callables'])
                and before['callables'] == after.get('callables'))
    if after.get('case', '').startswith('q26_'):
        case = after['case']
        expected = [] if '_unset_' in case else [0 if '_zero_' in case else 4]
        return after.get('row_source_count') == 1 and after.get('sequential_read_settings') == [expected]
    assertions = after.get('quality_assertions', after.get('structural_assertions'))
    if isinstance(assertions, dict) and assertions:
        return all(value is True for value in assertions.values())
    # Some controls only promise preserved semantics and physical structure.
    case = after.get('case', '')
    if case.endswith(('_control', '_empty', '_all_null')):
        fields = ('callables', 'switch_output_counts', 'row_read_setting_names')
        return ('callables' in before and 'callables' in after
                and all(before.get(field) == after.get(field) for field in fields))
    return False


def pair_quality_records(before_records, after_records, bug, base, revision, ir_evidence=None,
                         *, required_case_owners=None, required_profile_sha256=None):
    """Return derived sanitized records plus an honest aggregate verdict."""
    before_by_key, duplicates = {}, set()
    for record in before_records:
        key = record_key(record)
        if key in before_by_key:
            duplicates.add(key)
        before_by_key[key] = record
    paired, after_keys = [], set()
    for original in after_records:
        after = dict(original)
        after.update(quality_acceptance_pass=False, quality_improvement_verified=False)
        key = record_key(after)
        before = before_by_key.get(key)
        reasons = []
        if key in after_keys or key in duplicates:
            reasons.append('ambiguous_duplicate_case')
        after_keys.add(key)
        if (after.get('bug') != bug or not after.get('case', '').startswith(bug.lower() + '_')
            or not _provenance(after, 'fixed', revision) or record_verdict(after) != 'functional'):
            reasons.append('fixed_semantics_or_provenance_failed')
        if before is None:
            reasons.append('matching_baseline_missing')
        elif before.get('bug') != bug or not _provenance(before, 'baseline', base) or record_verdict(before) != 'functional':
            reasons.append('baseline_semantics_or_provenance_unavailable')
        elif not _matching(before, after):
            reasons.append('baseline_workload_or_profile_mismatch')
        if not reasons:
            plan_key = 'query_plan_sha256' if after.get('query_plan_parsed') else 'physical_ast_sha256'
            if not all(_digest(record.get(plan_key)) for record in (before, after)):
                reasons.append('paired_plan_evidence_missing')
            else:
                try:
                    accepted = _fixed_acceptance(before, after)
                    ir_accepted = True
                    # Q11 requires exact Ordered/branch-identity/effect-placement
                    # contracts in addition to SQL rows and physical counters.
                    if bug == 'Q11' or after.get('additional_ir_admission_required'):
                        after['additional_ir_admission_required'] = True
                        from anonymous_cpp_verify import validate_fixed_admission
                        ir_accepted = validate_fixed_admission(ir_evidence, bug, revision)
                        if not ir_accepted:
                            reasons.append('additional_ir_admission_pending')
                        else:
                            after['direct_ir_admission_evidence_sha256'] = hashlib.sha256(
                                json.dumps(ir_evidence, sort_keys=True).encode()).hexdigest()
                    if not accepted:
                        reasons.append('fixed_optimization_contract_not_established')
                    elif ir_accepted:
                        after['quality_acceptance_pass'] = True
                        after['quality_improvement_verified'] = bool(_improvement(after['case'], before, after))
                    after['baseline_plan_sha256'] = before[plan_key]
                    after['baseline_build_receipt_sha256'] = before['build_receipt_sha256']
                    after['baseline_record_sha256'] = hashlib.sha256(json.dumps(before, sort_keys=True).encode()).hexdigest()
                except (KeyError, TypeError, ValueError, OverflowError):
                    reasons.append('quality_metric_contract_incomplete')
                    after['quality_acceptance_pass'] = False
                    after['quality_improvement_verified'] = False
        after['quality_pairing_reasons'] = reasons
        after['quality_pairing_version'] = 'required_case_physical_and_paired_controls_v3'
        paired.append(after)
    observed_owners = {record['case']: record.get('generator_module') for record in paired}
    coverage = (isinstance(required_case_owners, dict) and bool(required_case_owners)
                and observed_owners == required_case_owners
                and _digest(required_profile_sha256)
                and all(record.get('quality_profile_sha256') == required_profile_sha256
                        and record.get('generator_module') == required_case_owners.get(record['case'])
                        for record in paired))
    for record in paired:
        record['required_quality_profile_sha256'] = required_profile_sha256
        record['finding_profile_coverage_complete'] = coverage
        if not coverage:
            record['quality_acceptance_pass'] = False
            record['quality_improvement_verified'] = False
            record['quality_pairing_reasons'].append('required_finding_profile_coverage_missing')
    complete = bool(paired) and after_keys == set(before_by_key) and not duplicates and coverage
    semantic = bool(paired) and all(record_verdict(record) == 'functional'
        and _provenance(record, 'fixed', revision) and record.get('bug') == bug for record in paired)
    success = (complete and semantic and all(record['quality_acceptance_pass'] for record in paired)
               and any(record['quality_improvement_verified'] for record in paired))
    status = 'native_failed' if not semantic else 'verified_quality' if success else 'verified_semantics_quality_pending'
    return paired, success, status
