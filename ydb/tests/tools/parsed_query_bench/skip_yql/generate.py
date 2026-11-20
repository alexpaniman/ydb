#!/usr/bin/env python3
"""Prepare a compact read-only LogicalPlan benchmark pair from producer requests."""

import argparse
from pathlib import Path
import subprocess
import tempfile

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from google.protobuf.message import DecodeError


REQUEST = "Ydb.Query.Benchmark.ExecuteQueryRequest"
MAX_REQUEST_BYTES = 16 << 20


def request_class(descriptor):
    files = descriptor_pb2.FileDescriptorSet.FromString(descriptor).file
    pool = descriptor_pool.DescriptorPool()
    for file in files:
        pool.AddSerializedFile(file.SerializeToString())
    return message_factory.GetMessageClass(pool.FindMessageTypeByName(REQUEST))


def encode(message):
    return message.SerializeToString(deterministic=True)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def dense(messages, name):
    require(all(message.id == index for index, message in enumerate(messages, 1)),
            f"Expected dense {name} IDs")
    return {message.id: message for message in messages}


def child_operators(op):
    kind = op.WhichOneof("node")
    if kind == "join":
        return (op.join.left, op.join.right)
    if kind in ("aggregate", "filter", "map"):
        return (getattr(op, kind).input,)
    require(kind in ("read", "replicate_port", "rows"), f"Unsupported operator {kind}")
    return ()


def op_expressions(op):
    kind = op.WhichOneof("node")
    if kind == "map":
        return [item.lambda_expr_id for item in op.map.definitions]
    if kind == "filter":
        return (op.filter.predicate_lambda_expr_id,)
    if kind == "rows" and op.rows.HasField("rows_expr_id"):
        return (op.rows.rows_expr_id,)
    return ()


def expression_edges(expr):
    kind = expr.WhichOneof("node")
    if kind == "list":
        return expr.list.items
    if kind == "call":
        return expr.call.arguments
    if kind == "arguments":
        return expr.arguments.arguments
    if kind == "lambda":
        binding = getattr(expr, "lambda")
        return [binding.arguments_expr_id, *binding.body]
    if kind == "column":
        return (expr.column.row_argument_expr_id,)
    require(kind in ("argument", "atom", "parameter", "type_literal", "world"),
            f"Unsupported expression {kind}")
    return ()


def direct_column(exprs, lambda_id):
    expression = exprs[lambda_id]
    if not expression.HasField("lambda") or expression.HasField("type_id"):
        return None
    binding = getattr(expression, "lambda")
    arguments = exprs[binding.arguments_expr_id]
    if (len(binding.body) != 1 or not arguments.HasField("arguments")
            or arguments.HasField("type_id") or len(arguments.arguments.arguments) != 1):
        return None
    arg = exprs[arguments.arguments.arguments[0]]
    body = exprs[binding.body[0]]
    if (not arg.HasField("argument") or arg.HasField("type_id")
            or not body.HasField("column") or body.column.row_argument_expr_id != arg.id):
        return None
    return body


def live_expressions(operators, expressions, replacements):
    pending = [expr for op in operators for expr in op_expressions(op)]
    live = set()
    while pending:
        expr = pending.pop()
        expr = replacements.get(expr, expr)
        if expr not in live:
            live.add(expr)
            pending.extend(expression_edges(expressions[expr]))
    return live


def intern_literals(expressions, functions):
    # Share only atoms, type literals, and deterministic literal expressions.
    canonical, replacements = {}, {}
    for idx, expression in expressions.items():
        kind = expression.WhichOneof("node")
        edges = expression_edges(expression)
        if kind not in ("atom", "type_literal") and not (
            kind == "call" and functions[expression.call.function_id].name
            in ("Int32", "Uint64", "String", "SafeCast") and all(arg in replacements for arg in edges)
        ):
            continue
        clone = type(expression)()
        clone.CopyFrom(expression)
        clone.ClearField("id")
        if kind == "call":
            clone.call.arguments[:] = [replacements[arg] for arg in edges]
        replacements[idx] = canonical.setdefault(encode(clone), idx)
    return replacements


