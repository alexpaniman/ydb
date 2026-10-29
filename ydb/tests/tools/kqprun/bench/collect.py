#!/usr/bin/env python3
"""Explain the compilation workloads with kqprun, collect spans, and build a report."""

import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAMES = {'1c': '1C', 'tpch': 'TPC-H', 'tpcds': 'TPC-DS', 'medium': '1C medium', 'tiny': '1C tiny'}
PREFIX = '''$round = ($x,$y) -> {return $x;};
$to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };
$to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };
'''


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def workload(name, root, framework):
    if name == '1c':
        queries = [
            (p.stem, p.read_text()) for p in sorted((framework / 'queries').glob('*.yql'), key=lambda p: int(p.stem))
        ]
        schema = framework.joinpath('schema.yql').read_text()
    elif name in ('medium', 'tiny'):
        queries = [('1', (HERE / 'samples' / name / 'query.sql').read_text())]
        schema = (HERE / 'samples' / name / 'schema.sql').read_text()
        # Keep the original schema and query, but avoid depending on pre-created directories.
        for old in ('/Root/pg-rbo/', '/Root/small-rbo/'):
            schema = schema.replace(old, '/Root/')
            queries = [(key, sql.replace(old, '/Root/')) for key, sql in queries]
    else:
        data = root / 'ydb/core/kqp/ut/rbo/data'
        schema = (data / 'schema' / f'{name}.sql').read_text()
        schema = re.sub(r'(CREATE TABLE [^(]+\([^;]*\))\s*;', r'\1 WITH (STORE = COLUMN);', schema)
        queries = [
            (str(i), PREFIX + (data / f'yql-{name}' / f'q{i}.yql').read_text())
            for i in range(1, (22 if name == 'tpch' else 99) + 1)
        ]
    if not queries:
        raise ValueError(f'No queries found for {name}')
    return queries, schema


