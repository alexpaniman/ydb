#!/usr/bin/env python3
"""Repeat a traced comparison with tracing disabled; retain every request result."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import time

from run import binary_digest


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def parse_log(text, expected, warmups, query_count):
    text = re.sub(r'\x1b\[[0-9;]*m', '', text)
    rows = []
    current = None
    starts = 0
    for line in text.splitlines():
        start = re.search(r" Executing query(?: (\d+))? \('([^']+)'\)(?:, loop (\d+))?", line)
        if start:
            if current is not None:
                if current['success'] and current['request_ms'] is None:
                    raise ValueError(('request has no duration', current))
                rows.append(current)
            starts += 1
            current = dict(query=Path(start[2]).name, index=int(start[1] or 0), loop=int(start[3] or 0),
                           warmup='(warmup, tracing disabled)' in line, success=True, request_ms=None)
        if current is not None:
            if re.search(r'^Query \d+ .*failed:', line) or 'Failed to execute query, reason:' in line:
                current['success'] = False
            elapsed = re.search(r'Generic request finished\. (Total|Estimated) duration: ([\d.]+)(us|ms|s)', line)
            if elapsed:
                if current['request_ms'] is not None:
                    raise ValueError(('duplicate request duration', current))
                current['request_ms'] = float(elapsed[2]) * {'us':.001, 'ms':1, 's':1000}[elapsed[3]]
                current['duration_source'] = elapsed[1].lower()
    if current is not None:
        if current['success'] and current['request_ms'] is None:
            raise ValueError(('request has no duration', current))
        rows.append(current)
    assert starts == expected + warmups * query_count, (starts, expected, warmups, query_count)
    assert sum(row['warmup'] for row in rows) == warmups * query_count
    measured = [row for row in rows if not row['warmup']]
    assert len(measured) == expected
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--comparison', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--warmups', type=int, default=5)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    if min(args.repeats, args.warmups) < 1: parser.error('counts must be positive')
    manifest_path = args.comparison / 'manifest.json'
    source = json.loads(manifest_path.read_text())
    native_modes = source.get('native_modes', ['after'])
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = dict(source=str(manifest_path.resolve()), source_sha256=digest(manifest_path),
                    script_sha256=digest(__file__), tracing=False, repeats=args.repeats, warmups=args.warmups, batches=[])
    # Match the traced driver: verify each frozen runner once per invocation.
    # Re-reading an 11 GB binary for every batch can dominate control runs.
    for mode, binary in source['binaries'].items():
        assert binary_digest(Path(binary)) == source['binary_sha256'][mode], binary
    for original in source['batches']:
        mode, workload = original['variant'], original['workload']
        destination = args.out / mode / workload / Path(original['trace']).parent.name
        destination.mkdir(parents=True, exist_ok=True)
        # Keep the same schema, queries, config and runner flags; only the
        # tracing, observation count and output paths differ.
        command = []
        iterator = iter(original['command'])
        for token in iterator:
            if token in ('--trace-file', '--trace-dataset', '--trace-level'): next(iterator)
            elif token == '--log-file': next(iterator); command += [token, str(destination / 'server.log')]
            elif token == '--warmup-count': next(iterator); command += [token, str(args.warmups)]
            elif token == '--loop-count': next(iterator); command += [token, str(args.repeats)]
            else: command.append(token)
        signature = hashlib.sha256(json.dumps([original['signature'], command]).encode()).hexdigest()
        result_path = destination / 'result.json'
        if result_path.exists():
            if not args.resume: raise ValueError(f'{destination} exists; use --resume')
            result = json.loads(result_path.read_text())
            assert result['signature'] == signature
            assert digest(destination / 'run.log') == result['log_sha256']
        else:
            print(f'Untraced {mode} {workload} {destination.name}', flush=True)
            start = time.monotonic()
            with (destination / 'run.log').open('w') as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            text = (destination / 'run.log').read_text()
            if completed.returncode and (completed.returncode != 1 or 'query requests failed' not in text):
                raise RuntimeError(f'Runner failed: {destination}/run.log')
            rows = parse_log(text, args.repeats * original['queries'], args.warmups, original['queries'])
            result = dict(signature=signature, variant=mode, workload=workload, command=command,
                          log_sha256=digest(destination / 'run.log'), seconds=time.monotonic()-start,
                          returncode=completed.returncode, measurements=rows)
            result_path.write_text(json.dumps(result, indent=2)+'\n')
        manifest['batches'].append(result)
        (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    summary = {}
    for workload in {b['workload'] for b in manifest['batches']}:
        rows = {mode:{} for mode in ('before','after')}
        failures = {mode:[] for mode in rows}
        for batch in manifest['batches']:
            if batch['workload'] != workload: continue
            for row in batch['measurements']:
                if row['success']: rows[batch['variant']].setdefault(row['query'], []).append(row['request_ms'])
                else: failures[batch['variant']].append(row['query'])
        paired = rows['before'].keys() & rows['after'].keys()
        ratios = [statistics.median(rows['after'][q])/statistics.median(rows['before'][q]) for q in paired]
        summary[workload] = dict(paired_queries=len(paired), failures=failures,
            geomean_request_duration_after_over_before=math.exp(statistics.mean(map(math.log,ratios))) if ratios else None)
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    for workload, result in summary.items():
        print(json.dumps(dict(result, workload=workload,
            failures={mode: len(queries) for mode, queries in result['failures'].items()})))
    failed = [r for b in manifest['batches'] if b['variant'] in native_modes for r in b['measurements'] if not r['success']]
    failed_batches = [b for b in manifest['batches'] if b['variant'] in native_modes and b['returncode']]
    if failed or failed_batches:
        raise SystemExit(f'{len(failed)} measured native request failures; {len(failed_batches)} batches had a measured or warmup failure')

if __name__ == '__main__': main()