def check_graph(graph, operators, expressions, hubs):
    require(not graph.subqueries, "Subqueries are outside the compaction profile")
    refs = [graph.input_operator_id]
    ports = {hub: [] for hub in hubs}
    for op in operators.values():
        inputs = child_operators(op)
        if op.HasField("replicate_port"):
            port = op.replicate_port
            ports[port.hub_id].append(port.port_index)
            inputs = (hubs[port.hub_id].input_operator_id,)
        else:
            refs.extend(inputs)
        require(all(0 < ref < op.id for ref in inputs), "Invalid operator order")
    require(all(indices and sorted(indices) == list(range(len(indices))) for indices in ports.values())
            and all(not hub.HasField("location") for hub in hubs.values()),
            "Invalid or unused replicate hub")
    refs.extend(hub.input_operator_id for hub in hubs.values())
    require(len(refs) == len(operators) and set(refs) == set(operators),
            "Unreferenced or reused operator")
    for expr in expressions.values():
        require(not expr.HasField("location")
                and all(0 < ref < expr.id for ref in expression_edges(expr)),
                "Invalid expression order or location")
    require(live_expressions(operators.values(), expressions, {}) == set(expressions),
            "Unreferenced expression")
    return {hub: len(indices) for hub, indices in ports.items()}


def definitions(op):
    kind = op.WhichOneof("node")
    if kind in ("read", "rows"):
        return [col.iu_id for col in getattr(op, kind).columns]
    if kind == "aggregate":
        return [item.output_iu_id for item in op.aggregate.aggregations]
    if kind == "map":
        return [item.output_iu_id for item in op.map.definitions]
    if kind == "replicate_port":
        return [col.output_iu_id for col in op.replicate_port.columns]
    return []


def scope(op, schemas, hubs):
    if op.HasField("replicate_port"):
        return schemas[hubs[op.replicate_port.hub_id].input_operator_id]
    result = {}
    for child in child_operators(op):
        result.update(schemas[child])
    return result


def outputs(op, inputs, schemas, types, optional):
    kind = op.WhichOneof("node")
    result = inputs
    if kind == "aggregate":
        aggregate = op.aggregate
        require(not aggregate.key_iu_ids and not aggregate.distinct_all,
                "Grouped and distinct aggregates are outside the compaction profile")
        require(all(item.input_iu_id in result for item in aggregate.aggregations), "Out-of-scope aggregate input")
        result = {}
    elif kind == "join":
        join = op.join
        require(join.kind in (join.INNER, join.CROSS, join.LEFT, join.LEFT_ONLY)
                and not join.filter_lambda_expr_ids and bool(join.keys) == (join.kind != join.CROSS)
                and not any(key.equal_nulls for key in join.keys), "Unsupported join conditions")
        left, right = schemas[join.left], schemas[join.right]
        require(all(key.left_iu_id in left and key.right_iu_id in right for key in join.keys),
                "Out-of-scope join key")
        if join.kind == join.LEFT:
            right = {iu: optional.get(typ) for iu, typ in right.items()}
        if join.kind == join.LEFT_ONLY:
            return left
        return {**left, **right}
    elif kind == "replicate_port":
        port = op.replicate_port
        if port.port_index:
            require(len(port.columns) == len(result) and {col.input_iu_id for col in port.columns} == set(result),
                    "Expected complete replicate bindings")
            return {col.output_iu_id: result[col.input_iu_id] for col in port.columns}
        require(not port.columns, "Unexpected primary replicate bindings")
        return result
    result.update((iu, types[iu]) for iu in definitions(op))
    return result


def aliases_for(op, schemas, expressions, types):
    available = schemas[op.map.input]
    aliases = {}
    for item in op.map.definitions:
        expr = direct_column(expressions, item.lambda_expr_id)
        if expr is None:
            return {}
        iu = expr.column.iu_id
        if (iu not in available or item.output_iu_id in available or item.output_iu_id in aliases
                or available[iu] != types[item.output_iu_id]
                or (expr.HasField("type_id") and expr.type_id != available[iu])):
            return {}
        aliases[item.output_iu_id] = iu
    return aliases


def rewrite_operator(op, operator, unit, expr):
    kind = op.WhichOneof("node")
    node = getattr(op, kind)
    if kind in ("read", "rows"):
        for col in node.columns:
            col.iu_id = unit(col.iu_id)
        if kind == "rows" and node.HasField("rows_expr_id"):
            node.rows_expr_id = expr(node.rows_expr_id)
    elif kind == "join":
        node.left, node.right = operator(node.left), operator(node.right)
        for item in node.keys:
            item.left_iu_id, item.right_iu_id = unit(item.left_iu_id), unit(item.right_iu_id)
    elif kind == "replicate_port":
        for col in node.columns:
            col.input_iu_id, col.output_iu_id = unit(col.input_iu_id), unit(col.output_iu_id)
    else:
        node.input = operator(node.input)
        if kind == "map":
            for item in node.definitions:
                item.output_iu_id, item.lambda_expr_id = unit(item.output_iu_id), expr(item.lambda_expr_id)
        elif kind == "filter":
            node.predicate_lambda_expr_id = expr(node.predicate_lambda_expr_id)
        elif kind == "aggregate":
            for item in node.aggregations:
                item.input_iu_id, item.output_iu_id = unit(item.input_iu_id), unit(item.output_iu_id)


