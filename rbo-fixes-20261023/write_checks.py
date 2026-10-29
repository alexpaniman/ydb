"""Compose write checks in memory; never persist or print rendered statements.

The harness owns anonymous query/result buffers. Oracles expose booleans only.
Routing is deliberate legacy optimization of an existing AST, including when
error fallback is disabled; these cases do not claim native RBO write support.
"""
import json

import generate_and_verify as g


CASE_SPECIFIC_SCHEMA = True


def _integer(value):
    return g.typed('Int64', value)


def _rows(values, columns=('id', 'k', 'v')):
    return g.combine([g.select([g.alias(_integer(value), column)
                               for column, value in zip(columns, row)])
                      for row in values])


def _write(mode, values, columns=('id', 'k', 'v')):
    return mode + ' INTO w (' + ', '.join(columns) + ') ' + _rows(values, columns)


def _update(predicate, value):
    return 'UPDATE w SET k = ' + _integer(value) + ' WHERE ' + predicate


def _read(indexed=False):
    def arm(source, tag):
        return g.select([g.alias(_integer(tag), 'source'), 'id', 'k', 'v'], source)
    arms = [arm('w', 0)]
    if indexed:
        arms.append(arm('`/Root/w/byk/indexImplTable`', 1))
    return g.combine(arms)


def _program(*operations):
    return '; '.join(operations)


