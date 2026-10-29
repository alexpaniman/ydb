"""Generate optimizer checks in memory; persist outcomes, never generated SQL.

The relational builders deliberately retain only composable test recipes.
Queries, input data and result payloads live in anonymous memory files. The
runner's diagnostic stream is captured and reduced to failure categories.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import subprocess

ROOT = Path('/home/alexpaniman/projects/ydb/ai-verification')
HERE = Path(__file__).resolve().parent
BINARY = ROOT / 'ydb/tests/tools/kqprun/kqprun'
MATH = ROOT / 'yql/essentials/udfs/common/math/libmath_udf.so'


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def call(name, *args):
    return name + '(' + ', '.join(map(str, args)) + ')'


def typed(name, value):
    return call(name, quote(value))


def op(left, symbol, right):
    return '(' + str(left) + ' ' + symbol + ' ' + str(right) + ')'


def alias(expr, name):
    return str(expr) + ' AS ' + name


def select(fields, source=None, *, where=None, group=None, having=None,
           order=None, limit=None, distinct=False):
    parts = ['SELECT', 'DISTINCT' if distinct else '', ', '.join(fields)]
    for key, value in [('FROM', source), ('WHERE', where), ('GROUP BY', group),
                       ('HAVING', having), ('ORDER BY', order), ('LIMIT', limit)]:
        if value is not None:
            parts += [key, str(value)]
    return ' '.join(x for x in parts if x)


def derived(query, name):
    return '(' + query + ') AS ' + name


def combine(queries, operation='UNION ALL'):
    return (' ' + operation + ' ').join(queries)


def join(left, right, predicate, kind='INNER'):
    return left + ' ' + kind + ' JOIN ' + right + ' ON ' + predicate


def over(expr, partition=None, order=None, frame=None):
    parts = []
    if partition:
        parts += ['PARTITION BY', partition]
    if order:
        parts += ['ORDER BY', order]
    if frame:
        parts += [frame]
    return expr + ' OVER (' + ' '.join(parts) + ')'


def recipes(n):
    count = call('COUNT', '*')
    integer = lambda value: typed('Int64', value)
    grouped = select(['k', alias(count, 'n')], 't', group='k')
    pairs = combine([select([alias(integer(g), 'g'), alias(integer(7), 'x')])
                     for g in (1, 2)])
    structs = select([alias(call('AsStruct', alias(integer(1), 'x')), 's'),
                      alias(call('AsStruct', alias(integer(2), 'x')), 'q')], 'l')
    null_column = select([alias(call('Nothing', 'Int32?'), 'x')])
    inside = op('k', 'IN', '(' + null_column + ')')
    left = derived(select([alias(typed('Int32', 1), 'k')]), 'a')
    right = derived(select([alias(typed('Uint32', 1), 'k')]), 'b')
    result = {
        'typed_predicate': select(['a.k', 'b.k'], join(left, right, op('a.k', '=', 'b.k')),
                                  where=op('-a.k', '<', typed('Int32', 0))),
        'volatile_group': select(['k', 'n'], derived(grouped, 's'),
                                 where=op(call('Random', 'k'), '<', '0.5')),
        'distinct_rollup': select(['g', alias(call('COUNT', 'DISTINCT x'), 'c')],
                                  derived(pairs, 's'), group=call('ROLLUP', 'g'), order='g'),
        'distinct_group_output': select([alias(count, 'c')], 't', group='id', distinct=True),
        'group_expression': select([alias(call('COALESCE', op('k', '+', call('CAST', count + ' AS Int64')), 'k'), 'n')],
                                    't', group='k'),
        'nested_members': select([alias(call('SUM', 'z.s.x'), 'a'), alias(call('SUM', 'z.q.x'), 'b')],
                                  derived(structs, 'z')),
        'in_fallback': select(['id'], 'l', where=call('COALESCE', inside, 'TRUE')),
        'in_legacy_mode': select(['id'], 'l', where=op('id', 'NOT IN', '(' + null_column + ')')),
        'unordered_rank': select(['id', alias(over(call('RANK')), 'r')], 't'),
        'window_overlap': select(['k', alias(over(count, 'k', 'k'), 'c')], 't', limit=2),
        'window_distinct': select([alias(over(count), 'n')], 't', distinct=True),
        'plain_having': select(['k', alias(count, 'n')], 't', group='k', having=op('k', '>', '0')),
        'distinct_variance': select([alias(call('COUNT', 'DISTINCT k'), 'c'),
                                     alias(call('STDDEV_SAMP', 'k'), 's')], 't'),
        'interval_average': select([alias(call('AVG', typed('Interval', 'PT1S')), 'a')], 't'),
        'tuple_some': select([alias(call('SOME', call('AsTuple', 'id', 'k', op('id', '+', '1'))), 'v')], 't'),
        'repeated_order': select(['id', 'k'], 't', order='id, id', limit=3),
        'composite_order': select(['id', 'k'], 't', order=call('AsTuple', 'k', 'id'), limit=3),
        'nested_order': select(['z.s'], derived(structs, 'z'), order='z.s.x', limit=1),
        'full_map': select(['l.id', 'r.id'], join('l', 'r', op('l.id', '=', 'r.id'), 'FULL')),
    }
    left_rows = select(['id', 'k', alias(call('RandomNumber', 'id'), 'v')], 't', where=op('id', '=', '1'))
    result['cbo_volatile_map'] = select([alias(call('COUNT', 'DISTINCT a.v'), 'n')],
        join(derived(left_rows, 'a'), 't AS b', op('a.k', '=', 'b.k')))
    ranked = select([alias('a.k', 'k'), alias(over(call('ROW_NUMBER'), 'a.id', 'b.id'), 'r')],
        join('t AS a', 't AS b', op('a.k', '=', 'b.k')))
    result['window_distribution'] = select(['k', alias(call('SUM', 'r'), 'n')], derived(ranked, 's'), group='k')
    single = select(['id'], 't', where=op('id', '=', '1'))
    result['full_distribution'] = select(['a.id', alias(count, 'n')],
        join(derived(single, 'a'), 't AS b', op('a.id', '=', 'b.id'), 'FULL'), group='a.id')
    result['full_residual'] = select(['l.id', 'r.id'], join('l', 'r', op(op('l.id', '=', 'r.id'), 'AND', op('l.k', '<', 'r.k')), 'FULL'))
    result['fulltext_range'] = select(['k'], 'ft', where=op('k', '=', '3'))
    result['fulltext_lookup'] = select(['l.id', 'ft.k'], join('l', 'ft', op('l.k', '=', 'ft.k')))
    json_extract = lambda path: call('JSON_VALUE', 'doc', quote(path))
    result['olap_multiple_json'] = select([alias(json_extract('$.a'), 'a'), alias(json_extract('$.b'), 'b')], 'col')
    result['olap_json_reuse'] = select([alias(call('AsTuple', json_extract('$.a'), call('CAST', 'doc AS String')), 'v')], 'col')
    basic = op('k', '=', '1')
    substring = call('String::Contains', 'txt', quote('a'))
    result['olap_boolean'] = select(['id'], 'col', where=op(op(basic, 'AND', substring), 'OR', op('k', '=', '2')))
    result['mixed_effect_results'] = '; '.join([select(['id'], 'l'), 'UPSERT INTO l ' + select([alias(typed('Int32', 2), 'id'), alias(typed('Int32', 4), 'k')]), select(['id'], 'l', order='id')])
    union_parts = [select([alias(typed('Int32', x), 'v')]) for x in (1, 2)] + [select([alias(call('Nothing', 'Int32?'), 'v')])]
    result['nested_union_nullable'] = select(['v'], derived(combine(union_parts), 's'), where='v IS NULL')
    result['index_write_maintenance'] = 'INSERT INTO indexed ' + select([alias(typed('Uint64', 2), 'id'), alias(typed('Int32', 5), 'k')])
    result['focused_composite_compare'] = select(['id'], 't', where=op(call('AsTuple', call('AsTuple', 'id', 'k'), 'id'), '=', call('AsTuple', call('AsTuple', 'k', 'id'), 'k')))
    result['fulltext_postings'] = select([alias(count, 'n')], '`/Root/ft/search_idx/indexImplTable`')
    result['fulltext_base_control'] = select(['id', 'k'], 'ft', order='id')
    result['fulltext_lookup_compact'] = result['fulltext_lookup']
    result['conditional_index_update'] = 'UPDATE indexed SET k = ' + typed('Int32', 5) + ' WHERE ' + op('id', '=', typed('Uint64', 1))
    result['fulltext_premaintained_lookup'] = result['fulltext_lookup']
    nan_key = call('IF', op('id', '>', '0'), typed('Double', 'nan'), typed('Double', 0))
    result['window_nan_partition'] = select(['id', alias(over(count, nan_key), 'n')], 't')
    guarded = derived(select(['id'], 't', where=op(op('id', '%', '2'), '=', '0')), 'a')
    safe_key = call('Ensure', 'a.id', op('a.id', '>', '1'), quote('Invalid value'))
    result['computed_key_guard'] = select(['a.id'], join(guarded, 't AS b', op(safe_key, '=', 'b.id')))
    window_structs = select(['id', alias(call('AsStruct', alias(op('id', '+', '1'), 'x')), 's'), alias(op('id', '+', '10'), 'x')], 't')
    result['nested_window_member'] = select(['id', alias(over(call('SUM', 'z.s.x')), 'n')], derived(window_structs, 'z'), order='id')
    result['olap_json_reuse_scalar'] = select([alias(op(call('LENGTH', call('CAST', 'doc AS String')), '+', call('LENGTH', json_extract('$.a'))), 'v')], 'col')
    result['conditional_update'] = 'UPDATE t SET k = ' + typed('Int64', 5) + ' WHERE ' + op('id', '=', typed('Int64', 1))
    result['direct_index_read'] = select(['id', 'k'], 'indexed VIEW byk', order='id')
    result['index_base_control'] = result['index_write_maintenance']
    result['indexed_upsert_no_stream'] = 'UPSERT INTO indexed ' + select([alias(typed('Uint64', 2), 'id'), alias(typed('Int32', 5), 'k')])
    result['mixed_numeric_shuffle'] = select(['l.id', 'r.id'], join('l', 'r', op('l.k', '=', 'r.k')))
    cast_left = derived(select(['id', alias(call('CAST', 'k AS Double'), 'k')], 'l'), 'a')
    result['mixed_numeric_normalized'] = select(['a.id', 'r.id'], join(cast_left, 'r', op('a.k', '=', 'r.k')))
    return result


def schema_and_seeds(n, case):
    tables = {
        'l': [('id', 'Int32 NOT NULL'), ('k', 'Int32')],
        'r': [('id', 'Int32 NOT NULL'), ('k', 'Double')],
        't': [('id', 'Int64 NOT NULL'), ('k', 'Int64 NOT NULL')],
    }
    ddl = []
    for table, columns in tables.items():
        definitions = [name + ' ' + typ for name, typ in columns] + ['PRIMARY KEY (id)']
        ddl.append('CREATE TABLE ' + table + ' (' + ', '.join(definitions) + ')')
    seeds = []
    for table, rows in [('l', [(1, 3)]), ('r', [(1, 3)]), ('t', [(i, 1) for i in range(1, n + 1)])]:
        keytype = 'Int64' if table == 't' else 'Int32'
        valuetype = 'Double' if table == 'r' else keytype
        arms = [select([alias(typed(keytype, i), 'id'), alias(typed(valuetype, k), 'k')]) for i, k in rows]
        seeds.append('UPSERT INTO ' + table + ' ' + combine(arms))
    if case.startswith('fulltext_'):
        definitions = ['id Uint64 NOT NULL', 'k Int32', 'txt String', 'PRIMARY KEY (id)']
        definitions += ['INDEX search_idx GLOBAL USING fulltext_plain ON (k, txt) WITH (tokenizer=standard, use_filter_lowercase=true)']
        ddl.append('CREATE TABLE ft (' + ', '.join(definitions) + ')')
        arms = [select([alias(typed('Uint64', i), 'id'), alias(typed('Int32', 3), 'k'), alias(quote(words), 'txt')]) for i, words in enumerate(['alpha beta', 'gamma', ''], 1)]
        seeds.append('INSERT INTO ft ' + combine(arms))
        if case == 'fulltext_premaintained_lookup':
            postings = []
            for i, words in enumerate(['alpha beta', 'gamma', ''], 1):
                for word in words.split():
                    postings.append(select([alias(typed('Uint64', i), 'id'), alias(typed('Int32', 3), 'k'), alias(quote(word), '__ydb_token')]))
            seeds.append('UPSERT INTO `/Root/ft/search_idx/indexImplTable` ' + combine(postings))
    if case in ('index_write_maintenance', 'conditional_index_update', 'direct_index_read', 'index_base_control', 'indexed_upsert_no_stream'):
        definitions = ['id Uint64 NOT NULL', 'k Int32', 'PRIMARY KEY (id)', 'INDEX byk GLOBAL ON (k)']
        ddl.append('CREATE TABLE indexed (' + ', '.join(definitions) + ')')
        seeds.append('INSERT INTO indexed ' + select([alias(typed('Uint64', 1), 'id'), alias(typed('Int32', 3), 'k')]))
    if case.startswith('olap_'):
        definitions = ['id Uint64 NOT NULL', 'k Int64 NOT NULL', 'txt Utf8 NOT NULL', 'doc Json', 'PRIMARY KEY (id)']
        ddl.append('CREATE TABLE col (' + ', '.join(definitions) + ') PARTITION BY HASH(id) WITH (STORE=COLUMN)')
        arms = [select([alias(typed('Uint64', i), 'id'), alias(typed('Int64', k), 'k'), alias(typed('Utf8', word), 'txt'), alias(call('CAST', quote(json.dumps({'a':str(i),'b':str(k)})) + ' AS Json'), 'doc')]) for i,(k,word) in enumerate([(1,'alpha'),(1,'b'),(2,'b'),(3,'alpha')],1)]
        seeds.append('UPSERT INTO col ' + combine(arms))
    return '; '.join(ddl) + ';', seeds


def anonymous_text(label, contents):
    fd = os.memfd_create(label)
    os.write(fd, contents.encode())
    os.lseek(fd, 0, os.SEEK_SET)
    return fd


def failure_category(text):
    signatures = [
        ('multiple JSON projection alias', 'Multiple projections for same column'),
        ('missing UDF library', 'Failed to find UDF module'),
        ('aggregate tuple state', 'tupleType->GetSize'),
        ('duplicate physical sort key', 'Duplicate index'),
        ('unsupported merge key', 'Unsupported type'),
        ('unavailable column binding', 'Member not found'),
        ('physical type mismatch', 'mismatch'),
        ('type mismatch', 'type should be'),
        ('invalid expression child', 'child should not be nullptr'),
        ('invalid aggregate phase', 'GetAggregationPhase'),
        ('unsupported distinct variance', 'Variance not supported'),
        ('unsupported distinct variance', 'Variacnce not supported'),
        ('HAVING translation', 'Invalid number of filters'),
        ('shared plan without spilling', 'channel spilling disabled'),
        ('scalar cardinality check', 'Scalar subquery returned more than one row'),
    ]
    for label, token in signatures:
        if token.lower() in text.lower():
            return label
    return 'query or compilation error'


def run_case(case, mode, n):
    query = recipes(n)[case]
    pragmas = [('YqlSelect', 'disable' if mode == 'legacy' else 'force')]
    if case == 'in_fallback':
        query = 'PRAGMA AnsiInForEmptyOrNullableItemsCollections; ' + query
    elif case == 'in_legacy_mode':
        query = 'PRAGMA DisableAnsiInForEmptyOrNullableItemsCollections; ' + query
    elif case == 'full_map':
        pragmas += [('kikimr.CostBasedOptimizationLevel', '0'), ('kikimr.HashJoinMode', 'map')]
    elif case == 'focused_composite_compare':
        pragmas += [('ydb.EnableNewRBOPhysicalStagePeephole', 'false')]
    elif case.startswith('mixed_numeric_'):
        pragmas += [('kikimr.CostBasedOptimizationLevel', '0'), ('kikimr.HashJoinMode', 'grace')]
    elif case == 'window_nan_partition':
        pragmas += [('ydb.WindowFunctionsV2', 'true')]
    elif case in ('fulltext_lookup', 'fulltext_lookup_compact', 'fulltext_premaintained_lookup'):
        pragmas += [('ydb.EnableAutoIndexSelectionForIndexLookupJoin', 'true')]
    elif case.endswith('_distribution') or case == 'full_residual':
        pragmas += [('kikimr.HashJoinMode', 'grace')]
    query = ''.join('PRAGMA ' + name + '=' + quote(value) + '; ' for name, value in pragmas) + query + ';'
    schema, seeds = schema_and_seeds(n, case)
    descriptors = []
    def memory(label, value):
        fd = anonymous_text(label, value)
        descriptors.append(fd)
        return '/proc/self/fd/' + str(fd)
    config = str(HERE / (mode + '.conf'))
    if case == 'full_residual' and mode == 'new':
        config = str(HERE / 'block.conf')
    if case.endswith('_distribution'):
        config = memory('generated-config', Path(config).read_text().replace(
            'DefaultEnableShuffleElimination: false', 'DefaultEnableShuffleElimination: true'))
    if case.startswith('fulltext_') and case != 'fulltext_lookup_compact':
        config = memory('generated-specialized-config', Path(config).read_text().replace('FeatureFlags {', 'FeatureFlags {\n EnableCompactFulltextIndex: false'))
    if case == 'mixed_effect_results':
        config = memory('generated-statements-config', Path(config).read_text().replace('EnablePerStatementQueryExecution: true', 'EnablePerStatementQueryExecution: false'))
    if case in ('index_write_maintenance', 'index_base_control', 'indexed_upsert_no_stream'):
        config = memory('generated-write-config', Path(config).read_text().replace('TableServiceConfig {', 'TableServiceConfig {\n EnableIndexStreamWrite: false'))
    command = [str(BINARY), '-c', config, '-u', str(MATH),
               '-s', memory('generated-schema', schema)]
    if case.endswith('_distribution') or case.startswith('mixed_numeric_'):
        command += ['-N', '4']
    for i, seed in enumerate(seeds):
        command += ['-p', memory('generated-data-' + str(i), seed + ';')]
    statement_count = len(seeds)
    command += ['-p', memory('generated-check', query)]
    if case in ('index_write_maintenance', 'conditional_index_update', 'index_base_control'):
        validation_source = 'indexed' if case == 'index_base_control' else '`/Root/indexed/byk/indexImplTable`'
        validation = 'PRAGMA YqlSelect=' + quote('force' if mode == 'new' else 'disable') + '; ' + select(['id', 'k'], validation_source, order='id') + ';'
        command += ['-p', memory('generated-validation', validation)]
    result_path = memory('result-buffer', '')
    result_fd = descriptors[-1]
    command += ['-C', 'query', '--log-file', '/dev/null', '--result-file', result_path]
    try:
        process = subprocess.run(command, pass_fds=descriptors, capture_output=True, timeout=75,
                                 preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        os.lseek(result_fd, 0, os.SEEK_SET)
        payload = os.read(result_fd, 8 * 1024 * 1024)
        record = {'case': case, 'mode': mode, 'exit_code': process.returncode,
                  'row_count': len(payload.splitlines()), 'result_sha256': hashlib.sha256(payload).hexdigest()}
        if process.returncode == 0:
            try:
                canonical = sorted(json.dumps(json.loads(line), sort_keys=True, separators=(',', ':')) for line in payload.splitlines())
                record['unordered_result_sha256'] = hashlib.sha256(('\n'.join(canonical)).encode()).hexdigest()
            except (ValueError, TypeError):
                record['result_not_jsonl'] = True
        if process.returncode:
            diagnostics = (process.stdout + process.stderr).decode(errors='replace')
            query_ids = [int(value) for value in re.findall(r'Executing query ([0-9]+)', diagnostics)]
            if query_ids:
                last_query = query_ids[-1]
                record['failure_stage'] = 'setup' if last_query < statement_count else ('target' if last_query == statement_count else 'validation')
            record['failure_category'] = failure_category(diagnostics)
            record['diagnostic_source_locations'] = sorted(set(re.findall(r'(?:ydb|yql|util)/[a-zA-Z0-9_/.-]+\.(?:cpp|h):[0-9]+', diagnostics)))[:16]
            categories = ['key not found', 'unknown table', 'unknown column', 'type annotation', 'result count', 'effects', 'fulltext', 'unexpected', 'assertion failed', 'does not exist', 'invalid result', 'Multiple statements', 'Not supported', 'Cannot load', 'Json2', 'duplicate', 'WithEffects', 'KqpPhysicalQuery']
            record['unsupported_internal_callables'] = sorted(set(re.findall(r'Unsupported callable: ([A-Za-z0-9_]+)', diagnostics)))
            record['diagnostic_categories'] = [token for token in categories if token.lower() in diagnostics.lower()]
        if case.startswith('mixed_numeric_') and process.returncode == 0:
            record['semantic_oracle_pass'] = record['row_count'] == 1
        if case in ('index_write_maintenance', 'index_base_control') and process.returncode == 0:
            record['semantic_oracle_pass'] = record['row_count'] == 2
        if case in ('fulltext_postings', 'window_nan_partition', 'full_residual') and process.returncode == 0:
            parsed = [json.loads(line) for line in payload.splitlines()]
            if case == 'fulltext_postings':
                record['semantic_oracle_pass'] = len(parsed) == 1 and parsed[0]['n'] == 3
                record['unexpected_empty_index'] = len(parsed) == 1 and parsed[0]['n'] == 0
            elif case == 'window_nan_partition':
                record['semantic_oracle_pass'] = len(parsed) == n and all(row['n'] == n for row in parsed)
            else:
                record['semantic_oracle_pass'] = len(parsed) == 2
        if case == 'volatile_group' and process.returncode == 0:
            values = [json.loads(line) for line in payload.splitlines()]
            record['invalid_aggregate_observed'] = any(0 < row['n'] < n for row in values)
        if case == 'cbo_volatile_map' and process.returncode == 0:
            values = [json.loads(line) for line in payload.splitlines()]
            record['shared_value_evaluated_more_than_once'] = any(row['n'] > 1 for row in values)
        return record
    except subprocess.TimeoutExpired:
        return {'case': case, 'mode': mode, 'timeout': True}
    finally:
        for fd in descriptors:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('cases', nargs='+', choices=sorted(recipes(40)))
    parser.add_argument('--rows', type=int, default=40)
    parser.add_argument('--modes', nargs='+', default=['new', 'legacy'], choices=['new', 'legacy'])
    args = parser.parse_args()
    with (HERE / 'generator-outcomes.jsonl').open('a') as stream:
        for case in args.cases:
            for mode in args.modes:
                record = run_case(case, mode, args.rows)
                line = json.dumps(record)
                stream.write(line + '\n')
                stream.flush()
                print(line, flush=True)


if __name__ == '__main__':
    main()
