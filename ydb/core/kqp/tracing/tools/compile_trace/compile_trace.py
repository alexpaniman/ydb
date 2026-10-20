#!/usr/bin/env python3
"""Analysis of query compilation spans.

Loads spans written by the CompileTraceBench test (JSON lines) or exported
from Jaeger (JSON of /api/traces/<id>) into SQLite, checks their health and
reports where compilation time goes. See README.md next to this file.
"""

import argparse
import bisect
import json
import math
import re
import sqlite3
import statistics
import sys
from collections import Counter, defaultdict

COMPILE_ROOT = 'Compile query'
STATUS_ERROR = 2

SCHEMA = '''
CREATE TABLE IF NOT EXISTS runs (
    trace_id TEXT, bench TEXT, query INTEGER, level INTEGER, repeat INTEGER,
    client_us INTEGER, ok INTEGER, load1 REAL, source TEXT);
CREATE TABLE IF NOT EXISTS spans (
    trace_id TEXT, span_id TEXT, parent_id TEXT, name TEXT,
    start_us INTEGER, dur_us INTEGER, status INTEGER, attrs TEXT, source TEXT);
'''


# Loading

def hex_id(value):
    return value.lower() if value else None


def load_jsonl(path, con):
    runs, spans = [], []
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            rec = json.loads(line)
            if rec.get('kind') == 'run':
                runs.append((rec.get('trace_id'), rec['bench'], rec['query'], rec['level'], rec['repeat'],
                             rec['client_us'], int(rec['ok']), rec.get('load1'), path))
            elif rec.get('kind') == 'span':
                parent = rec.get('parent_span_id') or None
                spans.append((hex_id(rec['trace_id']), hex_id(rec['span_id']), hex_id(parent), rec['name'],
                              rec['start_ns'] // 1000, (rec['end_ns'] - rec['start_ns']) // 1000,
                              rec.get('status', 0), json.dumps(rec.get('attrs', {})), path))
    con.executemany('INSERT INTO runs VALUES (?,?,?,?,?,?,?,?,?)', runs)
    con.executemany('INSERT INTO spans VALUES (?,?,?,?,?,?,?,?,?)', spans)
    return len(runs), len(spans)


def load_jaeger(path, con):
    with open(path) as f:
        data = json.load(f)
    spans = []
    for trace in data['data']:
        for s in trace['spans']:
            parent = next((r['spanID'] for r in s['references'] if r['refType'] == 'CHILD_OF'), None)
            attrs = {t['key']: t['value'] for t in s['tags']}
            status = STATUS_ERROR if attrs.get('otel.status_code') == 'ERROR' or attrs.get('error') is True else 0
            spans.append((hex_id(s['traceID']).rjust(32, '0'), hex_id(s['spanID']).rjust(16, '0'),
                          hex_id(parent).rjust(16, '0') if parent else None, s['operationName'],
                          s['startTime'], s['duration'], status, json.dumps(attrs), path))
    con.executemany('INSERT INTO spans VALUES (?,?,?,?,?,?,?,?,?)', spans)
    return 0, len(spans)