def rewrite_expression(expression, expr, unit):
    kind = expression.WhichOneof("node")
    if kind == "call":
        expression.call.arguments[:] = [expr(i) for i in expression.call.arguments]
    elif kind == "list":
        expression.list.items[:] = [expr(i) for i in expression.list.items]
    elif kind == "lambda":
        binding = getattr(expression, "lambda")
        binding.arguments_expr_id = expr(binding.arguments_expr_id)
        binding.body[:] = [expr(i) for i in binding.body]
    elif kind == "arguments":
        expression.arguments.arguments[:] = [expr(i) for i in expression.arguments.arguments]
    elif kind == "column":
        expression.column.row_argument_expr_id = expr(expression.column.row_argument_expr_id)
        expression.column.iu_id = unit(expression.column.iu_id)


def follow(replacements, index):
    while index in replacements:
        index = replacements[index]
    return index


def reindex(entries):
    return {old: new for new, old in enumerate(entries, 1)}


def preserves_keys(join_keys, result_ius, aliases):
    if len({follow(aliases, iu) for iu in result_ius}) != len(result_ius):
        return False
    for keys in join_keys:
        resolved = [(follow(aliases, left), follow(aliases, right)) for left, right in keys]
        if resolved != sorted(set(resolved)):
            return False
    return True


def compact_root(root, plan_types, functions):
    graph = root.graph
    operators = dense(graph.operators, "operator")
    expressions = dense(graph.expressions, "expression")
    units = dense(graph.information_units, "IU")
    hubs = dense(graph.replicate_hubs, "hub")
    port_counts = check_graph(graph, operators, expressions, hubs)
    types = {idx: unit.defined_type_id for idx, unit in units.items()}
    require(all(typ in plan_types for typ in types.values()), "Missing IU type")
    defined = [iu for op in operators.values() for iu in definitions(op)]
    require(len(defined) == len(units) and set(defined) == set(units), "Unreferenced or redefined IU")
    optional = {typ.optional_item: typ.id for typ in plan_types.values() if typ.HasField("optional_item")}
    optional.update({typ.id: typ.id for typ in plan_types.values() if typ.HasField("optional_item")})

    # Secondary ports rebind the entire row. Keep any named input binding intact.
    pinned = {col.input_iu_id for op in operators.values() for col in op.replicate_port.columns}
    join_keys = [sorted((key.left_iu_id, key.right_iu_id) for key in op.join.keys)
                 for op in operators.values() if op.HasField("join")]
    result_ius = {col.iu_id for col in root.columns}
    operator_aliases, unit_aliases, removed_hubs, schemas = {}, {}, set(), {}
    for idx, op in operators.items():
        inputs = scope(op, schemas, hubs)
        used = live_expressions((op,), expressions, {})
        require(all(expressions[expr].column.iu_id in inputs for expr in used
                    if expressions[expr].HasField("column")), "Out-of-scope expression IU")
        schemas[idx] = outputs(op, inputs, schemas, types, optional)
        if op.HasField("replicate_port"):
            port = op.replicate_port
            if port_counts[port.hub_id] == 1 and not port.port_index and not port.columns:
                removed_hubs.add(port.hub_id)
                operator_aliases[idx] = hubs[port.hub_id].input_operator_id
        elif op.HasField("map"):
            aliases = aliases_for(op, schemas, expressions, types)
            if aliases and not aliases.keys() & pinned:
                candidate = unit_aliases | aliases
                # Native join keys are sorted by IU pair. The producer compares that order.
                if preserves_keys(join_keys, result_ius, candidate):
                    unit_aliases = candidate
                    operator_aliases[idx] = op.map.input

    require(result_ius and result_ius <= schemas[graph.input_operator_id].keys(), "Out-of-scope result IU")

    live_operators = {idx: op for idx, op in operators.items() if idx not in operator_aliases}
    expression_aliases = intern_literals(expressions, functions)
    live = live_expressions(live_operators.values(), expressions, expression_aliases)
    live_expressions_by_id = {idx: expr for idx, expr in expressions.items() if idx in live}
    live_units = {idx: unit for idx, unit in units.items() if idx not in unit_aliases}
    live_hubs = {idx: hub for idx, hub in hubs.items() if idx not in removed_hubs}

    operator_ids = reindex(live_operators)
    unit_ids = reindex(live_units)
    expression_ids = reindex(live_expressions_by_id)
    hub_ids = reindex(live_hubs)

    def operator(idx):
        return operator_ids[follow(operator_aliases, idx)]

    def unit(idx):
        return unit_ids[follow(unit_aliases, idx)]

    def expr(idx):
        return expression_ids[expression_aliases.get(idx, idx)]

    graph.input_operator_id = operator(graph.input_operator_id)
    for col in root.columns:
        col.iu_id = unit(col.iu_id)
    for idx, op in live_operators.items():
        op.id = operator_ids[idx]
        rewrite_operator(op, operator, unit, expr)
        if op.HasField("replicate_port"):
            op.replicate_port.hub_id = hub_ids[op.replicate_port.hub_id]
    for idx, expression in live_expressions_by_id.items():
        expression.id = expression_ids[idx]
        rewrite_expression(expression, expr, unit)
    for idx, value in live_units.items():
        value.id = unit_ids[idx]
    for idx, hub in live_hubs.items():
        hub.id = hub_ids[idx]
        hub.input_operator_id = operator(hub.input_operator_id)
    for name, entries in (("operators", live_operators), ("expressions", live_expressions_by_id),
                          ("information_units", live_units), ("replicate_hubs", live_hubs)):
        messages = getattr(graph, name)
        del messages[:]
        messages.extend(entries.values())


