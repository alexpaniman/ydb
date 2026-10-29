"""Compositional correlation checks; retain only semantic verdicts and hashes.

No generated query, fixture, result payload, or runner diagnostics is persisted.
The caller controls scale/seed so this file is a generator, not a saved input.
"""
import argparse
import hashlib
import json
import os
import resource
import subprocess

import generate_and_verify as g


def table(name, columns):
    fields = [column + ' ' + typ for column, typ in columns]
    fields.append('PRIMARY KEY (id)')
    return 'CREATE TABLE ' + name + ' (' + ', '.join(fields) + ')'


def insert_rows(name, column_types, rows):
    arms = [g.select([g.alias(g.typed(typ, value), column)
                      for (column, typ), value in zip(column_types, row)])
            for row in rows]
    return 'UPSERT INTO ' + name + ' ' + g.combine(arms)


def build(case, seed):
    magnitude = abs(seed) % 1000 + 1
    key_type = 'Double' if case.startswith('nan') else 'Int64'
    inner_key_type = 'Uint64' if case.startswith('typed') else key_type
    columns = [('id', 'Uint64 NOT NULL'), ('k', key_type), ('v', 'Int64')]
    inner_columns = [(name, inner_key_type if name == 'k' else typ) for name, typ in columns]
    schema = '; '.join([table('outer_rows', columns), table('inner_rows', inner_columns)]) + ';'
    outer_key = 'nan' if case.startswith('nan') else magnitude
    outer_rows = [(1, outer_key, magnitude)]
    expected = None
    if case.startswith('demand'):
        inner_rows = [(1, magnitude, magnitude * 13)]
        if case == 'demand_extra_domains':
            inner_rows += [(i, magnitude + 1, magnitude * i) for i in range(2, 4)]
        expected = magnitude * 13
    elif case.startswith('typed'):
        inner_rows = [(i, magnitude * i, magnitude + i) for i in range(1, 3)]
        expected = magnitude + 1
        if case == 'typed_same_type_control':
            inner_columns = columns
            inner_key_type = key_type
            schema = '; '.join([table('outer_rows', columns), table('inner_rows', inner_columns)]) + ';'
    elif case.startswith('nan'):
        inner_rows = [(1, outer_key, magnitude)]
        if case == 'nan_finite_control':
            outer_rows = [(1, magnitude, magnitude)]
            inner_rows = [(1, magnitude, magnitude)]
            expected = magnitude
    else:
        inner_rows = [(1, magnitude + 1, magnitude)]
        expected = magnitude * 17
    outer_types = [(name, typ.removesuffix(' NOT NULL')) for name, typ in columns]
    inner_types = [(name, typ.removesuffix(' NOT NULL')) for name, typ in inner_columns]
    seeds = [insert_rows('outer_rows', outer_types, outer_rows),
             insert_rows('inner_rows', inner_types, inner_rows)]
    predicate = g.op('i.k', '=', 'o.k')
    expression = 'i.v'
    if case.startswith('empty'):
        expression = g.call('Coalesce', g.call('SUM', 'i.v'), g.typed('Int64', expected))
        if case == 'empty_count_restore_control':
            # COUNT contributes zero for the demanded empty domain and stays live.
            expression = g.op(expression, '+', g.call('CAST', g.call('COUNT', '*') + ' AS Int64'))
    scalar = '(' + g.select([expression], 'inner_rows AS i', where=predicate) + ')'
    query = g.select([g.alias(scalar, 'observed')], 'outer_rows AS o')
    return schema, seeds, query, expected


CASES = ['demand_extra_domains', 'demand_requested_only_control',
         'typed_capture_collision', 'typed_same_type_control',
         'nan_domain_equality', 'nan_finite_control',
         'empty_scalar_projection', 'empty_count_restore_control']


def run(case, mode, seed):
    schema, seeds, query, expected = build(case, seed)
    descriptors = []
    def memory(label, value):
        fd = g.anonymous_text(label, value)
        descriptors.append(fd)
        return '/proc/self/fd/' + str(fd)
    query = 'PRAGMA YqlSelect=' + g.quote('disable' if mode == 'legacy' else 'force') + '; ' + query + ';'
    command = [str(g.BINARY), '-c', str(g.HERE / (mode + '.conf')),
               '-s', memory('schema', schema)]
    for statement in seeds:
        command += ['-p', memory('fixture', statement + ';')]
    command += ['-p', memory('check', query)]
    result_path = memory('result', '')
    result_fd = descriptors[-1]
    command += ['-C', 'query', '--log-file', '/dev/null', '--result-file', result_path]
    try:
        completed = subprocess.run(command, pass_fds=descriptors, capture_output=True,
                                   timeout=75, preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        os.lseek(result_fd, 0, os.SEEK_SET)
        payload = os.read(result_fd, 8 * 1024 * 1024)
        record = {'case': case, 'mode': mode, 'exit_code': completed.returncode,
                  'result_sha256': hashlib.sha256(payload).hexdigest(),
                  'row_count': len(payload.splitlines())}
        if completed.returncode:
            record['failure_category'] = g.failure_category((completed.stdout + completed.stderr).decode(errors='replace'))
            record['semantic_oracle_pass'] = False
        else:
            try:
                actual = [json.loads(line) for line in payload.splitlines()]
                record['semantic_oracle_pass'] = actual == [{'observed': expected}]
                record['unexpected_null_result'] = expected is not None and actual == [{'observed': None}]
            except (ValueError, TypeError):
                record['result_not_jsonl'] = True
        return record
    except subprocess.TimeoutExpired:
        return {'case': case, 'mode': mode, 'timeout': True}
    finally:
        for fd in descriptors:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('cases', nargs='*', choices=CASES)
    parser.add_argument('--seed', type=int, default=41)
    parser.add_argument('--modes', nargs='+', choices=['new', 'legacy'], default=['new', 'legacy'])
    args = parser.parse_args()
    with (g.HERE / 'correlation-oracle-outcomes.jsonl').open('a') as output:
        for case in args.cases or CASES:
            for mode in args.modes:
                line = json.dumps(run(case, mode, args.seed))
                output.write(line + '\n')
                output.flush()
                print(line, flush=True)


if __name__ == '__main__':
    main()