def derive(con):
    """Builds the analysis tables: one row per span with its run, ancestry and own time."""
    con.executescript('''
        DROP TABLE IF EXISTS tree;
        DROP TABLE IF EXISTS trace_runs;
        CREATE INDEX IF NOT EXISTS spans_id ON spans(trace_id, span_id);
        CREATE INDEX IF NOT EXISTS spans_parent ON spans(trace_id, parent_id);
    ''')
    spans = con.execute('SELECT trace_id, span_id, parent_id, name, start_us, dur_us, status, attrs FROM spans').fetchall()
    by_id = {(t, s): (t, s, p, n, st, d, stat, a) for t, s, p, n, st, d, stat, a in spans}
    children = defaultdict(list)
    for t, s, p, *_ in spans:
        if p and (t, p) in by_id:
            children[(t, p)].append(s)

    # A run per trace: from the benchmark run records, else from the root span.
    runs = {r[0]: r[1:] for r in con.execute('SELECT trace_id, bench, query, level, repeat FROM runs WHERE trace_id IS NOT NULL')}
    trace_runs = {}
    for t, s, p, n, st, d, stat, a in spans:
        if p and (t, p) in by_id:
            continue
        if t in runs:
            bench, query, level, repeat = runs[t]
        else:
            attrs = json.loads(a)
            m = re.search(r'Q(\d+)', n)
            bench = attrs.get('ydb.bench') or ('tpcds' if 'DS' in n else 'tpch' if 'TPC-H' in n else '')
            query = attrs.get('ydb.bench.query') or (int(m.group(1)) if m else None)
            m = re.search(r'\[L(\d+)\]', n)
            level = attrs.get('ydb.trace.level') or (int(m.group(1)) if m else None)
            repeat = attrs.get('ydb.bench.repeat', 0)
        trace_runs[t] = (t, bench, query, level, repeat)
    con.execute('CREATE TABLE trace_runs (trace_id TEXT PRIMARY KEY, bench TEXT, query INTEGER, level INTEGER, repeat INTEGER)')
    con.executemany('INSERT OR REPLACE INTO trace_runs VALUES (?,?,?,?,?)', trace_runs.values())

    rows = []
    for t, s, p, n, st, d, stat, a in spans:
        if p and (t, p) in by_id:
            continue
        stack = [(s, 0, '', None)]
        while stack:
            sid, depth, ancestors, parent_name = stack.pop()
            _, _, pid, name, start, dur, status, attrs = by_id[(t, sid)]
            kids = children[(t, sid)]
            kid_union = interval_union([(by_id[(t, k)][4], by_id[(t, k)][4] + by_id[(t, k)][5]) for k in kids],
                                       start, start + dur)
            attr = json.loads(attrs)
            self_us = max(dur - kid_union, 0)
            outlives = max((by_id[(t, k)][4] + by_id[(t, k)][5]) - (start + dur) for k in kids) if kids else 0
            rows.append((t, sid, pid, name, start, dur, self_us, attr.get('ydb.cpu_us'), status, depth,
                         parent_name, ancestors, attrs, max(outlives, 0)))
            for k in kids:
                stack.append((k, depth + 1, ancestors + ' > ' + name if ancestors else name, name))
    con.execute('''CREATE TABLE tree (trace_id TEXT, span_id TEXT, parent_id TEXT, name TEXT, start_us INTEGER,
        dur_us INTEGER, self_us INTEGER, cpu_us INTEGER, status INTEGER, depth INTEGER, parent TEXT, ancestors TEXT,
        attrs TEXT, child_overrun_us INTEGER)''')
    con.executemany('INSERT INTO tree VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)', rows)
    con.executescript('''
        CREATE INDEX tree_name ON tree(name);
        CREATE INDEX tree_trace ON tree(trace_id);
        DROP VIEW IF EXISTS spans_view;
        CREATE VIEW spans_view AS
            SELECT r.bench, r.query, r.level, r.repeat, t.* FROM tree t JOIN trace_runs r USING (trace_id);
    ''')
    con.commit()


def interval_union(intervals, lo=None, hi=None):
    """Total length covered by the intervals, clipped to [lo, hi]."""
    total, end = 0, None
    for a, b in sorted(intervals):
        if lo is not None:
            a, b = max(a, lo), min(b, hi)
        if b <= a:
            continue
        if end is None or a > end:
            total += b - a
            end = b
        elif b > end:
            total += b - end
            end = b
    return total


def cmd_load(args):
    con = sqlite3.connect(args.output)
    con.executescript(SCHEMA)
    for path in args.inputs:
        with open(path) as f:
            first = f.readline()
        runs, spans = (load_jsonl if '"kind"' in first else load_jaeger)(path, con)
        print(f'{path}: {runs} runs, {spans} spans', file=sys.stderr)
    derive(con)
    n = con.execute('SELECT count(*) FROM tree').fetchone()[0]
    print(f'{args.output}: {n} spans in {con.execute("SELECT count(*) FROM trace_runs").fetchone()[0]} traces', file=sys.stderr)


# Reporting

def pct(a, b):
    return 100.0 * a / b if b else 0.0


def ms(us):
    return us / 1000.0


