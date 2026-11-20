import csv
import io
import json
import subprocess

import pytest
import yatest.common
from google.protobuf.internal.encoder import _VarintBytes as varint
from ydb.public.api.protos import ydb_query_pb2, ydb_value_pb2

TOOL = "ydb/tests/tools/parsed_query_bench/skip_yql/parsed_query_skip_yql_bench"
PROTO = "ydb/tests/tools/parsed_query_bench/proto/logical_plan.proto"

PLAN = """
version: 1
types { id: 1 data { slot: "Int32" } }
functions { id: 1 kind: YQL_CALLABLE name: "==" }
parameters { name: "$min_key" type_id: 1 }
"""

ROOT = """
roots {{
    id: {root_id}
    statement_index: {statement}
    result_index: {result}
    columns {{ iu_id: 1 name: "selected" }}
    graph {{
        input_operator_id: 2
        operators {{ id: 1 read {{ table_path: "/Root/items" columns {{ column: "id" iu_id: 1 }} }} }}
        operators {{ id: 2 filter {{ input: 1 predicate_lambda_expr_id: 6 }} }}
        information_units {{ id: 1 defined_type_id: 1 }}
        expressions {{ id: 1 argument {{ debug_name: "row" }} }}
        expressions {{ id: 2 arguments {{ arguments: 1 }} }}
        expressions {{ id: 3 type_id: 1 column {{ row_argument_expr_id: 1 iu_id: 1 }} }}
        expressions {{ id: 4 type_id: 1 parameter: "$min_key" }}
        expressions {{ id: 5 call {{ function_id: 1 arguments: 3 arguments: 4 }} }}
        expressions {{ id: 6 lambda {{ arguments_expr_id: 2 body: 5 }} }}
    }}
}}
"""
SQL = "DECLARE $min_key AS Int32; SELECT id AS selected FROM items WHERE id = $min_key;"


def message_field(tag, payload):
    return varint(tag << 3 | 2) + varint(len(payload)) + payload


@pytest.fixture
def inputs(tmp_path):
    catalog = tmp_path / "catalog.json"
    catalog.write_text(json.dumps({
        "format_version": 1,
        "cluster": "benchmark",
        "database": "/Root",
        "table_path_prefix": "/Root",
        "tables": [{
            "path": "/Root/items",
            "owner_id": 1,
            "table_id": 1,
            "schema_version": 1,
            "storage": "row",
            "key_columns": ["id"],
            "columns": [{"name": "id", "type": "Int32", "id": 1, "not_null": True}],
        }],
    }))
    request = ydb_query_pb2.ExecuteQueryRequest(
        exec_mode=ydb_query_pb2.EXEC_MODE_EXECUTE,
        session_id="offline",
    )
    supplied = request.parameters["$min_key"]
    supplied.type.type_id = ydb_value_pb2.Type.INT32
    supplied.value.int32_value = 7

    text, logical = tmp_path / "text.pb", tmp_path / "logical_plan.pb"
    protoc = yatest.common.binary_path("contrib/tools/protoc/bin/protoc")
    tool = yatest.common.binary_path(TOOL)

    def prepare(sql=SQL, plan=PLAN + ROOT.format(root_id=1, statement=0, result=0)):
        query = request.query_content
        query.syntax = ydb_query_pb2.SYNTAX_YQL_V1
        query.text = sql
        text.write_bytes(request.SerializeToString())
        request.ClearField("query_content")
        wire = subprocess.check_output(
            [protoc, "-I", yatest.common.source_path(), "--encode=Ydb.Query.LogicalPlan", PROTO],
            input=plan.encode(),
        )
        # Public ExecuteQueryRequest leaves tag 5 unused. ParsedQuery.logical_plan is tag 7.
        logical.write_bytes(request.SerializeToString() + message_field(5, message_field(7, wire)))

        def run(*args):
            return subprocess.run(
                [tool, "--catalog", catalog, *args, text, logical], text=True, capture_output=True,
            )

        return run, (text, logical)

    return prepare


@pytest.mark.parametrize("mode,clock", [("--bench", "total"), ("--client-ready", "ready")])
def test_timing_rows(inputs, mode, clock):
    run, files = inputs()
    verified = run("--verify")
    assert verified.returncode == 0, verified.stderr
    measured = run(mode, "1", "1")
    assert measured.returncode == 0, measured.stderr
    rows = list(csv.DictReader(io.StringIO(measured.stdout)))
    assert len(rows) == 2
    assert {row["stage"] for row in rows} == {"text", "logical_plan"}
    sizes = dict(zip(("text", "logical_plan"), (path.stat().st_size for path in files)))
    for row in rows:
        assert row["iteration"] == "0"
        assert int(row["bytes"]) == sizes[row["stage"]]
        assert int(row[clock + "_wall_ns"]) > 0
        assert int(row[clock + "_cpu_ns"]) > 0


@pytest.mark.parametrize("before,after,error", [
    ('name: "selected"', 'name: "changed"', "Native logical roots differ"),
    ('name: "=="', 'name: ">="', "Native logical roots differ"),
    ("filter { input: 1", "filter { input: 99", "Invalid LogicalPlan reference 99"),
])
def test_rejects_changed_or_invalid_plan(inputs, before, after, error):
    fixture = PLAN + ROOT.format(root_id=1, statement=0, result=0)
    assert fixture.count(before) == 1
    run, _ = inputs(plan=fixture.replace(before, after))
    rejected = run("--bench", "0", "1")
    assert rejected.returncode == 1 and error in rejected.stderr
    assert not rejected.stdout


def test_multiple_selects(inputs):
    query = SQL + " SELECT id AS selected FROM items WHERE id = $min_key;"
    plan = PLAN + ROOT.format(root_id=1, statement=0, result=0) + ROOT.format(root_id=2, statement=1, result=1)
    run, _ = inputs(sql=query, plan=plan)
    verified = run("--verify")
    assert verified.returncode == 0, verified.stderr
