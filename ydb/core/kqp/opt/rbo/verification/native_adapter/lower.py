"""Logical native structures -> strict semantic IR, entirely in source Python.

The handlers are trusted semantic code, just like the existing C++ exporter.
Unknown callables are never made opaque automatically. This initial adapter is
for diagnostic transformation pairs, not physical endpoint equivalence.
"""

from dataclasses import dataclass

from rbo_verifier.ir import parse_snapshot
from rbo_verifier.types import INTEGER_TYPES, integer_bounds

from .archive import AdapterError, NativeArchive, array, boolean, keys, require, string, unhex


SCOPE = "OPTIMIZER_TRANSFORMATION_PAIR"
SCALARS = INTEGER_TYPES | {"Bool"}
# Schema-v1 producer's EOperator IDs. Enum changes need an explicit adapter
# review; a contradictory display name must not reinterpret another operator.
NATIVE_KINDS = {"Read": 1, "Map": 2, "Filter": 4, "Root": 17}


@dataclass(frozen=True)
class Scalar:
    expression: dict
    type: str
    nullable: bool

    @property
    def signature(self):
        return self.type, self.nullable


def scalar_type(raw):
    require(raw is not None, "missing scalar annotation")
    nullable = raw["kind"] == 8
    item = raw["item"] if nullable else raw
    require(item["kind"] == 6 and item["text"] in SCALARS, "unreviewed scalar type")
    if nullable:
        require(raw["text"] in {f"Optional<{item['text']}>", f"{item['text']}?"},
                "Optional type text/structure disagree")
    return item["text"], nullable


def row_type(raw):
    require(raw is not None and raw["kind"] == 5 and raw["item"]["kind"] == 3,
            "operator must carry List<Struct> annotation")
    return struct_type(raw["item"])


def struct_type(raw):
    require(raw is not None and raw["kind"] == 3, "row argument must be Struct annotated")
    return {unhex(item["name_hex"]): scalar_type(item["type"]) for item in raw["items"]}


def _boolean_call(name, args):
    require(all(arg.type == "Bool" for arg in args), f"{name} requires Bool operands")
    require(len(args) == 1 if name == "Not" else len(args) >= 2,
            f"invalid {name} arity")
    if name == "Not":
        expression = {"kind": "not", "arg": args[0].expression}
    else:
        expression = {"kind": name.lower(), "args": [arg.expression for arg in args]}
    return Scalar(expression, "Bool", any(arg.nullable for arg in args))


def _comparison(name, args):
    require(len(args) == 2 and args[0].type == args[1].type,
            "comparison requires two operands of the same reviewed type")
    require(name in {"==", "!="} or args[0].type in INTEGER_TYPES,
            "ordered comparison requires integers")
    expression = {"kind": {"==": "eq", "!=": "eq", "<": "lt", "<=": "lte", ">": "gt", ">=": "gte"}[name],
                  "left": args[0].expression, "right": args[1].expression}
    if name == "!=":
        expression = {"kind": "not", "arg": expression}
    return Scalar(expression, "Bool", any(arg.nullable for arg in args))


def _exists(name, args):
    require(len(args) == 1, "Exists requires one operand")
    return Scalar({"kind": "exists", "arg": args[0].expression}, "Bool", False)


def bool_xor(name, args):
    """Example extension: native two-argument nonnullable Bool Xor.

    Adding this Python handler needs no change to the structural exporter or
    captured JSON. Optional Xor is deliberately excluded from this contract.
    """
    require(len(args) == 2 and all(arg.signature == ("Bool", False) for arg in args),
            "Xor requires two nonnullable Bool operands")
    left, right = (arg.expression for arg in args)
    return Scalar({"kind": "or", "args": [
        {"kind": "and", "args": [left, {"kind": "not", "arg": right}]},
        {"kind": "and", "args": [{"kind": "not", "arg": left}, right]},
    ]}, "Bool", False)


DEFAULT_SCALAR_HANDLERS = {
    **{name: _boolean_call for name in ("And", "Or", "Not")},
    **{name: _comparison for name in ("==", "!=", "<", "<=", ">", ">=")},
    "Exists": _exists,
    "Xor": bool_xor,
}


