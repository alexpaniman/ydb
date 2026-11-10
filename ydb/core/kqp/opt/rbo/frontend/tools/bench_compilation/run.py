#!/usr/bin/env python3
"""Collect native frontend before/after traces after untraced in-process EXPLAIN loops."""
import argparse
import collections
import csv
import hashlib
import json
import math
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

import corpus

HERE = Path(__file__).resolve().parent
VIEWER = Path.home() / 'projects/violinoscope/violinoscope.py'
FRAMEWORK = Path.home() / 'projects/optimizer-test-framework/compilation_bench'
NAMES = corpus.NAMES
FRONTEND_PHASES = ('Native parse', 'Native table discovery', 'Native binding and scalar typing',
                   'Native plan formatting', 'Native operator translation')


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def binary_digest(path):
    # Frozen runners may carry the hash computed when they were copied. Reuse
    # it only while the same inode and all content/change timestamps match.
    # Hashing two multi-gigabyte runners again can dominate a small experiment.
    metadata = path.with_suffix('.json')
    if metadata.is_file():
        saved = json.loads(metadata.read_text())
        stat = path.stat()
        identity = {key: getattr(stat, key) for key in
                    ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')}
        if saved.get('binary') == str(path) and saved.get('file_identity') == identity:
            return saved['sha256']
    return digest(path)


def value(v):
    return next(iter(v.values()))


def summarize(trace, expected, first_index, native):
    groups = collections.defaultdict(list)
    for line in trace.open():
        for resource in json.loads(line)['resourceSpans']:
            for scope in resource['scopeSpans']:
                for span in scope['spans']:
                    groups[span['traceId']].append(span)
    result = []
    for spans in groups.values():
        parents = [s for s in spans if s['name'] == 'kqprun.query']
        roots = [s for s in spans if s['name'] == 'Compile query']
        assert len(parents) == 1, (trace, 'request count', len(parents))
        assert len(roots) == 1, (trace, 'compile count', len(roots))
        attrs = {a['key']: value(a['value']) for a in parents[0].get('attributes', [])}
        assert int(attrs['query.index']) >= first_index, (trace, 'warmup was traced', attrs)
        root = roots[0]
        names = {s['name'] for s in spans}
        if not native:
            assert 'Native parse' not in names, trace
        # The pre-migration revision emits the outer compiler span only.
        # Both configurations disable AST and compiled-query caches; the native
        # revision additionally proves every required frontend/RBO phase below.
        if native:
            assert 'Native parse' in names, (trace, 'missing native parse')
        fallback = []
        phase_ms = collections.defaultdict(float)
        for s in spans:
            phase_ms[s['name']] += (int(s['endTimeUnixNano'])-int(s['startTimeUnixNano']))/1e6
            if s['name'] == 'Native frontend fallback':
                fallback += [value(a['value']) for a in s.get('attributes', []) if a['key'] == 'fallback.reason']
        success = root.get('status', {}).get('code') != 2 and parents[0].get('status', {}).get('code') != 2
        if success and native and not fallback:
            required = {'Native parse', 'Native table discovery', 'Native binding and scalar typing',
                        'Native operator translation', 'New RBO optimization', 'Emit native physical plan'}
            assert required <= names, (trace, attrs, required - names)
            assert all(s.get('status', {}).get('code') != 2 for s in spans
                       if s['name'] in required), (trace, attrs)
        frontend_ms = sum(phase_ms[n] for n in FRONTEND_PHASES)
        rbo_ms=phase_ms['New RBO optimization']
        result.append(dict(frontend_ms=frontend_ms, operator_typing_ms=phase_ms['Native operator typing'], rbo_ms=rbo_ms, frontend_over_rbo=frontend_ms/rbo_ms if rbo_ms else None, query=attrs['query.name'], index=int(attrs['query.index']), success=success,
                           ms=phase_ms['Compile query'], phases=dict(phase_ms), fallback=fallback,
                           frontend_phases_measured=[n for n in FRONTEND_PHASES if n in names],
                           native=bool(native and not fallback and 'Native operator translation' in names)))
    assert len(result) == expected, (trace, len(result), expected)
    return result


def render(out, batches):
    paths = [b['trace'] for b in batches]
    common = [sys.executable, str(VIEWER), '--unit', 'Compile query', '--label-attr', 'query.name',
              '--label-regex', r'^\d+-(?P<label>.+)\.sql$', '--layout', str(HERE / 'compilation.layout.json'),
              '--timeline', 'unit']
    def report(name, inputs, left, right, title):
        command = common + inputs + ['--left', left, '--right', right, '--title', title, '-o', str(out / name)]
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        print('Report:', out / name, flush=True)
    for mode in ('before', 'after'):
        selected = [b for b in batches if b['variant'] == mode]
        report(f'variant-{mode}.html', [NAMES[b['workload']] + '=' + b['trace'] for b in selected],
               '1C', 'TPC-H,TPC-DS', f'Native frontend migration · variant={mode} · warmed EXPLAIN · peephole off')
    report('comparison.html', paths,
           ','.join(NAMES[n] + ' · before' for n in NAMES),
           ','.join(NAMES[n] + ' · after' for n in NAMES),
           'Native frontend migration · before / after · warmed EXPLAIN · peephole off')
    for name in NAMES:
        selected = [b['trace'] for b in batches if b['workload'] == name]
        if selected:
            report(f'comparison-{name}.html', selected, NAMES[name] + ' · before', NAMES[name] + ' · after',
                   f'Native frontend migration · {NAMES[name]} · before / after')


def summarize_all(out, batches):
    rows, summary = [], []
    for b in batches:
        for q in b['measurements']:
            rows.append(dict(workload=b['workload'], variant=b['variant'], **q))
    write(out / 'measurements.json', json.dumps(rows, indent=2) + '\n')
    with (out / 'measurements.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=['workload','variant','query','index','success','native','ms','fallback'])
        writer.writeheader()
        for r in rows:
            writer.writerow({k:r[k] for k in writer.fieldnames})
    for name in NAMES:
        selected = [r for r in rows if r['workload'] == name]
        if not selected:
            continue
        modes = {}
        for mode in ('before', 'after'):
            rr = [r for r in selected if r['variant'] == mode]
            ok = [r for r in rr if r['success']]
            modes[mode] = dict(requests=len(rr), success=len(ok), failures=len(rr)-len(ok),
                               native=sum(r['native'] for r in ok), fallback=sum(bool(r['fallback']) for r in ok),
                               median_ms=statistics.median(r['ms'] for r in ok) if ok else None,
                               total_ms=sum(r['ms'] for r in ok))
        paired = {}
        for mode in ('before', 'after'):
            queries = collections.defaultdict(list)
            for r in selected:
                if r['variant'] == mode and r['success']:
                    queries[r['query']].append(r['ms'])
            paired[mode] = {q:statistics.median(v) for q,v in queries.items()}
        both = paired['before'].keys() & paired['after'].keys()
        ratios = [paired['after'][q]/paired['before'][q] for q in both]
        native_queries = {r['query'] for r in selected if r['variant'] == 'after' and r['success'] and r['native']}
        native_both = both & native_queries
        native_ratios = [paired['after'][q]/paired['before'][q] for q in native_both]
        reasons = collections.Counter(reason for r in selected if r['variant'] == 'after' for reason in r['fallback'])
        overhead=[r['frontend_over_rbo'] for r in selected if r['variant']=='after' and r['success'] and r['frontend_over_rbo'] is not None]
        overhead.sort()
        summary.append(dict(workload=name, native_frontend_over_rbo_median=statistics.median(overhead) if overhead else None, native_frontend_over_rbo_p95=overhead[min(len(overhead)-1,int(len(overhead)*.95))] if overhead else None, modes=modes, common_successes=len(both),
                            paired_geomean_after_over_before=math.exp(statistics.mean(map(math.log,ratios))) if ratios else None,
                            native_common_successes=len(native_both),
                            native_paired_geomean_after_over_before=math.exp(statistics.mean(map(math.log,native_ratios))) if native_ratios else None,
                            only_before_success=sorted(paired['before'].keys()-paired['after'].keys()),
                            only_after_success=sorted(paired['after'].keys()-paired['before'].keys()),
                            fallback_reasons=dict(reasons)))
    write(out / 'summary.json', json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2),flush=True)


def main():
    global VIEWER
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=Path.cwd())
    ap.add_argument('--framework', type=Path, default=FRAMEWORK, help='1C compilation_bench directory')
    ap.add_argument('--viewer', type=Path, default=VIEWER, help='Violinoscope Python entry point')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--before', type=Path, required=True)
    ap.add_argument('--after', type=Path, required=True)
    ap.add_argument('--before-native', action='store_true', help='Compare two native checkpoints; require native coverage on both')
    ap.add_argument('--limit', type=int)
    ap.add_argument('--workloads', default='1c,tpch,tpcds,clickbench,medium,tiny,tpch-perf')
    ap.add_argument('--warmups', type=int, default=5)
    ap.add_argument('--repeats', type=int, default=1)
    ap.add_argument('--resume', action='store_true')
    ap.add_argument('--render-only', action='store_true')
    ap.add_argument('--modes', default='before,after')
    ap.add_argument('--no-render', action='store_true')
    args=ap.parse_args()
    VIEWER=args.viewer.resolve()
    out=args.out.resolve(); root=args.source.resolve()
    out.mkdir(parents=True,exist_ok=True)
    if args.render_only:
        manifest=json.loads((out/'manifest.json').read_text())
        summarize_all(out,manifest['batches']);render(out,manifest['batches']);return
    binaries={'before':args.before.resolve(), 'after':args.after.resolve()}
    native_modes=['before', 'after'] if args.before_native else ['after']
    version=max(int(y)*100+int(r) for y,r in re.findall(r'\{(\d+),(\d+)\}',(root/'yql/essentials/public/langver/yql_langver_list.inc').read_text()))
    binary_hashes={mode:binary_digest(path) for mode,path in binaries.items()}
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    patch=subprocess.check_output(['git','diff'],cwd=root)
    existing = json.loads((out/'manifest.json').read_text()) if args.resume and (out/'manifest.json').exists() else None
    if existing:
        assert digest(out/'source.patch') == existing['patch_sha256'], 'Saved source snapshot changed'
    else:
        write(out/'source.patch',patch.decode())
    manifest=dict(comparison="native frontend migration",head=head,
                  native_modes=native_modes,
                  phase_granularity={mode: "native frontend and RBO phases" if mode in native_modes else "outer Compile query span" for mode in binaries},
                  viewer_path=str(VIEWER),
                  viewer_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=VIEWER.parent,text=True).strip(),
                  runner_sha256=digest(Path(__file__)),layout_sha256=digest(HERE/'compilation.layout.json'),
                  binaries={mode:str(path) for mode,path in binaries.items()},binary_sha256=binary_hashes,patch_sha256=hashlib.sha256(patch).hexdigest(),
                  warmups=args.warmups,repeats=args.repeats,peephole=False,threads=1,limited=args.limit,batches=[])
    if existing:
        assert existing.get('native_modes', ['after']) == native_modes, 'Resume native modes changed'
        for key in ('binary_sha256', 'warmups', 'repeats', 'limited'):
            assert existing[key] == manifest[key], ('Resume settings changed', key)
        manifest = dict(existing, batches=[])
    configs={}
    for mode in ('before','after'):
        configs[mode]=f'''TableServiceConfig {{
  EnableNewRBO: true
  {'NativeFrontendMode: NATIVE_REQUIRED' if mode in native_modes else ''}
  EnableFallbackToYqlOptimizer: false
  EnableInlineJoinFiltersAfterCBO: false
  EnableFallbackOnMultipleStatements: false
  EnableAstCache: false
  CompileQueryCacheSize: 0
  AllowOlapDataQuery: true
  DefaultLangVer: {version}
  BackportMode: All
  CompileTimeoutMs: 60000
  EnableNewRBOPhysicalStagePeephole: false
}}
FeatureFlags {{ EnableStatistics: false }}
LogConfig {{ DefaultLevel: 2 }}
'''
        write(out/f'variant-{mode}.conf',configs[mode])
    write(out/'limits.conf','MaxTableColumns: 1000\nMaxTableIndices: 100\nMaxPaths: 100000\n')
    batch_number=0
    for name in args.workloads.split(','):
        queries,schema=corpus.workload(name,root,args.framework.resolve())
        if args.limit: queries=queries[:args.limit]
        for offset in range(0,len(queries),1000):
            batch=queries[offset:offset+1000]
            folder=out/'inputs'/name/f'{offset+1:05d}-{offset+len(batch):05d}'
            ddl=corpus.select_schema(schema,batch) if name=='1c' else schema
            write(folder/'schema.sql',ddl)
            for i,(label,sql) in enumerate(batch):
                write(folder/'queries'/f'{i:05d}-{name}-q{label}.sql',sql)
            # Alternate order to reduce systematic drift; variants run sequentially.
            modes=('before','after') if batch_number%2==0 else ('after','before')
            batch_number+=1
            for mode in modes:
                if mode not in args.modes.split(','): continue
                binary=binaries[mode]; binary_hash=binary_hashes[mode]
                dest=out/mode/name/f'{offset+1:05d}-{offset+len(batch):05d}'
                dest.mkdir(parents=True,exist_ok=True)
                trace=dest/'spans.jsonl'
                signature=hashlib.sha256(json.dumps([binary_hash,configs[mode],batch,ddl,args.warmups,args.repeats]).encode()).hexdigest()
                result_file=dest/'result.json'
                if result_file.exists():
                    if not args.resume: raise ValueError(f'{result_file} exists; use --resume')
                    result=json.loads(result_file.read_text())
                    assert result['signature']==signature,result_file
                    assert digest(trace)==result['trace_sha256'],trace
                    print('Reuse',dest,flush=True)
                else:
                    command=[str(binary),'--app-config',str(out/f'variant-{mode}.conf'),'--scheme-limits',str(out/'limits.conf'),
                             '--scheme-query',str(folder/'schema.sql'),'--script-query-dir',str(folder/'queries'),
                             '-C','query','-A','explain','--continue-after-fail','--timeout','120000','--threads','1',
                             '--warmup-count',str(args.warmups),'--loop-count',str(args.repeats),
                             '--trace-file',str(trace),'--trace-dataset',NAMES[name]+' · '+mode,'--trace-level','15',
                             '--log-file',str(dest/'server.log'),'--result-file','/dev/null',
                             '--script-ast-file','/dev/null','--script-plan-file','/dev/null']
                    print(f'Collect {name} {offset+1}-{offset+len(batch)} {mode}, warmups={args.warmups}',flush=True)
                    start=time.time()
                    with (dest/'run.log').open('w') as log:
                        process=subprocess.Popen(command,cwd=root,stdout=log,stderr=subprocess.STDOUT)
                        pid=process.pid;returncode=process.wait()
                    log=(dest/'run.log').read_text()
                    if returncode and (returncode!=1 or 'query requests failed' not in log):
                        raise RuntimeError(f'Collection stopped ({returncode}); see {dest}/run.log')
                    assert log.count('(warmup, tracing disabled)')==args.warmups*len(batch),dest
                    result=dict(signature=signature,variant=mode,workload=name,queries=len(batch),command=command,pid=pid,
                                trace=str(trace),trace_sha256=digest(trace),returncode=returncode,seconds=round(time.time()-start,2),
                                measurements=summarize(trace,len(batch)*args.repeats,len(batch)*args.warmups,mode in native_modes))
                    write(result_file,json.dumps(result,indent=2)+'\n')
                    print(f'Finished {name} {mode}: {result["seconds"]}s, {sum(q["success"] for q in result["measurements"])}/{len(result["measurements"])} successful',flush=True)
                manifest['batches'].append(result)
                write(out/'manifest.json',json.dumps(manifest,indent=2)+'\n')
    summarize_all(out,manifest['batches'])
    if not args.no_render: render(out,manifest['batches'])
    failed=[q for b in manifest['batches'] if b['variant'] in native_modes for q in b['measurements'] if not q['success'] or not q['native'] or q['fallback']]
    failed_batches=[b for b in manifest['batches'] if b['variant'] in native_modes and b['returncode']]
    if failed or failed_batches:
        raise SystemExit(f'{len(failed)} measured native compilation failures; {len(failed_batches)} batches had a measured or warmup failure; see measurements.json and batch run logs')

if __name__=='__main__': main()
