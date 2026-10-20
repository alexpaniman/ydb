import contextlib
import io
import json
import os
import shutil
import sqlite3
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from ydb.core.kqp.tracing.tools.compile_trace import compile_trace


class TestCompileTrace(unittest.TestCase):
    TRACE_ID = 'a' * 32
    ROOT_ID = '1' * 16
    PHASE_ID = '2' * 16
    STEP_ID = '3' * 16

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.db = str(self.directory / 'traces.db')

    def span(self, span_id, parent_id, name, start, duration, attrs=None):
        return {
            'kind': 'span', 'trace_id': self.TRACE_ID, 'span_id': span_id,
            'parent_span_id': parent_id, 'name': name,
            'start_ns': start * 1000, 'end_ns': (start + duration) * 1000,
            'attrs': attrs or {}, 'status': 1,
        }

    def records(self):
        run = {
            'kind': 'run', 'trace_id': self.TRACE_ID, 'bench': 'tpch',
            'query': 1, 'level': 15, 'repeat': 0, 'client_us': 100, 'ok': True,
        }
        baseline = dict(run, trace_id=None, level=0)
        return [
            run, baseline, baseline,
            self.span(self.ROOT_ID, None, 'Compile query', 1_000_000, 100),
            self.span(self.PHASE_ID, self.ROOT_ID, 'Phase', 1_000_005, 90),
            self.span(self.STEP_ID, self.PHASE_ID, 'Step', 1_000_010, 80),
        ]

    def write_jsonl(self, records, name='spans.jsonl'):
        path = self.directory / name
        path.write_text(''.join(json.dumps(record) + '\n' for record in records))
        return str(path)

    def load(self, *paths):
        with contextlib.redirect_stderr(io.StringIO()):
            compile_trace.cmd_load(SimpleNamespace(inputs=paths, output=self.db))

    def query(self, sql):
        with contextlib.closing(sqlite3.connect(self.db)) as con:
            return con.execute(sql).fetchall()

    def assert_tree(self):
        self.assertEqual(self.query('SELECT name, self_us FROM tree ORDER BY depth'), [
            ('Compile query', 10), ('Phase', 10), ('Step', 80),
        ])
        with contextlib.closing(sqlite3.connect(self.db)) as con:
            stacks = compile_trace.folded_stacks(con, 15, None, 'wall', False, False)
        self.assertEqual(sum(stacks.values()), 100)

    def test_reload_and_append_preserve_span_identity_and_baseline_runs(self):
        records = self.records()
        path = self.write_jsonl(records)
        self.load(path)
        self.assert_tree()
        self.load(path, str(self.directory) + '/./spans.jsonl')
        self.assert_tree()
        # Equal untraced measurements are still distinct runs, but reloading
        # the file must not count them twice.
        self.assertEqual(self.query('SELECT level, COUNT(*) FROM runs GROUP BY level'), [(0, 2), (15, 1)])

        records.append(dict(records[1], repeat=1, client_us=120))
        self.write_jsonl(records)
        self.load(path)
        self.assert_tree()
        self.assertEqual(self.query('SELECT level, COUNT(*) FROM runs GROUP BY level'), [(0, 3), (15, 1)])

    def test_jaeger_and_jsonl_exports_of_same_trace_do_not_duplicate_spans(self):
        records = self.records()
        jsonl = self.write_jsonl(records)
        jaeger = self.directory / 'jaeger.json'
        spans = []
        for record in records:
            if record['kind'] != 'span':
                continue
            spans.append({
                'traceID': record['trace_id'].upper(),
                'spanID': record['span_id'],
                'references': [{'refType': 'CHILD_OF', 'spanID': record['parent_span_id']}]
                if record['parent_span_id'] else [],
                'operationName': record['name'],
                'startTime': record['start_ns'] // 1000,
                'duration': (record['end_ns'] - record['start_ns']) // 1000,
                'tags': [],
            })
        jaeger.write_text(json.dumps({'data': [{'spans': spans}]}))
        self.load(jsonl, str(jaeger), jsonl, str(jaeger))
        self.assert_tree()
        self.assertEqual(self.query('SELECT COUNT(*) FROM spans'), [(3,)])

    def test_existing_database_with_duplicates_is_repaired(self):
        path = self.write_jsonl(self.records())
        self.load(path)
        with contextlib.closing(sqlite3.connect(self.db)) as con:
            con.executescript('''
                DROP INDEX spans_identity;
                DROP INDEX runs_trace;
                INSERT INTO spans SELECT * FROM spans;
                INSERT INTO runs SELECT * FROM runs;
            ''')
        self.load(path)
        self.assert_tree()
        self.assertEqual(self.query('SELECT COUNT(*) FROM spans'), [(3,)])
        self.assertEqual(self.query('SELECT COUNT(*) FROM runs'), [(3,)])

    def test_reload_replaces_run_records_with_legacy_relative_source_paths(self):
        path = self.write_jsonl(self.records())
        relative_path = os.path.relpath(path)
        self.load(path)
        with contextlib.closing(sqlite3.connect(self.db)) as con:
            con.execute('UPDATE runs SET source = ?', (relative_path,))
            con.commit()
        self.load(relative_path)
        self.assertEqual(self.query('SELECT COUNT(*) FROM runs'), [(3,)])
        self.assert_tree()

    def measurement(self, trace_id, bench='tpch', query=1, duration=100_000, phase=True):
        records = [
            {'kind': 'run', 'trace_id': trace_id, 'bench': bench, 'query': query,
             'level': 15, 'repeat': 0, 'client_us': duration, 'ok': True},
            self.span(self.ROOT_ID, None, 'Compile query', 1_000_000, duration, {'ydb.cpu_us': duration}),
        ]
        if phase:
            records.append(self.span(self.PHASE_ID, self.ROOT_ID, 'Phase', 1_000_000, duration * 4 // 5, {
                'ydb.cpu_us': duration * 4 // 5, 'ydb.rbo.rule': 'Test rule', 'ydb.compile.iteration': 2,
            }))
        return [dict(record, trace_id=trace_id) for record in records]

    def report(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            compile_trace.cmd_report(SimpleNamespace(db=self.db, level=15, bench=None, top=25))
        return output.getvalue()

    def diff(self, before, metric='wall'):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            compile_trace.cmd_diff(SimpleNamespace(
                before=str(before), after=self.db, level=15, bench=None, metric=metric, top=25,
            ))
        return output.getvalue()

    @staticmethod
    def table_rows(output, section):
        section = output.split(section, 1)[1].split('\n## ', 1)[0]
        rows = [[cell.strip() for cell in line.split('|')[1:-1]]
                for line in section.splitlines() if line.startswith('| ')][1:]
        return {row[0]: row[1:] for row in rows}

    def test_appended_invocations_do_not_inflate_report_or_diff(self):
        records = self.measurement(self.TRACE_ID)
        path = self.write_jsonl(records)
        self.load(path)
        before = self.directory / 'before.db'
        shutil.copyfile(self.db, before)
        report = self.report()

        # Both invocations use repeat=0, but their trace IDs identify distinct measurements.
        records.extend(self.measurement('b' * 32))
        self.write_jsonl(records)
        self.load(path)
        after = self.report()
        self.assertIn('1 queries, 2 runs', after)
        self.assertEqual(report.split('## Own time')[1], after.split('## Own time')[1])
        for metric in ('wall', 'cpu'):
            diff = self.diff(before, metric)
            self.assertIn('100 ms -> 100 ms (+0.0%), 1 vs 2 runs', diff)
            for row in self.table_rows(diff, '# Diff:').values():
                self.assertEqual(float(row[2]), 0)

    def test_queries_are_averaged_independently_with_unequal_run_counts(self):
        self.load(self.write_jsonl([
            *self.measurement('a' * 32, duration=100_000),
            *self.measurement('b' * 32, duration=300_000),
            *self.measurement('c' * 32, query=2, duration=1_000_000),
        ]))
        report = self.report()
        self.assertIn('2 queries, 3 runs', report)
        rows = self.table_rows(report, '## Own time')
        self.assertEqual(float(rows['Compile query'][3]), 1200)
        self.assertEqual(float(rows['Phase'][0]), 2)
        self.assertEqual(float(rows['Phase'][1]), 960)
        rules = self.table_rows(report, '## Applied RBO rules')
        self.assertEqual(float(rules['Test rule'][0]), 2)
        self.assertEqual(float(rules['Test rule'][1]), 960)
        restarts = self.table_rows(report, '## Repeated runs')
        self.assertEqual(float(restarts['Phase'][0]), 2)
        self.assertEqual(float(restarts['Phase'][1]), 960)

    def test_benchmarks_keep_distinct_query_names_in_reports_and_profiles(self):
        self.load(self.write_jsonl([
            *self.measurement('a' * 32, bench='tpch', duration=100_000),
            *self.measurement('b' * 32, bench='tpcds', duration=1_000_000),
        ]))
        rows = self.table_rows(self.report(), '## Slowest queries')
        self.assertEqual(rows, {'tpcds Q1': ['1000.0', '90.9'], 'tpch Q1': ['100.0', '9.1']})
        with contextlib.closing(sqlite3.connect(self.db)) as con:
            stacks = compile_trace.folded_stacks(con, 15, None, 'wall', True, False)
        self.assertEqual(sum(value for name, value in stacks.items() if name.startswith('tpch Q1;')), 100_000)
        self.assertEqual(sum(value for name, value in stacks.items() if name.startswith('tpcds Q1;')), 1_000_000)

    def test_multiple_compilations_in_one_trace_count_as_one_measurement(self):
        extra = dict(self.span('9' * 16, None, 'Compile query', 2_000_000, 50_000), trace_id='a' * 32)
        self.load(self.write_jsonl([
            *self.measurement('a' * 32, duration=100_000, phase=False), extra,
            *self.measurement('b' * 32, duration=150_000, phase=False),
        ]))
        report = self.report()
        self.assertIn('1 queries, 2 runs', report)
        self.assertEqual(self.table_rows(report, '## Slowest queries')['tpch Q1'][0], '150.0')
        self.assertEqual(float(self.table_rows(report, '## Own time')['Compile query'][3]), 150)
        before = self.directory / 'before.db'
        shutil.copyfile(self.db, before)
        self.assertIn('150 ms -> 150 ms (+0.0%), 2 vs 2 runs', self.diff(before))

    def test_diff_counts_missing_spans_as_zero_in_each_measurement(self):
        records = self.measurement('a' * 32)
        path = self.write_jsonl(records)
        self.load(path)
        before = self.directory / 'before.db'
        shutil.copyfile(self.db, before)
        records.extend(self.measurement('b' * 32, phase=False))
        records.extend(self.measurement('c' * 32, phase=False))
        self.write_jsonl(records)
        self.load(path)
        rows = self.table_rows(self.diff(before), '# Diff:')
        self.assertEqual(rows['Phase'], ['80.0', '0.0', '-80.0', '-100%'])

    def filter_samples(self, spans, samples):
        self.load(self.write_jsonl([
            self.span(self.ROOT_ID, None, 'Compile query', 1_000_000, 100_000),
            *spans,
        ]))
        perf = self.directory / 'perf.txt'
        perf.write_text(''.join(
            f'worker {tid} {timestamp}:\n    123 {symbol}\n\n'
            for tid, timestamp, symbol in samples
        ))
        output = self.directory / 'profile.folded'
        with contextlib.redirect_stderr(io.StringIO()):
            compile_trace.cmd_perf_filter(SimpleNamespace(
                db=self.db, level=None, bench=None, name='Peephole stage',
                perf_script=str(perf), output=str(output),
            ))
        return output.read_text().splitlines()

    def test_profile_excludes_unrelated_threads(self):
        result = self.filter_samples([
            self.span(self.PHASE_ID, self.ROOT_ID, 'Peephole stage', 1_020_000, 20_000, {'thread.id': 111}),
        ], [
            (111, '1.030000', 'intended_work'),
            (999, '1.030000', 'unrelated_background_work'),
            (111, '1.050000', 'outside_span'),
        ])
        self.assertEqual(result, ['intended_work 1'])

    def test_spans_without_thread_id_match_known_and_unknown_threads(self):
        result = self.filter_samples([
            self.span(self.PHASE_ID, self.ROOT_ID, 'Peephole stage', 1_010_000, 10_000, {'thread.id': 111}),
            self.span(self.STEP_ID, self.ROOT_ID, 'Peephole stage', 1_040_000, 10_000),
        ], [
            (111, '1.015000', 'thread_bound'),
            (111, '1.045000', 'known_thread_unbound_span'),
            (999, '1.045000', 'unknown_thread_unbound_span'),
            (999, '1.015000', 'wrong_thread'),
        ])
        self.assertEqual(result, [
            'thread_bound 1', 'known_thread_unbound_span 1', 'unknown_thread_unbound_span 1',
        ])

    def test_nested_intervals_do_not_hide_the_outer_interval(self):
        result = self.filter_samples([
            self.span(self.PHASE_ID, self.ROOT_ID, 'Peephole stage', 1_010_000, 60_000, {'thread.id': 111}),
            self.span(self.STEP_ID, self.PHASE_ID, 'Peephole stage', 1_020_000, 10_000, {'thread.id': 111}),
        ], [
            (111, '1.025000', 'inner'),
            (111, '1.060000', 'outer_after_inner'),
        ])
        self.assertEqual(result, ['inner 1', 'outer_after_inner 1'])