class Lowerer:
    def __init__(self, archive, scalar_handlers=None):
        self.archive = archive if isinstance(archive, NativeArchive) else NativeArchive(archive)
        self.raw = self.archive.raw
        self.handlers = DEFAULT_SCALAR_HANDLERS if scalar_handlers is None else scalar_handlers
        self.nodes = []
        self.schemas = {}
        self.reachable = set()
        self.scalar_budget = 0

    def safe(self, node):
        require(not node["has_result"] and not node["position_aware"] and not node["has_side_effects"]
                and node["cse_safe"] and not node["unordered_children"], "unsafe scalar metadata")
        if node["kind"] == "Argument":
            require(not node["used_in_depends_on"], "DependsOn row arguments are unsupported")

    def atom(self, node_id):
        node = self.archive.expressions[node_id]
        self.safe(node)
        require(node["kind"] == "Atom" and not node["children"], "expected direct Atom")
        return unhex(node["content_hex"])

    def scalar(self, node_id, row_argument, scope, depth=0):
        self.scalar_budget += 1
        require(depth <= 64 and self.scalar_budget <= 4096, "scalar lowering budget exceeded")
        node = self.archive.expressions[node_id]
        self.safe(node)
        require(node["kind"] == "Callable", "free argument or non-callable scalar")
        name = unhex(node["content_hex"])
        children = node["children"]
        annotated = scalar_type(node["type"])
        if name == "Member":
            require(len(children) == 2 and children[0] == row_argument,
                    "Member does not reference this lambda's row argument")
            column = self.atom(children[1])
            require(column in scope, "Member references an absent input IU")
            result = Scalar({"kind": "column", "column": column}, *scope[column])
        elif name in SCALARS:
            require(len(children) == 1 and annotated == (name, False), "literal type/arity mismatch")
            text = self.atom(children[0])
            if name == "Bool":
                require(text in {"true", "false"}, "invalid Bool literal")
                value = text == "true"
            else:
                try:
                    value = int(text)
                except ValueError as error:
                    raise AdapterError("invalid integer literal") from error
                require(text == str(value), "integer literal must be canonical decimal")
                lower, upper = integer_bounds(name)
                require(lower <= value < upper, "integer literal outside storage domain")
            result = Scalar({"kind": "literal", "type": name, "value": value}, name, False)
        else:
            require(name in self.handlers, f"unsupported callable {name!r}; add a reviewed Python handler")
            args = [self.scalar(child, row_argument, scope, depth + 1) for child in children]
            result = self.handlers[name](name, args)
            require(isinstance(result, Scalar), "scalar handler must return a typed Scalar")
        require(result.signature == annotated, f"{name} result annotation disagrees with its semantics")
        return result

    def expression(self, root, scope):
        require(root is not None, "missing native expression")
        node = self.archive.expressions[root]
        self.safe(node)
        require(node["kind"] == "Lambda" and len(node["children"]) == 2, "expected one-body lambda")
        arguments = self.archive.expressions[node["children"][0]]
        self.safe(arguments)
        require(arguments["kind"] == "Arguments" and len(arguments["children"]) == 1,
                "expected one row argument")
        argument_id = arguments["children"][0]
        argument = self.archive.expressions[argument_id]
        self.safe(argument)
        require(argument["kind"] == "Argument" and argument["argument_index"] == 0,
                "invalid row argument")
        require(struct_type(argument["type"]) == scope, "row argument annotation differs from input schema")
        result = self.scalar(node["children"][1], argument_id, scope)
        if node["type"] is not None:
            require(scalar_type(node["type"]) == result.signature, "lambda result annotation differs from body")
        return result

    def roles(self, node, required):
        result = {item["role"]: item["root"] for item in node["expressions"]}
        require(result.keys() == set(required), f"unexpected expression roles for {node['kind']}")
        return result

    def operator(self, node_id):
        if node_id in self.schemas:
            return self.schemas[node_id]
        node = self.archive.operators[node_id]
        self.reachable.add(node_id)
        require(node["complete"] and node["stage_id"] is None and not node["physical_properties_present"],
                "incomplete or physically assigned operator")
        kind = node["kind"]
        require(kind in {"Root", "Read", "Filter", "Map"}, f"unsupported native operator {kind}")
        require(node["native_kind"] == NATIVE_KINDS[kind], "native operator kind/name disagree")
        children = node["children"]
        require(len(children) == (0 if kind == "Read" else 1), f"invalid {kind} child count")
        source = self.operator(children[0]) if children else None
        fields = node["fields"]
        if kind == "Read":
            keys(fields, {"storage_type", "sort_direction", "table_identity", "columns", "has_range_info"})
            require(type(fields["storage_type"]) is int and fields["storage_type"] in {1, 2}, "unreviewed storage type")
            require(type(fields["sort_direction"]) is int and fields["sort_direction"] == 0,
                    "ordered native reads need a separate contract")
            require(fields["has_range_info"] is False, "read ranges need a separate adapter")
            roles = self.roles(node, {"table", "olap_filter", "limit", "original_predicate", "ranges", "literal_range"})
            require(all(roles[key] is None for key in roles if key != "table"), "pushed reads are unsupported")
            table = self.tables.get(fields["table_identity"])
            require(table is not None, "read table absent from catalog")
            catalog_columns = {column["name"]: column for column in table["columns"]}
            columns = []
            schema = {}
            for item in array(fields["columns"]):
                keys(item, {"id", "column_hex"})
                name = unhex(item["column_hex"])
                require(item["id"] in self.archive.registry and item["id"] not in schema, "invalid read IU")
                require(name in catalog_columns, "read column absent from catalog")
                require(self.archive.registry[item["id"]]["column_hex"] == item["column_hex"], "read registry column differs")
                column = catalog_columns[name]
                require(column["type"] in SCALARS and type(column["nullable"]) is bool, "unreviewed read column type")
                schema[item["id"]] = column["type"], column["nullable"]
                columns.append({"source": name, "output": item["id"]})
            semantic = {"id": node_id, "op": "scan", "table": table["name"], "columns": columns, "pushed_limit": None}
        elif kind == "Filter":
            keys(fields, {"partially_pushed_down"})
            require(fields["partially_pushed_down"] is False, "partially pushed filters require a separate audit")
            roles = self.roles(node, {"predicate"})
            predicate = self.expression(roles["predicate"], source)
            require(predicate.type == "Bool", "filter predicate must be Bool")
            schema = dict(source)
            semantic = {"id": node_id, "op": "filter", "input": children[0], "predicate": predicate.expression}
        elif kind == "Map":
            keys(fields, {"definitions", "need_to_push"})
            boolean(fields["need_to_push"])
            self.roles(node, set())
            schema = dict(source)
            columns = [{"output": name, "expression": {"kind": "column", "column": name}} for name in source]
            for definition in array(fields["definitions"]):
                keys(definition, {"output", "expression"})
                output = string(definition["output"])
                require(output in self.archive.registry and output not in schema, "Map definition requires a fresh IU")
                require(definition["expression"] in self.archive.expressions, "dangling Map definition")
                value = self.expression(definition["expression"], source)
                columns.append({"output": output, "expression": value.expression})
                schema[output] = value.signature
            require(columns, "empty Map projection is outside this adapter")
            semantic = {"id": node_id, "op": "project", "input": children[0], "columns": columns, "ordered": False}
        else:
            require(node_id == self.raw["root"], "nested Root is unsupported")
            keys(fields, {"columns", "query_columns_hex"})
            self.roles(node, {"source_root"})
            require(node["type"] is None, "unexpected Root annotation requires review")
            # QueryColumns are original-label hints; the native ordered Columns
            # map is the actual output projection. Preserve both in the archive.
            for hint in array(fields["query_columns_hex"]):
                unhex(hint)
            columns = []
            schema = {}
            for index, item in enumerate(array(fields["columns"])):
                keys(item, {"id", "label_hex"})
                require(item["id"] in source, "root output IU is absent")
                label = unhex(item["label_hex"])
                output = f"result[{index}]:{label}"
                schema[output] = source[item["id"]]
                columns.append({"output": output, "expression": {"kind": "column", "column": item["id"]}})
            require(columns, "root output must not be empty")
            semantic = {"id": node_id, "op": "project", "input": children[0], "columns": columns, "ordered": True}
        if kind != "Root":
            require(row_type(node["type"]) == schema, f"{kind} native output annotation differs from derived schema")
        self.nodes.append(semantic)
        self.schemas[node_id] = schema
        return schema

    def run(self):
        require(self.raw["logical_complete"] and self.raw["catalog_complete"] and "catalog_error" not in self.raw,
                "incomplete native capture/catalog")
        require(not any(self.raw[key] for key in ("pg_syntax", "with_effects", "with_returning", "stage_graph_present"))
                and not self.raw["subplans"], "physical, effectful, Pg or subplan capture is not a logical diagnostic")
        graph = self.raw["stage_graph"]
        keys(graph, {"complete", "stage_ids", "connections"})
        require(graph["complete"] is False and graph["stage_ids"] == [] and graph["connections"] == [],
                "stage descriptors cannot be erased")
        require(isinstance(self.raw["catalog"], dict), "missing catalog")
        keys(self.raw["catalog"], {"tables"})
        self.tables = {}
        for table in array(self.raw["catalog"]["tables"]):
            name = string(table.get("name"))
            require(name not in self.tables, "duplicate catalog table")
            self.tables[name] = table
        require(self.archive.operators[self.raw["root"]]["kind"] == "Root", "archive root must be Root")
        schema = self.operator(self.raw["root"])
        require(self.reachable == self.archive.operators.keys(), "unreachable operators are not silently ignored")
        snapshot = {"format": "ydb-rbo-semantic-snapshot", "version": 1, "schema": self.raw["catalog"],
                    "plan": {"nodes": self.nodes, "root": self.raw["root"], "output": list(schema), "subplans": []},
                    "stage_graph": None}
        # The existing strict IR validator is a second check on every resulting
        # type, symbol, catalog fact, and operator combination.
        parse_snapshot(snapshot)
        return snapshot


def lower(archive, *, scalar_handlers=None):
    """Return logical semantic JSON; verify only with a diagnostic pair anchor.

    Explicit custom handlers are trusted caller code. The CLI uses only the
    source-pinned built-in registry; it never loads code from archive content.
    """
    return Lowerer(archive, scalar_handlers).run()
