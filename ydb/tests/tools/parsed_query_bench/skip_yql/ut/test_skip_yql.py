import csv
import importlib.util
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


@pytest.fixture(scope="module")
def generator():
    path = yatest.common.source_path(TOOL.rsplit("/", 1)[0] + "/generate.py")
    spec = importlib.util.spec_from_file_location("generate", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    descriptor = subprocess.check_output([yatest.common.binary_path(TOOL), "--dump-descriptor"])
    return module, module.request_class(descriptor)


@pytest.fixture
def compact_request(inputs, generator):
    _, (text, logical) = inputs()
    module, request_class = generator
    request = request_class.FromString(logical.read_bytes())

    def compact():
        original = request.SerializeToString()
        output = module.compact(request_class, text.read_bytes(), original)
        assert module.compact(request_class, text.read_bytes(), output) == output
        return request_class.FromString(output).parsed_query.logical_plan

    return request.parsed_query.logical_plan, compact


def append_alias(root, source, output):
    graph = root.graph
    row = graph.expressions.add(id=len(graph.expressions) + 1)
    row.argument.SetInParent()
    arguments = graph.expressions.add(id=row.id + 1)
    arguments.arguments.arguments.append(row.id)
    body = graph.expressions.add(id=row.id + 2)
    body.column.row_argument_expr_id = row.id
    body.column.iu_id = source
    expr = graph.expressions.add(id=row.id + 3)
    binding = getattr(expr, "lambda")
    binding.arguments_expr_id = arguments.id
    binding.body.append(body.id)
    graph.information_units.add(id=output, defined_type_id=1)
    return expr.id


def append_operator(graph):
    operator = graph.operators.add(id=len(graph.operators) + 1)
    graph.input_operator_id = operator.id
    return operator


def test_compact_elides_aliases_and_single_reader_in_each_root(compact_request):
    plan, compact = compact_request
    second = plan.roots.add()
    second.CopyFrom(plan.roots[0])
    second.id, second.statement_index, second.result_index = 2, 3, 1
    for root in plan.roots:
        graph = root.graph
        graph.replicate_hubs.add(id=1, input_operator_id=graph.input_operator_id)
        port = append_operator(graph)
        port.replicate_port.hub_id = 1
        alias = append_operator(graph)
        alias.map.input = port.id
        alias.map.definitions.add(output_iu_id=2, lambda_expr_id=append_alias(root, 1, 2))
        root.columns[0].iu_id = 2

    output = compact()
    assert [(root.id, root.statement_index, root.result_index) for root in output.roots] == [(1, 0, 0), (2, 3, 1)]
    for root in output.roots:
        assert [op.WhichOneof("node") for op in root.graph.operators] == ["read", "filter"]
        assert root.graph.input_operator_id == 2 and not root.graph.replicate_hubs
        assert len(root.graph.expressions) == 6 and len(root.graph.information_units) == 1
        assert [(column.iu_id, column.name) for column in root.columns] == [(1, "selected")]
    assert list(output.parameters) == list(plan.parameters)


def test_compact_preserves_distinct_output_bindings(compact_request):
    plan, compact = compact_request
    root = plan.roots[0]
    alias = append_operator(root.graph)
    alias.map.input = 2
    alias.map.definitions.add(output_iu_id=2, lambda_expr_id=append_alias(root, 1, 2))
    root.columns.add(iu_id=2, name="same_value")
    assert compact() == plan


def test_compact_preserves_mixed_map(compact_request):
    plan, compact = compact_request
    root = plan.roots[0]
    alias = append_operator(root.graph)
    alias.map.input = 2
    alias.map.definitions.add(output_iu_id=2, lambda_expr_id=append_alias(root, 1, 2))
    alias.map.definitions.add(output_iu_id=3, lambda_expr_id=append_alias(root, 1, 3))
    root.graph.expressions[-2].parameter = "$min_key"
    root.graph.expressions[-2].type_id = 1
    root.columns[0].iu_id = 2
    root.columns.add(iu_id=3, name="computed")
    assert compact() == plan


def test_compact_shares_context_free_literals(compact_request):
    plan, compact = compact_request
    plan.functions.add(id=2, kind=plan.functions[0].YQL_CALLABLE, name="Int32")
    root = plan.roots[0]
    alias = append_operator(root.graph)
    alias.map.input = 2
    for output in (2, 3):
        atom = root.graph.expressions.add(id=len(root.graph.expressions) + 1)
        atom.atom.content = b"7"
        lambda_id = append_alias(root, 1, output)
        body = root.graph.expressions[-2]
        body.type_id = 1
        body.call.function_id = 2
        body.call.arguments.append(atom.id)
        alias.map.definitions.add(output_iu_id=output, lambda_expr_id=lambda_id)
        root.columns.add(iu_id=output, name=f"literal{output}")

    output = compact().roots[0].graph
    assert len([expr for expr in output.expressions if expr.HasField("atom")]) == 1
    assert len([expr for expr in output.expressions if expr.HasField("call") and expr.call.function_id == 2]) == 1
    assert len(output.operators[-1].map.definitions) == 2


def test_compact_preserves_shared_producer(compact_request):
    plan, compact = compact_request
    root = plan.roots[0]
    graph = root.graph
    graph.replicate_hubs.add(id=1, input_operator_id=graph.input_operator_id)
    primary = append_operator(graph)
    primary.replicate_port.hub_id = 1
    secondary = append_operator(graph)
    secondary.replicate_port.hub_id = 1
    secondary.replicate_port.port_index = 1
    secondary.replicate_port.columns.add(input_iu_id=1, output_iu_id=2)
    graph.information_units.add(id=2, defined_type_id=1)
    join = append_operator(graph).join
    join.left, join.right, join.kind = primary.id, secondary.id, join.INNER
    join.keys.add(left_iu_id=1, right_iu_id=2)
    assert compact() == plan


def test_compact_rejects_alias_outside_input_scope(compact_request):
    plan, compact = compact_request
    root = plan.roots[0]
    alias = append_operator(root.graph)
    alias.map.input = 2
    alias.map.definitions.add(output_iu_id=2, lambda_expr_id=append_alias(root, 2, 2))
    root.columns[0].iu_id = 2
    with pytest.raises(ValueError, match="Out-of-scope expression IU"):
        compact()
