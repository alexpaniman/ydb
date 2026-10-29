"""Compositional C15/C17/C18 checks; rendered queries exist only in caller memory.

These are candidate regression recipes, not reproduced historical inputs.
Native baseline rejection on the intended rewrite path must be established
before a patched-only success is counted as a verified fix. Use the standard
fixture from generate_and_verify.schema_and_seeds with n >= 2; empty inputs
are derived with a predicate, so fixture creation stays valid.

There is intentionally no executable main or file/output operation here.
"""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import json
import hashlib


_source = Path(__file__).resolve().parents[1] / 'rbo-review-20261021' / 'generate_and_verify.py'
_spec = spec_from_file_location('_metadata_relational_builders', _source)
g = module_from_spec(_spec)
_spec.loader.exec_module(g)


CONFIG_NEEDS = {
    'baseline': 'the recorded main base with new RBO forced and fallback disabled',
    'patched': 'the independently based C17 or C18 branch with the same settings',
    'legacy_control': 'legacy optimizer with otherwise identical fixture/settings',
    'table_service': {'EnableFallbackToYqlOptimizer': False},
    'pragmas': {'YqlSelect': 'force', 'kikimr.CostBasedOptimizationLevel': '0'},
    'fixture': 'standard t(id Int64 NOT NULL, k Int64 NOT NULL), id=1..n, k=1',
    'nodes': 1,
    'coverage_status': 'candidate_until_baseline_failure_on_target_path',
    'result_comparison': 'typed unordered multiset; do not compare row order or raw bytes',
}

EXPECTED_FAILURES = {
    'c15_demanded_uncorrelated': 'scalar cardinality',
    'c15_partly_demanded_uncorrelated': 'scalar cardinality',
    'c15_demanded_correlated': 'scalar cardinality',
}

NEW_PARSER_MULTIPLICITY = {
    'c18_two_empty_sets_empty',
    'c18_two_empty_sets_nonempty',
}


def _named(name, query):
    return '$' + name + ' = (' + query + '); '


def _cross(left, right):
    return left + ' CROSS JOIN ' + right


def _grouping_sets(groups):
    return g.call('GROUPING SETS', *['(' + ', '.join(group) + ')' for group in groups])


def requirements(case, mode='new'):
    return {'generator_version': 'metadata_pruning_invariants_v2',
            'table_service_overrides': CONFIG_NEEDS['table_service'],
            'pragmas': list(CONFIG_NEEDS['pragmas'].items()), 'node_count': CONFIG_NEEDS['nodes']}


def recipes(n):
    """Return case-name -> in-memory query, with no persistence or printing."""
    if n < 2:
        raise ValueError('metadata checks require at least two seeded rows')
    result = {}
    integer = lambda value: g.typed('Int64', value)
    scalar = lambda query: '(' + query + ')'
    many = scalar(g.select(['inner_row.id'], 't AS inner_row'))
    correlated = scalar(g.select(['inner_row.id'], 't AS inner_row',
                                 where=g.op('inner_row.k', '=', 'outer_row.k')))
    never = g.op('outer_row.id', '<', integer(0))
    always = g.op('outer_row.id', '>', integer(0))
    some = g.op('outer_row.id', '=', integer(1))
    for label, guard, value in [
            ('unneeded_uncorrelated', never, many),
            ('demanded_uncorrelated', always, many),
            ('partly_demanded_uncorrelated', some, many),
            ('unneeded_correlated', never, correlated),
            ('demanded_correlated', always, correlated)]:
        result['c15_' + label] = g.select(
            [g.alias(g.call('IF', guard, value, integer(n)), 'value')], 't AS outer_row')
    present = g.call('IF', always, g.call('Just', 'outer_row.id'), g.call('Nothing', 'Int64?'))
    result['c15_coalesce_unneeded'] = g.select(
        [g.alias(g.call('COALESCE', present, many), 'value')], 't AS outer_row')
    for label, predicate in [('one_row_control', g.op('inner_row.id', '=', integer(1))),
                             ('zero_rows_control', 'FALSE')]:
        value = scalar(g.select(['inner_row.id'], 't AS inner_row', where=predicate))
        result['c15_' + label] = g.select([g.alias(value, 'value')], 't AS outer_row')
    live = g.op('id', '+', integer(n))
    dead = g.op(g.op('id', '*', integer(n)), '+', integer(1))
    shared = g.select(['id', 'k', g.alias(live, 'live'), g.alias(dead, 'dead')], 't')
    primary = g.select([g.alias(g.call('SUM', 'dead'), 'unused')], '$shared')
    prefix = _named('shared', shared) + _named('primary', primary)
    survivor = '$shared AS secondary'
    unused_primary = '$primary AS primary_port'
    summed = [g.alias(g.call('SUM', 'secondary.live'), 'total')]
    for ordering, source in [('primary_first', _cross(unused_primary, survivor)),
                             ('secondary_first', _cross(survivor, unused_primary))]:
        result['c17_secondary_singleton_' + ordering] = prefix + g.select(summed, source)
    result['c17_primary_retained_control'] = prefix + g.select(
        summed + [g.alias(g.call('MAX', 'primary_port.unused'), 'retained')],
        _cross(unused_primary, survivor))
    result['c17_two_secondary_ports_control'] = prefix + g.select(
        summed, _cross(_cross(unused_primary, survivor), '$shared AS another_secondary'))
    result['c17_no_dead_definition_control'] = _named(
        'shared', g.select(['id', 'k', g.alias(live, 'live')], 't')) + g.select(summed, survivor)
    # Removing an inactive subplan happens before FinishLogicalRewrite; unlike
    # removing a resultless aggregate during cleanup, this can leave a lone
    # secondary port while the producer still has its pre-pruning schema.
    scalar_primary = g.select([g.alias(scalar(primary), 'unused')])
    scalar_prefix = _named('shared', shared) + _named('primary', scalar_primary)
    result['c17_inactive_primary_subplan'] = scalar_prefix + g.select(summed, _cross(unused_primary, survivor))
    result['c17_inactive_primary_subplan_secondary_first'] = scalar_prefix + g.select(summed, _cross(survivor, unused_primary))
    result['c17_active_primary_subplan_control'] = scalar_prefix + g.select(
        summed + [g.alias(g.call('MAX', 'primary_port.unused'), 'retained')],
        _cross(unused_primary, survivor))

    for empty in (False, True):
        state = 'empty' if empty else 'nonempty'
        source = g.derived(g.select(['id', 'k'], 't', where='FALSE' if empty else None), 'input_rows')
        unused_aggregate = [g.alias(g.call('SUM', 'id'), 'unused')]
        for label, groups in [('one_empty_set', [[]]), ('two_empty_sets', [[], []]),
                              ('key_and_empty_set', [['k'], []])]:
            inner = g.select(unused_aggregate, source, group=_grouping_sets(groups))
            result['c18_' + label + '_' + state] = g.select(
                [g.alias(integer(1), 'marker')], g.derived(inner, 'discarded_result'))
            if label == 'two_empty_sets':
                result['c18_retained_two_empty_sets_' + state] = g.select(
                    [g.alias(integer(1), 'marker'), 'unused'], g.derived(inner, 'retained_result'))
        scalar = g.select(unused_aggregate, source)
        result['c18_scalar_control_' + state] = g.select(
            [g.alias(integer(1), 'marker')], g.derived(scalar, 'discarded_result'))
        grouped = g.select(unused_aggregate, source, group='k')
        result['c18_keyed_control_' + state] = g.select(
            [g.alias(integer(1), 'marker')], g.derived(grouped, 'discarded_result'))
        indicators = g.select([g.alias(g.call('GROUPING', 'k'), 'indicator')], source,
                              group=_grouping_sets([['k'], []]))
        result['c18_grouping_indicator_control_' + state] = g.select(
            ['indicator'], g.derived(indicators, 'grouping_result'))
    return result


