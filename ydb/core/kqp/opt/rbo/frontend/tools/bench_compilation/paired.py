#!/usr/bin/env python3
"""Render only queries with successful measurements in both frozen variants."""
import argparse
import collections
import json
from pathlib import Path

import run


def filter_trace(source, target, queries):
    allowed = set()
    def spans(payload):
        for resource in payload['resourceSpans']:
            for scope in resource['scopeSpans']:
                yield from scope['spans']
    with source.open() as stream:
        for line in stream:
            for span in spans(json.loads(line)):
                if span['name'] != 'kqprun.query': continue
                attributes = {a['key']: run.value(a['value']) for a in span.get('attributes', [])}
                if attributes.get('query.name') in queries: allowed.add(span['traceId'])
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open() as source_stream, target.open('w') as output:
        for line in source_stream:
            payload = json.loads(line)
            for resource in payload['resourceSpans']:
                for scope in resource['scopeSpans']:
                    scope['spans'] = [s for s in scope['spans'] if s['traceId'] in allowed]
                resource['scopeSpans'] = [s for s in resource['scopeSpans'] if s['spans']]
            payload['resourceSpans'] = [r for r in payload['resourceSpans'] if r['scopeSpans']]
            if payload['resourceSpans']: output.write(json.dumps(payload, separators=(',', ':')) + '\n')
    return len(allowed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--comparison', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    source = json.loads((args.comparison / 'manifest.json').read_text())
    run.VIEWER = Path(source.get('viewer_path', str(run.VIEWER)))
    states = collections.defaultdict(lambda: collections.defaultdict(dict))
    for batch in source['batches']:
        queries = states[batch['workload']][batch['variant']]
        for row in batch['measurements']:
            queries[row['query']] = queries.get(row['query'], True) and row['success']
    common = {w: {q for q, ok in modes['before'].items() if ok and modes['after'].get(q, False)}
              for w, modes in states.items()}
    batches = []
    for batch in source['batches']:
        selected = common[batch['workload']]
        if not selected: continue
        trace = Path(batch['trace'])
        target = args.out / batch['variant'] / batch['workload'] / trace.parent.name / trace.name
        count = filter_trace(trace, target, selected)
        if count:
            batches.append(dict(batch, trace=str(target.resolve()), trace_sha256=run.digest(target),
                                measurements=[r for r in batch['measurements'] if r['query'] in selected]))
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = dict(source, batches=batches, comparison='Successful paired queries only',
                    source_manifest=str((args.comparison / 'manifest.json').resolve()),
                    paired_queries={w: sorted(q) for w, q in common.items()})
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    run.summarize_all(args.out, batches)
    run.render(args.out, batches)


if __name__ == '__main__': main()