def table(headers, rows, out):
    rows = [[f'{v:.1f}' if isinstance(v, float) else '' if v is None else str(v) for v in r] for r in rows]
    widths = [max([len(h)] + [len(r[i]) for r in rows]) for i, h in enumerate(headers)]
    out.write('| ' + ' | '.join(h.ljust(w) for h, w in zip(headers, widths)) + ' |\n')
    out.write('|' + '|'.join('-' * (w + 2) for w in widths) + '|\n')
    for r in rows:
        out.write('| ' + ' | '.join(v.ljust(w) for v, w in zip(r, widths)) + ' |\n')
    out.write('\n')


def pick_level(con, level):
    levels = [r[0] for r in con.execute('SELECT DISTINCT level FROM trace_runs WHERE level IS NOT NULL ORDER BY level')]
    if level is not None:
        return level
    return levels[-1] if levels else None


def compile_spans(con, level, bench=None):
    """Compile query spans of the level: one per compilation."""
    sql = 'SELECT * FROM spans_view WHERE name = ? AND (? IS NULL OR level = ?) AND (? IS NULL OR bench = ?)'
    cur = con.execute(sql, (COMPILE_ROOT, level, level, bench, bench))
    cols = [d[0] for d in cur.description]
    return [dict(zip(cols, r)) for r in cur]


def subtree(con, trace_id, span_id):
    rows = con.execute('SELECT span_id, parent_id, name, start_us, dur_us, self_us, cpu_us, status, attrs, parent, '
                       'child_overrun_us FROM tree WHERE trace_id = ?', (trace_id,)).fetchall()
    kids = defaultdict(list)
    info = {}
    for r in rows:
        info[r[0]] = r
        kids[r[1]].append(r[0])
    result, stack = [], [span_id]
    while stack:
        s = stack.pop()
        result.append(info[s])
        stack.extend(kids[s])
    return result


def cmd_report(args):
    con = sqlite3.connect(args.db)
    out = sys.stdout
    level = pick_level(con, args.level)
    roots = compile_spans(con, level, args.bench)
    if not roots:
        sys.exit(f'No "{COMPILE_ROOT}" spans at level {level}')
    repeats = sorted({r['repeat'] for r in roots})
    queries = sorted({(r['bench'], r['query']) for r in roots}, key=lambda x: (x[0] or '', x[1] or 0))
    out.write(f'# Compilation spans: level {level}, {len(queries)} queries x {len(repeats)} repeats\n\n')

    # Per compilation: its spans, used by the checks and the breakdowns.
    compilations = []
    for r in roots:
        spans = subtree(con, r['trace_id'], r['span_id'])
        compilations.append((r, spans))

    health(con, compilations, level, out)
    breakdowns(compilations, len(repeats), args.top, out)


