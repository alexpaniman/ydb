import contextlib
import io
import json
import os
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
