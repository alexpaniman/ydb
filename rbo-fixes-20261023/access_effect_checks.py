"""C41/C47 compositional checks; rendered inputs and results stay in memory."""
import importlib.util
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location('_access_effect_builders', Path(__file__).resolve().parent.parent / 'rbo-review-20261021' / 'generate_and_verify.py')
g = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(g)
CASE_SPECIFIC_SCHEMA = True


def _statements(*items):
    return '; '.join(items)


def _write(table, fields):
    return 'UPSERT INTO ' + table + ' ' + g.select([g.alias(g.typed(typ, value), name) for name, typ, value in fields])


def _read_slot(slot, *, predicate=None, include_value=False):
    fields = [g.alias(g.typed('Int32', slot), 'slot'), 'id']
    if include_value:
        fields.append('k')
    return g.select(fields, 'l', where=predicate, order='id')


def _index_rows(n):
    return [{'z': (i - 1) // 3 + 1, 'a': None if i % 5 == 0 else i,
             'k': None if i % 4 == 0 else (i - 1) % 3, 'payload': 'value' + str(i)}
            for i in range(1, n + 1)]


def recipes(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('Use an integer fixture size of at least eight')
    add = _write('l', [('id', 'Int32', 2), ('k', 'Int32', 4)])
    change = _write('l', [('id', 'Int32', 1), ('k', 'Int32', n + 7)])
    points = g.op('id', 'IN', g.call('AsList', g.typed('Int32', 1), g.typed('Int32', 4)))
    absent = g.op('id', '<', g.typed('Int32', 0))
    result = {
        'c41_results_then_effect': _statements(_read_slot(0), add),
        'c41_effect_then_results': _statements(add, _read_slot(1)),
        'c41_multiple_results_effect': _statements(_read_slot(0), add, _read_slot(1)),
        'c41_write_then_read_same_table': _statements(change, _read_slot(1, include_value=True)),
        'c41_materialized_blocks': _statements(_read_slot(0, predicate=points),
            _write('l', [('id', 'Int32', 4), ('k', 'Int32', 6)]), _read_slot(1, predicate=points)),
        'c41_empty_first_result': _statements(_read_slot(0, predicate=absent), add, _read_slot(1)),
        'c41_empty_last_result': _statements(_read_slot(0), add, _read_slot(1, predicate=absent)),
        'c41_multiple_effects': _statements(add, change, _read_slot(1, include_value=True)),
        'c41_effect_only': add,
        'c41_read_only': _read_slot(0),
    }
    index_initial = g.combine([g.select([
        g.alias(g.typed('Int32', i), 'id'), g.alias(g.typed('Int32', 1), 'k'),
        g.alias(g.typed('Int32', 3), 'v')]) for i in range(1, n + 1)])
    index_changed = g.combine([g.select([
        g.alias(g.typed('Int32', i), 'id'), g.alias(g.typed('Int32', 2), 'k')])
        for i in range(1, n // 2 + 1)])
    index_effects = _statements('UPSERT INTO w ' + index_initial,
                               'UPSERT INTO w (id, k) ' + index_changed)
    result['c41_indexed_effect_sequence'] = index_effects
    result['c41_indexed_effect_blocks'] = index_effects
    source = 'ix VIEW byk'
    covered = ['z', 'a', 'k']
    complete = covered + ['payload']
    result.update({
        'c47_covering': g.select(covered, source, order='z, a'),
        'c47_noncovering': g.select(complete, source, order='z, a'),
        'c47_composite_primary_key': g.select(complete, source, where=g.op('k', '=', g.typed('Int32', 1))),
        'c47_nullable_primary_key': g.select(complete, source, where=g.op('a', 'IS', 'NULL')),
        'c47_nullable_index_key': g.select(covered, source, where=g.op('k', 'IS', 'NULL')),
        'c47_range_filter': g.select(covered, source, where=g.op(g.op('k', '>=', g.typed('Int32', 1)), 'AND', g.op('k', '<=', g.typed('Int32', 2)))),
        'c47_alias_join': g.select(['s.z AS z', 's.a AS a', 'b.payload AS payload'],
            g.join(source + ' AS s', 'ix AS b', g.op(g.op('s.z', '=', 'b.z'), 'AND', g.op('s.a', '=', 'b.a')))),
        'c47_sorted_limit': g.select(covered, source, order='k, z, a', limit=n // 2),
        'c47_main_table_control': g.select(complete, 'ix', order='z, a'),
    })
    return result


def schema_and_seeds(n, case):
    if case.startswith('c41_indexed_effect_'):
        definitions = ['id Int32 NOT NULL', 'k Int32', 'v Int32', 'PRIMARY KEY (id)',
                       'INDEX byk GLOBAL ON (k) COVER (v)']
        return 'CREATE TABLE w (' + ', '.join(definitions) + ');', []
    schema, seeds = g.schema_and_seeds(n, case)
    if not case.startswith('c47_'):
        return schema, seeds
    definitions = ['z Uint64 NOT NULL', 'a Uint64', 'k Int32', 'payload Utf8',
                   'PRIMARY KEY (z, a)', 'INDEX byk GLOBAL ON (k)']
    schema += ' CREATE TABLE ix (' + ', '.join(definitions) + ');'
    types = {'z': 'Uint64', 'a': 'Uint64', 'k': 'Int32', 'payload': 'Utf8'}
    arms = []
    for row in _index_rows(n):
        fields = []
        for name, value in row.items():
            expression = g.call('Nothing', types[name] + '?') if value is None else g.typed(types[name], value)
            # Keep nullable UNION arms type-identical so setup does not exercise
            # the separate heterogeneous-set-operation issue.
            if value is not None and name in ('a', 'k'):
                expression = g.call('Just', expression)
            fields.append(g.alias(expression, name))
        arms.append(g.select(fields))
    seeds.append('INSERT INTO ix ' + g.combine(arms))
    return schema, seeds


def metadata(case, n):
    settings = {}
    plans = []
    if case.startswith('c41_'):
        settings['EnablePerStatementQueryExecution'] = case == 'c41_indexed_effect_sequence'
        if case.startswith('c41_indexed_effect_'):
            settings['EnableIndexStreamWrite'] = True
        plans += ['Result bindings exist for every read root and no effect root',
                  'Transactions preserve original query-block order']
        if case == 'c41_materialized_blocks':
            plans.append('If range materialization is selected, its binding refers to that block precompute transaction')
    else:
        plans.append('Explicit VIEW retains an implementation-table read')
        if case in ('c47_noncovering', 'c47_composite_primary_key', 'c47_nullable_primary_key'):
            plans.append('Main-table lookup uses both physical primary-key fields in z,a order')
        if case in ('c47_covering', 'c47_nullable_index_key', 'c47_range_filter', 'c47_sorted_limit'):
            plans.append('Covering access does not need a main-table lookup')
    return {'bug': case[:3].upper(), 'deterministic': True, 'compare_legacy_hash': True,
            'repetitions': 1, 'pragmas': [], 'flag_pragmas': [],
            'table_service_overrides': settings, 'plan_assertions': plans,
            'assertions': ['Successful execution', 'Semantic oracle passes', 'New and legacy result hashes agree']}


def _expected(case, n):
    if case.startswith('c41_indexed_effect_'):
        return [{'source': tag, 'id': i, 'k': 2 if i <= n // 2 else 1, 'v': 3}
                for tag in range(2) for i in range(1, n + 1)]
    if case.startswith('c41_'):
        first = [{'slot': 0, 'id': 1}]
        after_add = [{'slot': 1, 'id': i} for i in (1, 2)]
        return {
            'c41_results_then_effect': first,
            'c41_effect_then_results': after_add,
            'c41_multiple_results_effect': first + after_add,
            'c41_write_then_read_same_table': [{'slot': 1, 'id': 1, 'k': n + 7}],
            'c41_materialized_blocks': first + [{'slot': 1, 'id': i} for i in (1, 4)],
            'c41_empty_first_result': after_add,
            'c41_empty_last_result': first,
            'c41_multiple_effects': [{'slot': 1, 'id': 1, 'k': n + 7}, {'slot': 1, 'id': 2, 'k': 4}],
            'c41_effect_only': [], 'c41_read_only': first,
        }[case]
    rows = _index_rows(n)
    fields = ['z', 'a', 'k']
    if case in ('c47_noncovering', 'c47_composite_primary_key', 'c47_nullable_primary_key', 'c47_main_table_control'):
        fields.append('payload')
    if case == 'c47_composite_primary_key':
        rows = [r for r in rows if r['k'] == 1]
    elif case == 'c47_nullable_primary_key':
        rows = [r for r in rows if r['a'] is None]
    elif case == 'c47_nullable_index_key':
        rows = [r for r in rows if r['k'] is None]
    elif case == 'c47_range_filter':
        rows = [r for r in rows if r['k'] in (1, 2)]
    elif case == 'c47_alias_join':
        rows = [r for r in rows if r['a'] is not None]
        fields = ['z', 'a', 'payload']
    elif case == 'c47_sorted_limit':
        rows = sorted(rows, key=lambda r: (-1 if r['k'] is None else r['k'], r['z'], -1 if r['a'] is None else r['a']))[:n // 2]
    return [{field: row[field] for field in fields} for row in rows]


def check_rows(case, rows, n):
    canonical = lambda values: sorted(repr(sorted(row.items())) for row in values)
    expected = _expected(case, n)
    semantic = canonical(rows) == canonical(expected)
    result = {'semantic_oracle_pass': semantic}
    if case.startswith('c41_') and case != 'c41_effect_only':
        # Distinct result-set markers detect dropped or reordered result bindings.
        result['result_binding_order_pass'] = [row.get('slot') for row in rows] == [row.get('slot') for row in expected]
        result['semantic_oracle_pass'] &= result['result_binding_order_pass']
    return result


def check_result_sets(case, result_sets, n):
    if not case.startswith('c41_'):
        return {'result_set_ownership_pass': len(result_sets) == 1}
    expected_rows = _expected(case, n)
    if case == 'c41_effect_only':
        expected_sets = []
    elif case == 'c41_empty_first_result':
        expected_sets = [[], expected_rows]
    elif case == 'c41_empty_last_result':
        expected_sets = [expected_rows, []]
    elif case in ('c41_multiple_results_effect', 'c41_materialized_blocks'):
        expected_sets = [expected_rows[:1], expected_rows[1:]]
    else:
        expected_sets = [expected_rows]
    canonical = lambda rows: sorted(repr(sorted(row.items())) for row in rows)
    valid = len(result_sets) == len(expected_sets) and all(
        canonical(actual) == canonical(expected)
        for actual, expected in zip(result_sets, expected_sets))
    return {'result_set_ownership_pass': valid, 'semantic_oracle_pass': valid}


def validation_queries(case, n):
    if case.startswith('c41_indexed_effect_'):
        arms = [g.select([g.alias(g.typed('Int32', tag), 'source'), 'id', 'k', 'v'], source)
                for tag, source in enumerate(['w', '`/Root/w/byk/indexImplTable`'])]
        return [g.combine(arms)]
    return [g.select(['id', 'k'], 'l', order='id')] if case == 'c41_effect_only' else []


def check_validation_results(case, result_sets, n):
    if case.startswith('c41_indexed_effect_'):
        if len(result_sets) != 1:
            return {'semantic_oracle_pass': False, 'result_set_ownership_pass': False}
        canonical = lambda rows: sorted(repr(sorted(row.items())) for row in rows)
        actual = result_sets[0]
        expected = _expected(case, n)
        base = canonical([r for r in actual if r.get('source') == 0]) == canonical(
            [r for r in expected if r['source'] == 0])
        index = canonical([r for r in actual if r.get('source') == 1]) == canonical(
            [r for r in expected if r['source'] == 1])
        return {'semantic_oracle_pass': base and index,
                'base_table_oracle_pass': base, 'index_table_oracle_pass': index,
                'result_set_ownership_pass': True, 'effect_persistence_pass': base and index,
                'index_extra_row_count': max(0, sum(r.get('source') == 1 for r in actual) - n)}
    if case != 'c41_effect_only':
        raise ValueError('No validation request defined for this case')
    expected = [[{'id': 1, 'k': 3}, {'id': 2, 'k': 4}]]
    valid = result_sets == expected
    return {'semantic_oracle_pass': valid, 'effect_persistence_pass': valid}