def health(con, compilations, level, out):
    out.write('## Health checks\n\n')
    rows = []

    # Coverage: how much of each compilation its direct children cover.
    coverage, total = [], 0
    for root, spans in compilations:
        kids = [(s[3], s[3] + s[4]) for s in spans if s[1] == root['span_id']]
        covered = interval_union(kids, root['start_us'], root['start_us'] + root['dur_us'])
        coverage.append(pct(covered, root['dur_us']))
        total += root['dur_us']
    rows.append(('Coverage of "Compile query" by child spans (min / median)',
                 f'{min(coverage):.1f}% / {statistics.median(coverage):.1f}%', 'below ~98% means untraced work'))

    # Own time of container spans: time inside a span that no child names.
    container_self = defaultdict(int)
    for root, spans in compilations:
        parents = {s[1] for s in spans}
        for s in spans:
            if s[0] in parents:
                container_self[s[2]] += s[5]
    unnamed = sum(container_self.values())
    worst = ', '.join(f'{n} {pct(v, total):.1f}%' for n, v in sorted(container_self.items(), key=lambda x: -x[1])[:4])
    rows.append(('Unattributed own time of container spans', f'{pct(unnamed, total):.1f}%', worst))

    # Children that end after their parent.
    overruns = [(s[2], s[10]) for _, spans in compilations for s in spans if s[10] > 0]
    if overruns:
        names = Counter(n for n, _ in overruns)
        rows.append(('Spans whose children end after them', f'{len(overruns)} (max {max(v for _, v in overruns)} us)',
                     ', '.join(f'{n} x{c}' for n, c in names.most_common(3))))
    else:
        rows.append(('Spans whose children end after them', '0', ''))

    # Overlapping siblings: concurrent work, double counted by naive sums.
    overlap_us, overlap_names = 0, Counter()
    for _, spans in compilations:
        by_parent = defaultdict(list)
        for s in spans:
            by_parent[s[1]].append(s)
        for sibs in by_parent.values():
            if len(sibs) < 2:
                continue
            ivs = [(s[3], s[3] + s[4]) for s in sibs]
            o = sum(b - a for a, b in ivs) - interval_union(ivs)
            if o > 0:
                overlap_us += o
                sibs = sorted(sibs, key=lambda s: s[3])
                for x, y in zip(sibs, sibs[1:]):
                    if y[3] < x[3] + x[4]:
                        overlap_names[y[2]] += 1
    rows.append(('Overlap of concurrent sibling spans', f'{pct(overlap_us, total):.1f}% of compile time',
                 ', '.join(f'{n} x{c}' for n, c in overlap_names.most_common(3))))

    # Errors.
    errors = Counter(s[2] for _, spans in compilations for s in spans if s[7] == STATUS_ERROR)
    rows.append(('Spans in error', str(sum(errors.values())), ', '.join(f'{n} x{c}' for n, c in errors.most_common(3))))

    # CPU accounting.
    cpu = [(r['cpu_us'], r['dur_us']) for r, _ in compilations if r['cpu_us'] is not None]
    if cpu:
        rows.append(('CPU time of compilation (ydb.cpu_us) vs wall', f'{pct(sum(c for c, _ in cpu), sum(d for _, d in cpu)):.1f}%',
                     'the rest is waiting: metadata, statistics, scheduling'))

    # Outliers among queries.
    per_query = defaultdict(list)
    for r, _ in compilations:
        per_query[(r['bench'], r['query'])].append(r['dur_us'])
    med = {q: statistics.median(v) for q, v in per_query.items()}
    overall = statistics.median(med.values())
    outliers = sorted(((q, v) for q, v in med.items() if v > 5 * overall), key=lambda x: -x[1])
    share = pct(sum(v for _, v in outliers), sum(med.values()))
    rows.append(('Outlier queries (> 5x median compile time)',
                 f'{len(outliers)}, {share:.1f}% of total' if outliers else '0',
                 ', '.join(f'Q{q[1]} {ms(v):.0f} ms' for q, v in outliers[:4]) + f' (median {ms(overall):.0f} ms)'))

    # Noise between repeats.
    cvs = [statistics.stdev(v) / statistics.mean(v) * 100 for v in per_query.values() if len(v) > 1 and statistics.mean(v)]
    if cvs:
        rows.append(('Run-to-run variation of compile time (median CV over queries)', f'{statistics.median(cvs):.1f}%',
                     f'max {max(cvs):.1f}%; differences below ~2x this are noise'))
    loads = [r[0] for r in con.execute('SELECT load1 FROM runs WHERE load1 >= 0')]
    if loads:
        rows.append(('Host load average during runs (min / max)', f'{min(loads):.1f} / {max(loads):.1f}', ''))

    # Overhead of tracing: client compile time per level relative to untraced runs.
    overhead = level_overhead(con)
    if overhead:
        rows.append(('Tracing overhead, client time vs level 0 (geometric mean)',
                     ', '.join(f'L{lvl} {v:+.1f}%' for lvl, v in overhead), ''))

    table(['Check', 'Value', 'Notes'], rows, out)


def level_overhead(con):
    rows = con.execute('SELECT bench, query, level, client_us FROM runs WHERE ok = 1').fetchall()
    by = defaultdict(list)
    for bench, query, level, client in rows:
        by[(bench, query, level)].append(client)
    base = {(b, q): statistics.median(v) for (b, q, l), v in by.items() if l == 0}
    if not base:
        return []
    result = []
    for level in sorted({l for _, _, l in by if l}):
        ratios = [math.log(statistics.median(v) / base[(b, q)]) for (b, q, l), v in by.items()
                  if l == level and (b, q) in base and base[(b, q)]]
        if ratios:
            result.append((level, (math.exp(statistics.mean(ratios)) - 1) * 100))
    return result