def select_schema(schema, queries):
    tables = set(re.findall(r'`(public/[^`]+)`', '\n'.join(sql for _, sql in queries)))
    statements = {m[1]: m[0] for m in re.finditer(r'CREATE TABLE\s+`([^`]+)`\s*\([^;]*;', schema)}
    missing = tables - statements.keys()
    if missing:
        raise ValueError(f'Tables missing from schema: {sorted(missing)}')
    return '\n'.join(statements[t] for t in sorted(tables))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        '--source',
        type=Path,
        default=next((p for p in HERE.parents if (p / 'ya').is_file()), Path.cwd()),
        help='YDB source checkout',
    )
    ap.add_argument('--binary', type=Path, help='Built kqprun (default: inside --source)')
    ap.add_argument(
        '--framework', type=Path, default=Path.home() / 'projects/optimizer-test-framework/compilation_bench'
    )
    ap.add_argument('--viewer', type=Path, default=Path.home() / 'projects/violinoscope/violinoscope.py')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--no-peephole', action='store_true', help='Disable new RBO physical-stage peephole')
    ap.add_argument('--workloads', default='1c,tpch,tpcds,medium,tiny')
    ap.add_argument('--batch-size', type=int, default=1000, help='Restart the local database after this many queries')
    ap.add_argument('--limit', type=int, help='First N queries per workload, for a quick check')
    ap.add_argument('--layout', type=Path, default=HERE / 'compilation.layout.json')
    ap.add_argument('--resume', action='store_true', help='Reuse completed batches after checking their input hashes')
    args = ap.parse_args()
    root, out = args.source.resolve(), args.out.resolve()
    binary = (args.binary or root / 'ydb/tests/tools/kqprun/kqprun').resolve()
    if args.batch_size < 1 or (args.limit is not None and args.limit < 1):
        ap.error('batch-size and limit must be positive')
    names = args.workloads.split(',')
    if any(n not in NAMES for n in names):
        ap.error('workloads: ' + ','.join(NAMES))
    if not binary.is_file():
        ap.error(f'Build {binary} first')
    out.mkdir(parents=True, exist_ok=True)
    versions = (root / 'yql/essentials/public/langver/yql_langver_list.inc').read_text()
    version = max(int(a) * 100 + int(b) for a, b in re.findall(r'\{(\d+),(\d+)\}', versions))
    config = f'''TableServiceConfig {{
  EnableNewRBO: true
  EnableFallbackToYqlOptimizer: false
  EnableInlineJoinFiltersAfterCBO: false
  EnableFallbackOnMultipleStatements: false
  AllowOlapDataQuery: true
  DefaultLangVer: {version}
  BackportMode: All
  CompileTimeoutMs: 60000
  EnableNewRBOPhysicalStagePeephole: {str(not args.no_peephole).lower()}
}}
FeatureFlags {{ EnableStatistics: false }}
LogConfig {{ DefaultLevel: 2 }}
'''
    if (out / 'app.conf').exists() and (out / 'app.conf').read_text() != config:
        raise ValueError('Configuration changed; choose another --out')
    write(out / 'app.conf', config)
    write(out / 'limits.conf', 'MaxTableColumns: 1000\nMaxTableIndices: 100\nMaxPaths: 100000\n')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    digest = hashlib.sha256()
    with binary.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    binary_hash = digest.hexdigest()
    manifest = {
        'binary_sha256': binary_hash,
        'commit': commit,
        'binary': str(binary),
        'framework': str(args.framework.resolve()),
        'peephole': not args.no_peephole,
        'level': 15,
        'warmup': 1,
        'config': config,
        'batches': [],
    }
    outputs = []
    for name in names:
        queries, schema = workload(name, root, args.framework)
        if args.limit:
            queries = queries[: args.limit]
        for offset in range(0, len(queries), args.batch_size):
            batch = queries[offset : offset + args.batch_size]
            folder = out / name / f'{offset+1:05d}-{offset+len(batch):05d}'
            ddl = select_schema(schema, batch) if name == '1c' else schema
            signature = hashlib.sha256(json.dumps([binary_hash, config, ddl, batch]).encode()).hexdigest()
            input_file = folder / 'inputs.json'
            if input_file.exists() and json.loads(input_file.read_text())['signature'] != signature:
                raise ValueError(f'{folder}: inputs changed; choose another --out')
            write(
                input_file, json.dumps({'signature': signature, 'queries': [key for key, _ in batch]}, indent=2) + '\n'
            )
            result_file = folder / 'result.json'
            trace = folder / 'spans.jsonl'
            if result_file.exists():
                previous = json.loads(result_file.read_text())
                if not args.resume:
                    raise ValueError(f'{folder} already exists; choose another --out or use --resume')
                if previous['signature'] != signature:
                    raise ValueError(f'{folder}: inputs changed; choose another --out')
                if not trace.is_file():
                    raise ValueError(f'{trace} is missing')
                result = previous
                print(f'Reuse {name}: {len(batch)} queries', flush=True)
            else:
                write(folder / 'schema.sql', ddl)
                for i, (label, sql) in enumerate(batch):
                    # Both are EXPLAIN requests. The first warms metadata; the second is the observation.
                    write(folder / 'queries' / f'{i:05d}-q{label}-warmup.sql', '-- warm-up\n' + sql)
                    write(folder / 'queries' / f'{i:05d}-q{label}.sql', f'-- {name} q{label}, measured\n' + sql)
                command = [
                    str(binary),
                    '--app-config',
                    str(out / 'app.conf'),
                    '--scheme-limits',
                    str(out / 'limits.conf'),
                    '--scheme-query',
                    str(folder / 'schema.sql'),
                    '--script-query-dir',
                    str(folder / 'queries'),
                    '-C',
                    'query',
                    '-A',
                    'explain',
                    '--continue-after-fail',
                    '--timeout',
                    '120000',
                    '--trace-file',
                    str(trace),
                    '--trace-dataset',
                    NAMES[name],
                    '--trace-level',
                    '15',
                    '--log-file',
                    str(folder / 'server.log'),
                    '--result-file',
                    '/dev/null',
                    '--script-ast-file',
                    '/dev/null',
                    '--script-plan-file',
                    '/dev/null',
                ]
                print(f'Collect {name}: queries {offset+1}–{offset+len(batch)}', flush=True)
                started = time.time()
                with (folder / 'run.log').open('w') as log:
                    process = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
                log = (folder / 'run.log').read_text()
                if process.returncode and 'query requests failed' not in log:
                    raise RuntimeError(f'Collection stopped ({process.returncode}); see {folder / "run.log"}')
                result = {
                    'signature': signature,
                    'workload': name,
                    'queries': len(batch),
                    'returncode': process.returncode,
                    'seconds': round(time.time() - started, 2),
                    'command': command,
                }
                write(result_file, json.dumps(result, indent=2) + '\n')
            outputs.append(str(trace))
            manifest['batches'].append(result)
            write(out / 'manifest.json', json.dumps(manifest, indent=2) + '\n')
    command = [
        sys.executable,
        str(args.viewer.resolve()),
        *outputs,
        '--unit',
        'Compile query',
        '--label-attr',
        'query.name',
        '--skip-label',
        r'.*-warmup\.sql$',
        '--label-regex',
        r'^\d+-(?P<label>q\d+)',
        '--layout',
        str(args.layout.resolve()),
        '--left',
        '1C',
        '--right',
        'TPC-H,TPC-DS',
        '--title',
        'Compilation · peephole ' + ('off' if args.no_peephole else 'on'),
        '-o',
        str(out / 'violinoscope.html'),
    ]
    subprocess.run(command, check=True)
    print(out / 'violinoscope.html', flush=True)


if __name__ == '__main__':
    main()
