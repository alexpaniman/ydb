"""Composable logical-rule checks; never persist or print rendered inputs.

Import this module in the sanitized runner. recipes(n) creates query strings
only in memory. metadata() describes configuration and comparison needs;
check_rows() consumes already parsed result rows and returns booleans only.
Use the imported schema_and_seeds() for the shared l/r/t fixtures.
"""

import importlib.util
from pathlib import Path


_REVIEW = Path(__file__).resolve().parent.parent / 'rbo-review-20261021'
_SPEC = importlib.util.spec_from_file_location(
    '_rbo_review_builders', _REVIEW / 'generate_and_verify.py')
_BUILDERS = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_BUILDERS)

alias = _BUILDERS.alias
call = _BUILDERS.call
derived = _BUILDERS.derived
join = _BUILDERS.join
op = _BUILDERS.op
quote = _BUILDERS.quote
select = _BUILDERS.select
typed = _BUILDERS.typed
schema_and_seeds = _BUILDERS.schema_and_seeds


CASES = {
    'c01_mixed_where_left': ('C01', 'mixed key types, left WHERE predicate'),
    'c01_mixed_where_right': ('C01', 'mixed key types, right WHERE predicate'),
    'c01_mixed_on_left': ('C01', 'mixed key types, left ON predicate'),
    'c01_mixed_on_right': ('C01', 'mixed key types, right ON predicate'),
    'c01_same_where': ('C01', 'same-type WHERE propagation control'),
    'c01_same_on': ('C01', 'same-type ON propagation control'),
    'c02_volatile_group': ('C02', 'nonrepeatable filter after grouping'),
    'c02_volatile_mixed': ('C02', 'mixed deterministic and nonrepeatable conjuncts'),
    'c02_deterministic_group': ('C02', 'deterministic grouping-key pushdown control'),
    'c02_unfiltered_group': ('C02', 'full-group aggregate control'),
    'c04_false_guard': ('C04', 'rejected false inner predicate guards fallible outer predicate'),
    'c04_null_guard': ('C04', 'rejected NULL inner predicate guards fallible outer predicate'),
    'c04_nonthrowing': ('C04', 'ordinary filter fusion control'),
    'c04_three_levels': ('C04', 'three-filter fusion and fixed-point reporting'),
    'c16_scalar_subplan': ('C16', 'unresolved scalar result above block cross join'),
    'c16_membership_subplan': ('C16', 'unresolved membership result above block cross join'),
    'c16_mixed_predicates': ('C16', 'independent conjuncts alongside unresolved scalar result'),
    'c16_resolved_control': ('C16', 'resolved non-equality block cross-join predicate'),
    'c16_force_inline_control': ('C16', 'forced late-inlining bypass control'),
    'c16_nonblock_control': ('C16', 'nonblock cross-join bypass control'),
    'c16_equi_control': ('C16', 'equality join bypass control'),
}

# The pinned main host rejects Pg syntax before optimizer entry. Keep these
# compositional probes available, but exclude them from executable coverage.
PG_CASES = {
    'c04_pg_false_guard': ('C04', 'Pg boolean inner rejection guards fallible outer predicate'),
    'c04_pg_null_guard': ('C04', 'nullable Pg boolean inner rejection guards fallible outer predicate'),
}


def _validate_n(n):
    if not isinstance(n, int) or not 8 <= n <= 1000:
        raise ValueError('Use an integer fixture size between 8 and 1000')


def _and(*predicates):
    result = predicates[0]
    for predicate in predicates[1:]:
        result = op(result, 'AND', predicate)
    return result


def _typed_join(n, left_type, right_type, filtered_side, location):
    left = derived(select([alias(typed(left_type, n), 'k')]), 'a')
    right = derived(select([alias(typed(right_type, n), 'k')]), 'b')
    equality = op('a.k', '=', 'b.k')
    predicate = op('-' + filtered_side + '.k', '<', typed('Int32', 0))
    condition = _and(equality, predicate) if location == 'on' else equality
    return select([alias('a.k', 'left_key'), alias('b.k', 'right_key')],
                  join(left, right, condition),
                  where=predicate if location == 'where' else None)


