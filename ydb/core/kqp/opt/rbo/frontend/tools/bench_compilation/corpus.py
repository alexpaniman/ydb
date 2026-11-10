"""Load the unchanged benchmark queries and their required schemas."""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAMES = {'1c': '1C', 'tpch': 'TPC-H', 'tpcds': 'TPC-DS', 'medium': '1C medium', 'tiny': '1C tiny', 'clickbench': 'ClickBench'}
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
            for i in range(1, ({'tpch':22,'tpcds':99,'clickbench':43}[name]) + 1)
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