def breakdowns(compilations, repeats, top, out):
    total = sum(r['dur_us'] for r, _ in compilations)
    cpu_total = sum(r['cpu_us'] or 0 for r, _ in compilations)
    by_name = defaultdict(lambda: [0, 0, 0, 0])
    callers = defaultdict(Counter)
    for root, spans in compilations:
        for s in spans:
            entry = by_name[s[2]]
            entry[0] += 1
            entry[1] += s[4]
            entry[2] += s[5]
            entry[3] += s[6] or 0
            callers[s[2]][s[9] or '-'] += s[4]

    out.write(f'## Own time by span (wall {ms(total) / repeats:.0f} ms per repeat'
              + (f', CPU {pct(cpu_total, total):.0f}% of it' if cpu_total else '') + ')\n\n')
    out.write('Own time is the span duration minus the time its children cover. CPU is the compilation CPU time '
              'while the span was open (children included).\n\n')
    rows = []
    for name, (n, dur, own, cpu) in sorted(by_name.items(), key=lambda x: -x[1][2])[:top]:
        rows.append((name, n // repeats, ms(own) / repeats, pct(own, total), ms(dur) / repeats,
                     f'{pct(cpu, dur):.0f}%' if cpu else ''))
    table(['Span', 'Calls', 'Own ms', 'Own %', 'Total ms', 'CPU/wall'], rows, out)

    out.write('## Callers of spans with several parents\n\n')
    rows = []
    multi = [(n, c) for n, c in callers.items() if len(c) > 1]
    for name, c in sorted(multi, key=lambda x: -sum(x[1].values()))[:top // 2]:
        t = sum(c.values())
        rows.append((name, ', '.join(f'{p} {pct(v, t):.0f}%' for p, v in c.most_common(4))))
    table(['Span', 'Share of its time by parent'], rows, out)

    family(compilations, repeats, total, 'ydb.rbo.property', 'Plan properties', out)
    family(compilations, repeats, total, 'ydb.rbo.rule', 'Applied RBO rules', out, top)

    # Per query: which queries dominate, and the median query's breakdown.
    per_query = defaultdict(list)
    for r, _ in compilations:
        per_query[r['query']].append(r['dur_us'])
    med = sorted(((q, statistics.median(v)) for q, v in per_query.items()), key=lambda x: -x[1])
    out.write('## Slowest queries\n\n')
    qtotal = sum(v for _, v in med)
    table(['Query', 'Median compile ms', '% of all'], [(f'Q{q}', ms(v), pct(v, qtotal)) for q, v in med[:10]], out)

    sizes(compilations, out)
    restarts(compilations, repeats, total, out)


def family(compilations, repeats, total, key, title, out, top=None):
    stats = defaultdict(lambda: [0, 0, 0])
    for _, spans in compilations:
        for s in spans:
            attrs = json.loads(s[8])
            if key in attrs:
                e = stats[attrs[key]]
                e[0] += 1
                e[1] += s[4]
                e[2] += s[6] or 0
    if not stats:
        return
    out.write(f'## {title}\n\n')
    rows = [(n, c // repeats, ms(d) / repeats, pct(d, total), d / c if c else 0)
            for n, (c, d, _) in sorted(stats.items(), key=lambda x: -x[1][1])[:top]]
    allc = sum(v[0] for v in stats.values())
    alld = sum(v[1] for v in stats.values())
    rows.append(('All', allc // repeats, ms(alld) / repeats, pct(alld, total), alld / allc if allc else 0))
    table(['Name', 'Calls', 'Total ms', '% of compile', 'us per call'], rows, out)


def sizes(compilations, out):
    """Time against input size for spans that record it: fixed cost per call or cost per node."""
    points = defaultdict(list)
    for _, spans in compilations:
        for s in spans:
            attrs = json.loads(s[8])
            size = attrs.get('ydb.expr.nodes') or attrs.get('ydb.cbo.relations')
            if size:
                points[s[2]].append((size, s[4]))
    rows = []
    for name, pts in sorted(points.items(), key=lambda x: -sum(d for _, d in x[1])):
        if len(pts) < 5:
            continue
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sxx = sum((x - mx) ** 2 for x in xs)
        syy = sum((y - my) ** 2 for y in ys)
        sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        slope = sxy / sxx if sxx else 0
        corr = sxy / math.sqrt(sxx * syy) if sxx and syy else 0
        unit = 'relation' if 'CBO' in name else 'node'
        rows.append((name, len(pts), f'{statistics.median(xs):.0f}', f'{my - slope * mx:.0f}', f'{slope:.2f} per {unit}',
                     f'{corr:.2f}'))
    if rows:
        out.write('## Time against input size\n\n')
        out.write('Linear fit of span duration (us) on its input size. A high correlation with a small fixed cost '
                  'means the cost grows with the input; a large fixed cost means per-call overhead.\n\n')
        table(['Span', 'Samples', 'Median size', 'Fixed us', 'Slope us', 'Correlation'], rows, out)


def restarts(compilations, repeats, total, out):
    repeated = defaultdict(lambda: [0, 0])
    for _, spans in compilations:
        for s in spans:
            it = json.loads(s[8]).get('ydb.compile.iteration')
            if it and it > 1:
                repeated[s[2]][0] += 1
                repeated[s[2]][1] += s[4]
    if repeated:
        out.write('## Repeated runs after pipeline restarts\n\n')
        rows = [(n, c // repeats, ms(d) / repeats, pct(d, total)) for n, (c, d) in sorted(repeated.items(), key=lambda x: -x[1][1])]
        table(['Span', 'Extra runs', 'Total ms', '% of compile'], rows, out)


# Conversions

def compile_trees(con, level, bench, from_root):
    """Per compilation: the spans to convert, starting at "Compile query" unless from_root."""
    for r in compile_spans(con, level, bench):
        top = r['span_id']
        if from_root:
            top = con.execute('SELECT span_id FROM tree WHERE trace_id = ? AND depth = 0', (r['trace_id'],)).fetchone()[0]
        yield r, subtree(con, r['trace_id'], top)


def cmd_perfetto(args):
    con = sqlite3.connect(args.db)
    level = pick_level(con, args.level)
    events = []
    for pid, (run, spans) in enumerate(compile_trees(con, level, args.bench, args.from_root), start=1):
        info = {s[0]: s for s in spans}
        order = sorted(spans, key=lambda s: (s[3], -s[4]))
        depth = {}
        for s in order:
            depth[s[0]] = depth.get(s[1], -1) + 1 if s[1] in info else 0
        order.sort(key=lambda s: (s[3], -s[4], depth[s[0]]))
        end = {}
        for s in order:
            e = s[3] + s[4]
            p = s[1]
            if p in end and s[3] < end[p] < e:
                e = end[p]  # cut at the parent's end so that it nests; real duration stays in args
            end[s[0]] = e
        tracks, track_of = [], {}

        def fits(t, s, parent):
            st = tracks[t]
            while st and st[-1][0] <= s[3]:
                st.pop()
            if parent is None:
                return not st
            return bool(st) and st[-1][1] == parent and st[-1][0] >= end[s[0]]

        for s in order:
            p = s[1] if s[1] in info else None
            t = track_of.get(p)
            if t is None or not fits(t, s, p):
                t = next((i for i in range(1, len(tracks)) if fits(i, s, None)), None)
                if t is None:
                    tracks.append([])
                    t = len(tracks) - 1
            tracks[t].append((end[s[0]], s[0]))
            track_of[s[0]] = t
            a = json.loads(s[8])
            if p is not None:
                a['parent'] = info[p][2]
            if end[s[0]] != s[3] + s[4]:
                a['real_dur_us'] = s[4]
            a['self_us'] = s[5]
            events.append(dict(ph='X', name=s[2], pid=pid, tid=pid * 1000 + t + 1, ts=s[3], dur=end[s[0]] - s[3], args=a))
        name = f'{run["bench"]} Q{run["query"]} r{run["repeat"]} L{run["level"]}'
        events.append(dict(ph='M', name='process_name', pid=pid, tid=pid * 1000 + 1, args=dict(name=name)))
        events.append(dict(ph='M', name='process_sort_index', pid=pid, tid=pid * 1000 + 1, args=dict(sort_index=pid)))
        for t in range(len(tracks)):
            events.append(dict(ph='M', name='thread_name', pid=pid, tid=pid * 1000 + t + 1,
                               args=dict(name='compile' if t == 0 else f'parallel {t}')))
            events.append(dict(ph='M', name='thread_sort_index', pid=pid, tid=pid * 1000 + t + 1, args=dict(sort_index=t)))
    with open(args.output, 'w') as f:
        json.dump(dict(traceEvents=events, displayTimeUnit='ms'), f)
    print(f'{args.output}: {len(events)} events', file=sys.stderr)


def folded_stacks(con, level, bench, metric, by_query, from_root):
    acc = Counter()
    for run, spans in compile_trees(con, level, bench, from_root):
        info = {s[0]: s for s in spans}
        prefix = [f'Q{run["query"]}'] if by_query else []
        for s in spans:
            path, cur = [], s
            while cur is not None:
                path.append(cur[2].replace(';', ','))
                cur = info.get(cur[1])
            value = s[5] if metric == 'wall' else own_cpu(s, spans)
            if value > 0:
                acc[';'.join(prefix + path[::-1])] += value
    return acc


def own_cpu(span, spans):
    """CPU time of the span minus that of its children, for spans recording it."""
    if span[6] is None:
        return 0
    return max(span[6] - sum(s[6] or 0 for s in spans if s[1] == span[0]), 0)


def cmd_folded(args):
    con = sqlite3.connect(args.db)
    level = pick_level(con, args.level)
    acc = folded_stacks(con, level, args.bench, args.metric, args.by_query, args.from_root)
    with open(args.output, 'w') as f:
        for k, v in acc.items():
            v = round(v / args.unit)
            if v > 0:
                f.write(f'{k} {v}\n')
    print(f'{args.output}: {len(acc)} stacks', file=sys.stderr)


def cmd_diff(args):
    """Compares two databases by span name: median over repeats of the per-repeat total own time."""
    def totals(path):
        con = sqlite3.connect(path)
        level = pick_level(con, args.level)
        per = defaultdict(lambda: defaultdict(int))
        compile_total = defaultdict(int)
        for run, spans in compile_trees(con, level, args.bench, False):
            for s in spans:
                per[s[2]][run['repeat']] += s[5] if args.metric == 'wall' else own_cpu(s, spans)
            compile_total[run['repeat']] += run['dur_us']
        med = {n: statistics.median(list(v.values()) + [0] * (len(compile_total) - len(v))) for n, v in per.items()}
        return med, statistics.median(compile_total.values()), len(compile_total)

    a, at, an = totals(args.before)
    b, bt, bn = totals(args.after)
    out = sys.stdout
    out.write(f'# Diff: compile time {ms(at):.0f} ms -> {ms(bt):.0f} ms ({pct(bt - at, at):+.1f}%), '
              f'{an} vs {bn} repeats\n\n')
    rows = []
    for n in sorted(set(a) | set(b), key=lambda n: -abs(b.get(n, 0) - a.get(n, 0)))[:args.top]:
        x, y = a.get(n, 0), b.get(n, 0)
        rows.append((n, ms(x), ms(y), ms(y - x), f'{pct(y - x, x):+.0f}%' if x else 'new'))
    table(['Span', 'Before ms', 'After ms', 'Delta ms', 'Delta %'], rows, out)


def cmd_perf_filter(args):
    """Keeps the perf samples taken inside spans of the given name, as folded stacks."""
    con = sqlite3.connect(args.db)
    level = pick_level(con, args.level)
    intervals = defaultdict(list)
    for run, spans in compile_trees(con, level, args.bench, False):
        for s in spans:
            if re.fullmatch(args.name, s[2]):
                tid = json.loads(s[8]).get('thread.id')
                intervals[tid].append((s[3], s[3] + s[4]))
    any_thread = sorted(iv for ivs in intervals.values() for iv in ivs)
    for v in intervals.values():
        v.sort()
    starts = {t: [a for a, _ in v] for t, v in intervals.items()}
    any_starts = [a for a, _ in any_thread]

    def inside(tid, ts):
        ivs, st = (intervals[tid], starts[tid]) if tid in intervals else (any_thread, any_starts)
        i = bisect.bisect_right(st, ts) - 1
        return i >= 0 and ivs[i][0] <= ts <= ivs[i][1]

    acc, kept, seen = Counter(), 0, 0
    header = re.compile(r'^\s*\S.*?\s+(\d+)(?:/(\d+))?\s+(?:\[\d+\]\s+)?(\d+\.\d+):')
    with open(args.perf_script) as f:
        sample = None
        for line in f:
            if not line.strip():
                if sample:
                    tid, ts, frames = sample
                    seen += 1
                    if inside(tid, ts) and frames:
                        acc[';'.join(reversed(frames))] += 1
                        kept += 1
                sample = None
                continue
            if sample is None:
                m = header.match(line)
                if m:
                    tid = int(m.group(2) or m.group(1))
                    sample = (tid, int(float(m.group(3)) * 1e6), [])
                continue
            parts = line.strip().split(None, 1)
            if len(parts) == 2:
                sym = re.sub(r'\+0x[0-9a-f]+$', '', parts[1].split(' (')[0])
                sample[2].append(sym.replace(';', ','))
    with open(args.output, 'w') as f:
        for k, v in acc.items():
            f.write(f'{k} {v}\n')
    print(f'{args.output}: kept {kept} of {seen} samples in {sum(len(v) for v in intervals.values())} spans', file=sys.stderr)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)

    s = sub.add_parser('load', help='load benchmark JSON lines or Jaeger exports into SQLite')
    s.add_argument('inputs', nargs='+')
    s.add_argument('-o', '--output', required=True)
    s.set_defaults(func=cmd_load)

    def common(s, top=True):
        s.add_argument('--level', type=int, help='verbosity level to analyse (default: the highest)')
        s.add_argument('--bench', help='benchmark to analyse (default: all)')
        if top:
            s.add_argument('--top', type=int, default=25)

    s = sub.add_parser('report', help='health checks and breakdowns')
    s.add_argument('db')
    common(s)
    s.set_defaults(func=cmd_report)

    s = sub.add_parser('perfetto', help='Chrome JSON trace for ui.perfetto.dev')
    s.add_argument('db')
    s.add_argument('-o', '--output', required=True)
    s.add_argument('--from-root', action='store_true', help='include the spans above "Compile query"')
    common(s, top=False)
    s.set_defaults(func=cmd_perfetto)

    s = sub.add_parser('folded', help='collapsed stacks for flamegraph.pl, speedscope or Firefox Profiler')
    s.add_argument('db')
    s.add_argument('-o', '--output', required=True)
    s.add_argument('--metric', choices=['wall', 'cpu'], default='wall')
    s.add_argument('--by-query', action='store_true', help='one subtree per query')
    s.add_argument('--from-root', action='store_true', help='include the spans above "Compile query"')
    s.add_argument('--unit', type=float, default=1, help='microseconds per count, e.g. 1000 for Firefox Profiler')
    common(s, top=False)
    s.set_defaults(func=cmd_folded)

    s = sub.add_parser('diff', help='compare two databases by span')
    s.add_argument('before')
    s.add_argument('after')
    s.add_argument('--metric', choices=['wall', 'cpu'], default='wall')
    common(s)
    s.set_defaults(func=cmd_diff)

    s = sub.add_parser('perf-filter', help='perf samples taken inside spans of a name, as collapsed stacks')
    s.add_argument('db')
    s.add_argument('--perf-script', required=True, help='output of "perf script" for a "perf record -k CLOCK_REALTIME" run')
    s.add_argument('--name', required=True, help='span name, a regular expression')
    s.add_argument('-o', '--output', required=True)
    common(s, top=False)
    s.set_defaults(func=cmd_perf_filter)

    args = p.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
