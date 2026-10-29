"""Q15 subset-amendment builders; never persist rendered inputs or plans."""
import json

import generate_and_verify as g
import quality_partition_checks as direct
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q15_subset_shared_metadata_stage_v1'


def _chain(kind):
    first = g.select(['id', 'p', 'o',
        g.alias(g.over(g.call('ROW_NUMBER'), 'p', 'id ASC'), 'a')], g.derived(windows._source(), 's'))
    first = g.select(['id', 'p', 'o', 'a'], g.derived(first, 'f'),
                     where=g.op('a', '>', g.typed('Uint64', 1)))
    if kind == 'extra_computed':
        first = g.select(['id', 'p', 'o', 'a',
            g.alias(g.op('a', '+', g.typed('Uint64', 3)), 'x')], g.derived(first, 'm'))
    second_keys = 'p, x' if kind == 'extra_computed' else 'p, o'
    second = g.select(['id', 'p', 'o', 'a',
        g.alias(g.over(g.call('ROW_NUMBER'), second_keys, 'id DESC'), 'b')], g.derived(first, 'w'))
    if kind == 'chain_three':
        return g.select(['id', 'a', 'b',
            g.alias(g.over(g.call('ROW_NUMBER'), 'p', 'id ASC'), 'c')], g.derived(second, 'z'), order='id')
    if kind == 'after_aggregate':
        return g.select(['p', g.alias(g.call('SUM', 'a'), 'a'), g.alias(g.call('SUM', 'b'), 'b')],
                        g.derived(second, 'z'), group='p', order='p')
    return g.select(['id', 'a', 'b'], g.derived(second, 'z'), order='id')


def recipes(n):
    result = direct.recipes(n)
    for kind in ('strict_superset', 'extra_computed', 'chain_three', 'after_aggregate'):
        result['q15_' + kind] = _chain(kind)
    for variant in ('subset', 'missing_key_control', 'disabled_source_control'):
        keys = 'p, o' if variant != 'missing_key_control' else 'o'
        result['q15_source_' + variant] = g.select(['id',
            g.alias(g.over(g.call('ROW_NUMBER'), keys, 'id'), 'r')], 'qs', order='id')
    return result


def schema_and_seeds(n, case):
    if not case.startswith('q15_source_'):
        return direct.schema_and_seeds(n, case)
    fields = ['id Int64 NOT NULL', 'p Int64 NOT NULL', 'o Uint64', 'PRIMARY KEY (p, id)']
    schema = 'CREATE TABLE qs (' + ', '.join(fields) + ') PARTITION BY HASH(p) WITH (STORE = COLUMN, PARTITION_COUNT = 4);'
    rows = [g.select([g.alias(g.typed('Int64', i), 'id'), g.alias(g.typed('Int64', i % 3), 'p'),
                      g.alias(g.call('Nothing', 'Uint64?') if i % 5 == 0 else
                              g.call('Just', g.typed('Uint64', i % 5)), 'o')]) for i in range(1, n + 1)]
    return schema, ['UPSERT INTO qs ' + g.combine(rows)]


def metadata(case, n):
    result = direct.metadata(case, n)
    result.update(generator_version=GENERATOR_VERSION,
                  acceptance_scope='Q15 subset contract with shared metadata and stage proof',
                  additional_ir_admission_required=True)
    if case.startswith('q15_source_'):
        result['table_service_overrides'].update(EnableOlapSink=True, AllowOlapDataQuery=True)
    if case == 'q15_source_disabled_source_control':
        result['table_service_overrides']['DefaultEnableShuffleElimination'] = False
    if case == 'q15_after_aggregate':
        result['table_service_overrides'].update(EnableQueryServiceSpilling=True,
            EnableSpillingInHashJoinShuffleConnections=True, EnableSpillingNodes='All', DqHashOperatorsUseBlocks=False)
    return result


def expected_rows(case, n):
    if case in direct.recipes(n):
        return direct.expected_rows(case, n)
    source = [{'id': i, 'p': i % 3, 'o': None if case.startswith('q15_source_') and i % 5 == 0 else i % 5}
              for i in range(1, n + 1)]
    if case.startswith('q15_source_'):
        key = (lambda row: row['o']) if case == 'q15_source_missing_key_control' else (lambda row: (row['p'], row['o']))
        return [{'id': row['id'], 'r': sum(other['id'] <= row['id'] and key(other) == key(row) for other in source)}
                for row in source]
    for row in source:
        row['a'] = sum(other['id'] <= row['id'] and other['p'] == row['p'] for other in source)
    kept = [row for row in source if row['a'] > 1]
    for row in kept:
        key = lambda item: (item['p'], item['a'] + 3) if case == 'q15_extra_computed' else (item['p'], item['o'])
        row['b'] = sum(other['id'] >= row['id'] and key(other) == key(row) for other in kept)
        row['c'] = sum(other['id'] <= row['id'] and other['p'] == row['p'] for other in kept)
    if case == 'q15_after_aggregate':
        return [{'p': p, 'a': sum(row['a'] for row in kept if row['p'] == p),
                 'b': sum(row['b'] for row in kept if row['p'] == p)} for p in sorted({row['p'] for row in kept})]
    columns = ('id', 'a', 'b', 'c') if case == 'q15_chain_three' else ('id', 'a', 'b')
    return [{key: row[key] for key in columns} for row in kept]


def check_rows(case, rows, n):
    canonical = lambda data: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in data)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_partition_rank_oracle': True, 'quality_improvement_verified': False}


def check_physical_ast(case, text, baseline=None):
    if case in direct.recipes(61) and case != 'q15_ordered_keys_control':
        result = direct.check_physical_ast(case, text, baseline)
    else:
        # Reuse validated baseline handling, then apply this profile's shape.
        result = windows.check_physical_ast('q01_empty_control', text, baseline)
        expected = 0 if case == 'q15_source_subset' else 1
        assertions = {'expected_shuffle_count': result['callables']['DqCnHashShuffle'] == expected}
        positive = case not in ('q15_source_missing_key_control', 'q15_source_disabled_source_control')
        if positive and baseline is not None:
            assertions['baseline_redundant_shuffle_present'] = result['baseline_callables']['DqCnHashShuffle'] > expected
        result.update(structural_assertions=assertions, structural_oracle_pass=all(assertions.values()))
    result['additional_ir_admission_required'] = True
    result['metadata_connection_agreement_verified'] = False
    result['quality_improvement_verified'] = False
    return result
