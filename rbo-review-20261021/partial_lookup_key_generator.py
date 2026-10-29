"""Generate partial-identity lookup checks entirely in anonymous memory.

Only semantic verdicts, counts, hashes and diagnostic categories are retained.
No rendered query, fixture, result payload or runner trace is saved or printed.
"""
import argparse
import hashlib
import json
import os
import re
import resource
import subprocess
import generate_and_verify as g


def table(name, columns, keys, index=None):
    definitions = [column + ' ' + typ for column, typ in columns]
    definitions.append('PRIMARY KEY (' + ', '.join(keys) + ')')
    if index:
        index_name, index_keys = index
        definitions.append('INDEX ' + index_name + ' GLOBAL ON (' + ', '.join(index_keys) + ')')
    return 'CREATE TABLE ' + name + ' (' + ', '.join(definitions) + ')'


def insert(name, columns, rows):
    arms = [g.select([g.alias(g.typed(typ.removesuffix(' NOT NULL'), value), col)
                      for (col, typ), value in zip(columns, row)]) for row in rows]
    return 'UPSERT INTO ' + name + ' ' + g.combine(arms)


def build(n, full_key, kind):
    n = max(4, n)
    left_columns = [(name, 'Int32 NOT NULL') for name in ('prefix', 'suffix', 'selector', 'payload')]
    right_columns = [(name, 'Int32 NOT NULL') for name in ('id', 'join_key', 'cutoff')]
    left_rows = [(1, i, 1, i) for i in range(1, n + 1)]
    left_rows += [(2, i, 2, i) for i in range(1, n * 4 + 1)]
    right_rows = [(1, 1, n // 2 + 1)]
    schema = '; '.join([table('identity_rows', left_columns, ['prefix', 'suffix'], ('by_selector', ['selector'])),
                        table('matching_rows', right_columns, ['id'])]) + ';'
    seeds = [insert('identity_rows', left_columns, left_rows), insert('matching_rows', right_columns, right_rows)]
    selected_columns = ['prefix', 'payload'] + (['suffix'] if full_key else [])
    selected = g.select(selected_columns, 'identity_rows', where=g.op('selector', '=', g.typed('Int32', 1)))
    predicate = g.op(g.op('l.prefix', '=', 'r.join_key'), 'AND', g.op('l.payload', '<', 'r.cutoff'))
    output = [g.alias('l.payload', 'v')]
    if kind == 'LEFT':
        output.append(g.alias('r.id', 'rid'))
    if full_key:
        output.append(g.alias('l.suffix', 'preserved_key'))
    query = g.select(output, g.join(g.derived(selected, 'l'), 'matching_rows AS r', predicate, kind))
    expected = []
    for prefix, suffix, selector, payload in left_rows:
        if selector != 1:
            continue
        matched = [rid for rid, key, cutoff in right_rows if prefix == key and payload < cutoff]
        if kind == 'LEFT':
            expected.extend((payload, rid) for rid in (matched or [None]))
        elif kind == 'LEFT SEMI' and matched:
            expected.append((payload,))
        elif kind == 'LEFT ONLY' and not matched:
            expected.append((payload,))
    return schema, seeds, query, expected


def run(mode, n, full_key, kind):
    descriptors = []
    def memory(label, value):
        fd = g.anonymous_text(label, value)
        descriptors.append(fd)
        return '/proc/self/fd/' + str(fd)
    schema, seeds, query, expected = build(n, full_key, kind)
    settings = (g.HERE / (mode + '.conf')).read_text()
    for flag, value in [('UseBlockHashJoin', 'false'), ('EnableQueryServiceSpilling', 'true'),
                        ('EnableSpillingInHashJoinShuffleConnections', 'true'), ('EnableSpillingNodes', '"All"'),
                        ('EnableInlineJoinFiltersAfterCBO', 'true'), ('EnablePruneKeyColumns', 'true')]:
        pattern = r'(?m)^\s*' + flag + r':\s*[^\n]+'
        if re.search(pattern, settings):
            settings = re.sub(pattern, '\n ' + flag + ': ' + value, settings)
        else:
            settings = settings.replace('TableServiceConfig {', 'TableServiceConfig {\n ' + flag + ': ' + value, 1)
    pragmas = [('YqlSelect', 'force' if mode == 'new' else 'disable'),
               ('kikimr.CostBasedOptimizationLevel', '0'), ('kikimr.HashJoinMode', 'grace'),
               ('ydb.OptDisableAutoIndexSelection', 'false')]
    query = ''.join('PRAGMA ' + name + '=' + g.quote(value) + '; ' for name, value in pragmas) + query + ';'
    command = [str(g.BINARY), '-c', memory('generated-config', settings), '-u', str(g.MATH),
               '-s', memory('generated-schema', schema)]
    for i, seed in enumerate(seeds):
        command += ['-p', memory('generated-data-' + str(i), seed + ';')]
    command += ['-p', memory('generated-check', query)]
    result = memory('result-buffer', '')
    result_fd = descriptors[-1]
    command += ['-C', 'query', '--log-file', '/dev/null', '--result-file', result]
    record = {'case': 'partial_lookup_key', 'mode': mode, 'full_key_control': full_key, 'join_kind': kind}
    try:
        process = subprocess.run(command, pass_fds=descriptors, capture_output=True, timeout=90,
                                 preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        os.lseek(result_fd, 0, os.SEEK_SET)
        payload = os.read(result_fd, 8 * 1024 * 1024)
        record.update(exit_code=process.returncode, row_count=len(payload.splitlines()),
                      result_sha256=hashlib.sha256(payload).hexdigest(), expected_row_count=len(expected))
        if process.returncode:
            diagnostic = (process.stdout + process.stderr).decode(errors='replace')
            record['failure_category'] = g.failure_category(diagnostic)
            record['diagnostic_source_locations'] = sorted(set(re.findall(r'(?:ydb|yql|util)/[a-zA-Z0-9_/.-]+\.(?:cpp|h):[0-9]+', diagnostic)))[:16]
            record['diagnostic_categories'] = [term for term in ['Unknown pragma', 'No key columns', 'cannot be null', 'channel spilling disabled', 'type annotation', 'unexpected'] if term.lower() in diagnostic.lower()]
        else:
            actual = [json.loads(line) for line in payload.splitlines()]
            projected = [(row['v'], row['rid']) if kind == 'LEFT' else (row['v'],) for row in actual]
            canonical = lambda rows: sorted(json.dumps(row, separators=(',', ':')) for row in rows)
            record['semantic_oracle_pass'] = canonical(projected) == canonical(expected)
            record['row_count_oracle_pass'] = len(actual) == len(expected)
        return record
    except subprocess.TimeoutExpired:
        return dict(record, timeout=True)
    finally:
        for fd in descriptors:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', type=int, default=6)
    parser.add_argument('--modes', nargs='+', choices=['new', 'legacy'], default=['new', 'legacy'])
    parser.add_argument('--kinds', nargs='+', choices=['left', 'semi', 'anti'], default=['left'])
    parser.add_argument('--controls', nargs='+', choices=['partial', 'full'], default=['partial', 'full'])
    args = parser.parse_args()
    kinds = {'left': 'LEFT', 'semi': 'LEFT SEMI', 'anti': 'LEFT ONLY'}
    with (g.HERE / 'generator-outcomes.jsonl').open('a') as output:
        for kind in args.kinds:
            for control in args.controls:
                for mode in args.modes:
                    record = run(mode, args.rows, control == 'full', kinds[kind])
                    line = json.dumps(record)
                    output.write(line + '\n')
                    output.flush()
                    print(line, flush=True)


if __name__ == '__main__':
    main()