def oracles(n):
    """Expected result rows, generated in memory for the documented fixture."""
    if n < 2:
        raise ValueError('metadata checks require at least two seeded rows')
    total = sum(value + n for value in range(1, n + 1))
    result = {
        'c15_unneeded_uncorrelated': [{'value': n} for _ in range(n)],
        'c15_unneeded_correlated': [{'value': n} for _ in range(n)],
        'c15_coalesce_unneeded': [{'value': value} for value in range(1, n + 1)],
        'c15_one_row_control': [{'value': 1} for _ in range(n)],
        'c15_zero_rows_control': [{'value': None} for _ in range(n)],
        'c17_secondary_singleton_primary_first': [{'total': total}],
        'c17_secondary_singleton_secondary_first': [{'total': total}],
        'c17_primary_retained_control': [{'total': total, 'retained': sum(value * n + 1 for value in range(1, n + 1))}],
        'c17_two_secondary_ports_control': [{'total': total * n}],
        'c17_no_dead_definition_control': [{'total': total}],
        'c17_inactive_primary_subplan': [{'total': total}],
        'c17_inactive_primary_subplan_secondary_first': [{'total': total}],
        'c17_active_primary_subplan_control': [{'total': total, 'retained': sum(value * n + 1 for value in range(1, n + 1))}],
    }
    for empty in (False, True):
        state = 'empty' if empty else 'nonempty'
        for label, count in [('one_empty_set', 1),
                             ('key_and_empty_set', 1 if empty else 2),
                             ('scalar_control', 1), ('keyed_control', 0 if empty else 1)]:
            result['c18_' + label + '_' + state] = [{'marker': 1} for _ in range(count)]
        result['c18_grouping_indicator_control_' + state] = (
            [{'indicator': 1}] if empty else [{'indicator': 0}, {'indicator': 1}])
    return result


def semantic_oracle(case, rows, n):
    if case in EXPECTED_FAILURES:
        # Reaching the successful-result oracle is itself a failed control.
        return {'semantic_oracle_pass': False, 'expected_outcome': EXPECTED_FAILURES[case]}
    if case in NEW_PARSER_MULTIPLICITY or case.startswith('c18_retained_two_empty_sets_'):
        # Compare pruning with the retained-output control under the same new
        # parser. This deliberately makes no engine-wide legacy-compatibility
        # claim: the legacy parser deduplicates repeated empty grouping sets.
        retained = case.startswith('c18_retained_')
        empty = case.endswith('_empty')
        expected = [{'marker': 1} for _ in range(2)]
        if retained:
            for row in expected:
                row['unused'] = None if empty else n * (n + 1) // 2
        canonical = lambda values: sorted(json.dumps(value, sort_keys=True, separators=(',', ':')) for value in values)
        projection = [{'marker': row['marker']} for row in rows]
        return {
            'semantic_oracle_pass': canonical(rows) == canonical(expected),
            'semantic_oracle_scope': 'same_new_parser_pruning_invariant',
            'legacy_duplicate_empty_set_compatibility': 'separate_unresolved_contract',
            'metamorphic_pair': 'c18_empty' if empty else 'c18_nonempty',
            'metamorphic_role': 'retained' if retained else 'pruned',
            'metamorphic_projection_sha256': hashlib.sha256('\n'.join(canonical(projection)).encode()).hexdigest(),
        }
    canonical = lambda values: sorted(json.dumps(value, sort_keys=True, separators=(',', ':')) for value in values)
    return {'semantic_oracle_pass': canonical(rows) == canonical(oracles(n)[case])}