def has_enum(message, field, name):
    enumeration = message.DESCRIPTOR.fields_by_name[field].enum_type
    return getattr(message, field) == enumeration.values_by_name[name].number


def compact(request_type, text_bytes, plan_bytes):
    require(max(len(text_bytes), len(plan_bytes)) <= MAX_REQUEST_BYTES, "Input exceeds 16 MiB")
    text = request_type.FromString(text_bytes)
    structured = request_type.FromString(plan_bytes)
    require(has_enum(text, "exec_mode", "EXEC_MODE_EXECUTE"), "Expected EXEC_MODE_EXECUTE")
    require(text.HasField("query_content") and has_enum(text.query_content, "syntax", "SYNTAX_YQL_V1"),
            "Expected YQL v1 text")
    require(structured.HasField("parsed_query") and structured.parsed_query.HasField("logical_plan"),
            "Expected a logical_plan request")
    parsed = type(structured.parsed_query)()
    parsed.CopyFrom(structured.parsed_query)
    structured.ClearField("query")
    text.ClearField("query")
    require(encode(text) == encode(structured), "Request envelopes differ")
    plan = parsed.logical_plan
    original = encode(parsed)
    parsed.DiscardUnknownFields()
    require(encode(parsed) == original, "Unknown fields in parsed_query")
    require(plan.version == 1 and not plan.required_features and plan.roots,
            "Expected LogicalPlan v1 without required features")
    types, functions = dense(plan.types, "type"), dense(plan.functions, "function")
    previous_statement = -1
    for index, root in enumerate(dense(plan.roots, "root").values()):
        require(root.HasField("graph") and root.HasField("statement_index")
                and root.statement_index > previous_statement
                and root.HasField("result_index") and root.result_index == index
                and not root.HasField("returning_limit_expr_id") and not root.ordered_rows,
                "Expected unordered read-only result roots in statement order")
        compact_root(root, types, functions)
        previous_statement = root.statement_index
    structured.parsed_query.CopyFrom(parsed)
    return encode(structured)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, required=True, help="benchmark --dump-descriptor output")
    parser.add_argument("--verify-with", type=Path, help="path to parsed_query_skip_yql_bench")
    parser.add_argument("--catalog", type=Path, help="offline catalog for --verify-with")
    parser.add_argument("text", type=Path)
    parser.add_argument("source_plan", type=Path)
    parser.add_argument("output", type=Path, help="new pair directory")
    args = parser.parse_args()
    if bool(args.catalog) != bool(args.verify_with):
        parser.error("--catalog and --verify-with must be used together")
    try:
        text = args.text.read_bytes()
        source = args.source_plan.read_bytes()
        plan = compact(request_class(args.descriptor.read_bytes()), text, source)
        require(not args.output.exists() and not args.output.is_symlink(), f"Output already exists: {args.output}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
            candidate = Path(temporary) / "pair"
            candidate.mkdir()
            text_path = candidate / "text.pb"
            plan_path = candidate / "logical_plan.pb"
            text_path.write_bytes(text)
            plan_path.write_bytes(plan)
            if args.verify_with:
                base = [args.verify_with.resolve(), "--catalog", args.catalog, "--verify", text_path]
                source_path = Path(temporary) / "source_plan.pb"
                source_path.write_bytes(source)
                subprocess.run([*base, source_path], check=True)
                subprocess.run([*base, plan_path], check=True)
            candidate.rename(args.output)
    except (OSError, ValueError, KeyError, TypeError, DecodeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Cannot generate LogicalPlan pair: {error}\n")


if __name__ == "__main__":
    main()