def _fusion(n, nullable=False, fallible=True, three_levels=False, pg=False):
    if pg:
        threshold = str(n // 2)
        inner_guard = op('id', '>', threshold)
        if nullable:
            inner_guard = 'CASE WHEN ' + inner_guard + ' THEN TRUE ELSE NULL END'
        inner = select(['id'], 't', where=inner_guard)
        divisor = op('q.id', '-', threshold)
        predicate = op(op('1', '/', divisor), '>=', '0')
        return 'SET TablePathPrefix = ' + quote('/Root/') + '; ' + select(
            [alias('q.id', 'id')], derived(inner, 'q'), where=predicate)
    threshold = typed('Int64', n // 2)
    inner_guard = op('id', '>', threshold)
    if nullable:
        inner_guard = call('IF', inner_guard, 'TRUE', call('Nothing', 'Bool?'))
    inner = select(['id'], 't', where=inner_guard)
    outer_guard = op('q.id', '>', threshold)
    predicate = (op(call('Ensure', 'q.id', outer_guard, quote('filter guard')),
                    '>', typed('Int64', 0)) if fallible
                 else op('q.id', '<', typed('Int64', n)))
    result = select([alias('q.id', 'id')], derived(inner, 'q'), where=predicate)
    if three_levels:
        result = select(['z.id'], derived(result, 'z'),
                        where=op('z.id', '<', typed('Int64', n)))
    return result


def recipes(n):
    """Return inputs in memory. This function performs no IO or execution."""
    _validate_n(n)
    result = {}
    for location in ('where', 'on'):
        result['c01_mixed_' + location + '_left'] = _typed_join(
            n, 'Int32', 'Uint32', 'a', location)
        result['c01_mixed_' + location + '_right'] = _typed_join(
            n, 'Uint32', 'Int32', 'b', location)
        result['c01_same_' + location] = _typed_join(
            n, 'Int32', 'Int32', 'a', location)

    grouped = select(['k', alias(call('COUNT', '*'), 'n')], 't', group='k')
    volatile = op(call('Random', 'k'), '<', '0.5')
    deterministic = op('k', '=', typed('Int64', 1))
    result['c02_volatile_group'] = select(['k', 'n'], derived(grouped, 'g'),
                                           where=volatile)
    result['c02_volatile_mixed'] = select(['k', 'n'], derived(grouped, 'g'),
                                           where=_and(deterministic, volatile))
    result['c02_deterministic_group'] = select(['k', 'n'], derived(grouped, 'g'),
                                                 where=deterministic)
    result['c02_unfiltered_group'] = grouped

    result['c04_false_guard'] = _fusion(n)
    result['c04_null_guard'] = _fusion(n, nullable=True)
    result['c04_nonthrowing'] = _fusion(n, fallible=False)
    result['c04_three_levels'] = _fusion(n, three_levels=True)

    cross = 'l AS a' + ' CROSS JOIN ' + 'r AS b'
    fields = [alias('a.id', 'left_id'), alias('b.id', 'right_id')]
    scalar = '(' + select([call('MAX', 'id')], 't') + ')'
    scalar_predicate = op('a.k', '<', scalar)
    membership = '(' + select(['id'], 't',
                               where=op('id', '<=', typed('Int64', 4))) + ')'
    result['c16_scalar_subplan'] = select(fields, cross, where=scalar_predicate)
    result['c16_membership_subplan'] = select(fields, cross,
                                               where=op('a.k', 'IN', membership))
    result['c16_mixed_predicates'] = select(fields, cross, where=_and(
        op('a.k', '>', typed('Int32', 0)),
        op('a.id', '<=', 'b.id'), scalar_predicate))
    result['c16_resolved_control'] = select(fields, cross, where=_and(
        op('a.k', '<', typed('Int32', n)), op('a.id', '<=', 'b.id')))
    result['c16_force_inline_control'] = result['c16_scalar_subplan']
    result['c16_nonblock_control'] = result['c16_scalar_subplan']
    result['c16_equi_control'] = select(fields,
        join('l AS a', 'r AS b', op('a.id', '=', 'b.id')),
        where=scalar_predicate)
    assert set(result) == set(CASES)
    return result


def unavailable_pg_recipes(n):
    _validate_n(n)
    return {'c04_pg_false_guard': _fusion(n, pg=True),
            'c04_pg_null_guard': _fusion(n, nullable=True, pg=True)}


def metadata(case, n):
    """Configuration and human-readable assertions; contains no input text."""
    _validate_n(n)
    bug, purpose = (CASES if case in CASES else PG_CASES)[case]
    volatile = case.startswith('c02_volatile_')
    result = {
        'bug': bug,
        'purpose': purpose,
        'deterministic': not volatile,
        'compare_legacy_hash': not volatile,
        'repetitions': 8 if volatile else 1,
        'table_service_overrides': {},
        'pragmas': [],
        'assertions': ['Successful execution', 'check_rows returns semantic_oracle_pass'],
        'plan_assertions': [],
    }
    if volatile:
        result['assertions'] += [
            'Every emitted group retains all input rows in its aggregate',
            'Do not compare random row selections across executions',
            'At least one repetition should emit a group to avoid vacuous coverage',
        ]
        result['plan_assertions'] = ['Nonrepeatable predicate remains above aggregation']
    elif case == 'c02_deterministic_group':
        result['plan_assertions'] = ['Grouping-key predicate remains eligible for pushdown']
    elif case.startswith('c01_same_'):
        result['plan_assertions'] = ['Equal-type key predicate can still propagate to opposite input']
    elif bug == 'C04':
        result['assertions'] += ['Inner-filter rejection prevents fallible outer evaluation']
        result['plan_assertions'] = ['Adjacent filters fuse with a lazy rejection boundary']
        if case.startswith('c04_pg_'):
            result['query_syntax'] = 'pg'
            result['generator_version'] = 'pg-filter-controls-v2'
    if bug == 'C16':
        result['table_service_overrides'] = {
            'UseBlockHashJoin': True,
            'UseBlockHashJoinForCross': case != 'c16_nonblock_control',
            'EnableInlineJoinFiltersAfterCBO': case == 'c16_force_inline_control',
        }
        result['assertions'] += ['Unavailable subplan bindings do not enter join-filter annotation']
        if case == 'c16_resolved_control':
            result['plan_assertions'] = ['Resolved non-equality predicates remain eligible for block join filters']
        elif case == 'c16_mixed_predicates':
            result['plan_assertions'] = ['Independent conjuncts retain pushdown eligibility']
    return result


def check_rows(case, rows, n):
    """Consume parsed JSON rows and return only sanitized oracle booleans."""
    _validate_n(n)
    if case not in CASES and case not in PG_CASES:
        raise KeyError('Unknown logical check label')
    try:
        if case.startswith('c01_'):
            valid = (len(rows) == 1 and rows[0]['left_key'] == n
                     and rows[0]['right_key'] == n)
        elif case.startswith('c02_'):
            full_groups = all(row['k'] == 1 and row['n'] == n for row in rows)
            valid = full_groups and (len(rows) <= 1 if case.startswith('c02_volatile_')
                                     else len(rows) == 1)
        elif case.startswith('c04_'):
            upper = n if case in ('c04_nonthrowing', 'c04_three_levels') else n + 1
            values = [int(row['id']) if case.startswith('c04_pg_') else row['id'] for row in rows]
            valid = sorted(values) == list(range(n // 2 + 1, upper))
        else:
            valid = (len(rows) == 1 and rows[0]['left_id'] == 1
                     and rows[0]['right_id'] == 1)
    except (KeyError, TypeError, ValueError):
        return {'semantic_oracle_pass': False, 'result_shape_valid': False}
    result = {'semantic_oracle_pass': bool(valid), 'result_shape_valid': True}
    if case.startswith('c02_volatile_'):
        result['nonvacuous_group_observed'] = bool(rows)
    return result