def recipes(n):
    if n < 2:
        raise ValueError('write checks require at least two rows')
    fresh = n + 1
    initial = [(i, 1, 3) for i in range(1, n + 1)]
    result = {}
    scenarios = {
        'existing_conflict': [(1, 2, 4)],
        'mixed_conflict': [(1, 2, 4), (fresh, 2, 4)],
        'duplicate_input': [(fresh, 2, 4), (fresh, 2, 4)],
        'no_conflict': [(fresh, 2, 4)],
    }
    for label, values in scenarios.items():
        result['c30_' + label] = _write('INSERT OR REVERT', values)
    result['c30_insert_control'] = _write('INSERT', [(fresh, 2, 4)])
    scope = _program(_write('INSERT', [(fresh, 2, 4)]),
                     _write('INSERT OR REVERT', [(1, 2, 4)]),
                     _write('INSERT', [(fresh + 1, 2, 4)]))
    result['c30_statement_scope'] = scope
    result['c30_single_unit_scope'] = scope

    for label in ('sink_disabled_guard', 'sink_disabled_fallback', 'sink_enabled_control'):
        result['c44_' + label] = _write('INSERT', initial)

    for indexed in (False, True):
        prefix = 'c46_' + ('indexed_' if indexed else 'plain_')
        for label, predicate in [('partial', g.op('id', '<=', _integer(n // 2))),
                                 ('no_matches', g.op('id', '<', _integer(0))),
                                 ('all_matches', g.op('id', '>', _integer(0)))]:
            result[prefix + label] = _update(predicate, 2)
    result['c46_statement_scope'] = _program(
        _write('INSERT', [(fresh, 1, 3)]), _update(g.op('id', '=', _integer(1)), 2),
        _write('INSERT', [(fresh + 1, 1, 3)]))

    for label, mode, indexed in [('indexed_partial_upsert', 'UPSERT', True),
                                  ('indexed_partial_replace', 'REPLACE', True),
                                  ('unindexed_control', 'UPSERT', False),
                                  ('stream_index_control', 'UPSERT', True)]:
        changed = [(i, 2) for i in range(1, n // 2 + 1)]
        update = _write(mode, changed, ('id', 'k'))
        result['c48_' + label] = update if label.startswith('stream_') else _program(_write('UPSERT', initial), update)
    result['c48_indexed_insert'] = _write('UPSERT', initial)
    result['c48_stream_full_upsert_control'] = _write('UPSERT', [(i, 2, 3) for i in range(1, n // 2 + 1)])
    result['c48_stream_partial_replace_control'] = _write('REPLACE', [(i, 2) for i in range(1, n // 2 + 1)], ('id', 'k'))
    result['c48_stream_nonkey_upsert_control'] = _write('UPSERT', [(i, 4) for i in range(1, n // 2 + 1)], ('id', 'v'))
    result['c48_stream_fresh_control'] = _write('UPSERT', initial)
    return result


def _has_index(case):
    return (case.startswith('c44_') or case.startswith('c46_indexed_') or
            (case.startswith('c48_') and case != 'c48_unindexed_control'))


def schema_and_seeds(n, case):
    definitions = ['id Int64 NOT NULL', 'k Int64', 'v Int64', 'PRIMARY KEY (id)']
    if _has_index(case):
        definitions.append('INDEX byk GLOBAL ON (k) COVER (v)')
    schema = 'CREATE TABLE w (' + ', '.join(definitions) + ');'
    # C48 begins with an empty table so baseline failure is the target UPSERT,
    # not setup through the separately defective indexed INSERT path.
    seeds = [] if (case.startswith('c44_') or
                   (case.startswith('c48_') and not case.startswith('c48_stream_')) or
                   case == 'c48_stream_fresh_control') else [
        _write('UPSERT', [(i, 1, 3) for i in range(1, n + 1)])]
    return schema, seeds


def validation_queries(case, n):
    # A separate request avoids the independent mixed-effect/result bug C41.
    # The runner exposes only this final request's result; every preceding
    # request must complete successfully before it can be observed.
    return [_read(_has_index(case))]


def metadata(case, n):
    settings = {
        'EnableFallbackToYqlOptimizer': False,
        'EnableIndexStreamWrite': not case.startswith('c48_') or case.startswith('c48_stream_'),
        'EnablePerStatementQueryExecution': case != 'c30_single_unit_scope',
    }
    result = {'table_service_overrides': settings, 'node_count': 1,
              'generator_version': 'separate_write_validation_v3',
              'pragmas': [('kikimr.CostBasedOptimizationLevel', '0')],
              'routing_expectation': 'Affected writes select the legacy optimizer before execution; no error fallback required.'}
    if case.startswith('c44_'):
        settings['EnableIndexStreamWrite'] = case == 'c44_sink_enabled_control'
        settings['EnableFallbackToYqlOptimizer'] = case == 'c44_sink_disabled_fallback'
        result['routing_expectation'] = 'C44 is a pre-execution guard, with legacy execution only when error fallback is enabled.'
        if case == 'c44_sink_disabled_guard':
            result['expected_rejection_token'] = 'New RBO table effects require sink index maintenance or legacy fallback'
    return result


def check_rows(case, rows, n):
    expected = [{'id': i, 'k': 1, 'v': 3} for i in range(1, n + 1)]
    if case.startswith('c30_'):
        if case in ('c30_no_conflict', 'c30_insert_control'):
            expected.append({'id': n + 1, 'k': 2, 'v': 4})
        elif case in ('c30_statement_scope', 'c30_single_unit_scope'):
            expected.extend({'id': i, 'k': 2, 'v': 4} for i in (n + 1, n + 2))
    elif case.startswith('c46_'):
        for row in expected:
            if (case.endswith('all_matches') or
                (case.endswith('partial') and row['id'] <= n // 2) or
                (case == 'c46_statement_scope' and row['id'] == 1)):
                row['k'] = 2
        if case == 'c46_statement_scope':
            expected.extend({'id': i, 'k': 1, 'v': 3} for i in (n + 1, n + 2))
    elif case.startswith('c48_'):
        if case not in ('c48_indexed_insert', 'c48_stream_fresh_control'):
            for row in expected:
                if row['id'] <= n // 2:
                    if case == 'c48_stream_nonkey_upsert_control':
                        row['v'] = 4
                    else:
                        row['k'] = 2
                    if case in ('c48_indexed_partial_replace', 'c48_stream_partial_replace_control'):
                        row['v'] = None
    elif case.startswith('c44_'):
        pass
    else:
        raise ValueError(case)
    tagged = [dict(row, source=tag) for tag in range(2 if _has_index(case) else 1)
              for row in expected]
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True) for row in values)
    verdict = {'semantic_oracle_pass': canonical(rows) == canonical(tagged),
               'base_table_oracle_pass': canonical([row for row in rows if row.get('source') == 0]) ==
                                         canonical([row for row in tagged if row['source'] == 0]),
               'index_consistency_checked': _has_index(case),
               'statement_scope_checked': 'scope' in case}
    if _has_index(case):
        index_rows = [row for row in rows if row.get('source') == 1]
        expected_index = [row for row in tagged if row['source'] == 1]
        verdict['index_table_oracle_pass'] = canonical(index_rows) == canonical(expected_index)
        verdict['index_extra_row_count'] = max(0, len(index_rows) - len(expected_index))
    return verdict


def check_validation_results(case, result_sets, n):
    # Validation is a single read request with one tagged result set. Its exact
    # set ownership must hold in addition to matching the expected table rows.
    if len(result_sets) != 1:
        return {'semantic_oracle_pass': False, 'result_set_ownership_pass': False}
    verdict = check_rows(case, result_sets[0], n)
    verdict['result_set_ownership_pass'] = True
    return verdict
