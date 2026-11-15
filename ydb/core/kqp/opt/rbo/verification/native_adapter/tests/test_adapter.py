"""Synthetic native structures: no claim these fixtures came from a compiler."""

import copy
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from native_adapter import AdapterError, NativeArchive, load_archive, lower
from native_adapter.cli import main
from native_adapter.lower import DEFAULT_SCALAR_HANDLERS
from rbo_verifier.ir import parse_snapshot
from rbo_verifier.verify import VerificationError, build_problem, build_transformation_pair_problem, solve


SOLVER = os.environ.get("RBO_Z3")


def hx(text):
    return text.encode().hex()


def data_type(name="Int32", nullable=False):
    value = {"kind": 6, "text": name, "flags": 0}
    return {"kind": 8, "text": f"Optional<{name}>", "flags": 0, "item": value} if nullable else value


def struct_type(columns):
    return {"kind": 3, "text": "Struct", "flags": 0,
            "items": [{"name_hex": hx(name), "type": data_type(*value)} for name, value in columns.items()]}


def row_type(columns):
    return {"kind": 5, "text": "List<Struct>", "flags": 0, "item": struct_type(columns)}


class Fixture:
    def __init__(self, nullable=False):
        self.scope = {"0": ("Int32", False), "1": ("Int32", nullable)}
        self.expressions = []
        self.raw = {
            "format": "ydb-rbo-native-structure", "version": 1, "observation": "diagnostic_only",
            "root": "o0", "catalog_complete": True, "logical_complete": True,
            "pg_syntax": False, "with_effects": False, "with_returning": False,
            "stage_graph_present": False, "stage_graph": {"complete": False, "stage_ids": [], "connections": []},
            "subplans": [], "expressions": self.expressions,
            "catalog": {"tables": [{"name": "T", "columns": [
                {"name": "k", "type": "Int32", "nullable": False},
                {"name": "x", "type": "Int32", "nullable": nullable}], "unique_keys": []}]},
            "registry": [{"id": "0", "alias_hex": hx("t"), "column_hex": hx("k")},
                         {"id": "1", "alias_hex": hx("t"), "column_hex": hx("x")},
                         {"id": "2", "alias_hex": hx("t"), "column_hex": hx("copy")}],
        }
        predicate = self.lambda_(lambda argument: self.call(
            ">", [self.member(argument, "1"), self.literal(10)], data_type("Bool", nullable)),
            result_type=data_type("Bool", nullable))
        self.raw["operators"] = [
            self.op("o0", "Root", ["o1"], None,
                    {"columns": [{"id": "0", "label_hex": hx("key")}, {"id": "1", "label_hex": hx("value")}],
                     "query_columns_hex": []}, [{"role": "source_root", "root": None}]),
            self.op("o1", "Filter", ["o2"], row_type(self.scope), {"partially_pushed_down": False},
                    [{"role": "predicate", "root": predicate}]),
            self.op("o2", "Read", [], row_type(self.scope),
                    {"storage_type": 1, "sort_direction": 0, "table_identity": "T", "has_range_info": False,
                     "columns": [{"id": "0", "column_hex": hx("k")}, {"id": "1", "column_hex": hx("x")}]},
                    [{"role": role, "root": None} for role in
                     ("table", "olap_filter", "limit", "original_predicate", "ranges", "literal_range")]),
        ]

    @staticmethod
    def op(node_id, kind, children, annotation, fields, expressions):
        return {"id": node_id, "kind": kind, "native_kind": {"Root": 17, "Read": 1, "Filter": 4, "Map": 2}[kind],
                "complete": True, "type": annotation,
                "children": children, "fields": fields, "expressions": expressions,
                "stage_id": None, "physical_properties_present": False}

    def expr(self, kind, content="", children=(), annotation=None):
        node_id = f"e{len(self.expressions)}"
        node = {"id": node_id, "kind": kind, "content_hex": hx(content), "children": list(children),
                "type": annotation, "flags": 0, "state": 0, "has_result": False, "result": None,
                "position_aware": False, "has_side_effects": False, "cse_safe": True,
                "unordered_children": False, "dependency_scope": None,
                "constraints_available": False, "constraints_text_hex": []}
        if kind == "Argument":
            node.update(argument_index=0, used_in_depends_on=False)
        self.expressions.append(node)
        return node_id

    def call(self, name, children, annotation):
        return self.expr("Callable", name, children, annotation)

    def literal(self, value):
        return self.call("Int32", [self.expr("Atom", str(value))], data_type())

    def member(self, argument, name):
        return self.call("Member", [argument, self.expr("Atom", name)], data_type(*self.scope[name]))

    def lambda_(self, build_body, result_type):
        argument = self.expr("Argument", "row", annotation=struct_type(self.scope))
        arguments = self.expr("Arguments", children=[argument])
        return self.expr("Lambda", children=[arguments, build_body(argument)], annotation=result_type)

    def drop_filter(self):
        changed = copy.deepcopy(self.raw)
        changed["operators"][0]["children"] = ["o2"]
        changed["operators"].pop(1)
        return changed


class AdapterTest(unittest.TestCase):
    def test_inventory_is_diagnostic_and_preserves_unknown_callable(self):
        fixture = Fixture()
        next(n for n in fixture.expressions if n["kind"] == "Callable" and n["content_hex"] == hx(">"))["content_hex"] = hx("NewApi")
        report = NativeArchive(fixture.raw).inspect()
        self.assertEqual(report["status"], "DIAGNOSTIC_ONLY")
        self.assertEqual(report["callables"]["NewApi"], 1)
        with self.assertRaisesRegex(AdapterError, "unsupported callable"):
            lower(fixture.raw)

    def test_nullable_filter_and_repeated_result_positions(self):
        raw = Fixture(nullable=True).raw
        raw["operators"][0]["fields"]["columns"] += [{"id": "1", "label_hex": hx("value")}]
        snapshot = lower(raw)
        self.assertEqual(snapshot["plan"]["output"], ["result[0]:key", "result[1]:value", "result[2]:value"])
        self.assertEqual([c.nullable for c in parse_snapshot(snapshot).output_schema()], [False, True, True])

    def test_map_appends_without_losing_input_columns(self):
        fixture = Fixture()
        expression = fixture.lambda_(lambda arg: fixture.member(arg, "1"), result_type=data_type())
        map_node = Fixture.op("o3", "Map", ["o1"], row_type({**fixture.scope, "2": ("Int32", False)}),
                              {"definitions": [{"output": "2", "expression": expression}], "need_to_push": False}, [])
        fixture.raw["operators"].append(map_node)
        fixture.raw["operators"][0]["children"] = ["o3"]
        fixture.raw["operators"][0]["fields"]["columns"].append({"id": "2", "label_hex": hx("copy")})
        snapshot = lower(fixture.raw)
        mapped = next(node for node in snapshot["plan"]["nodes"] if node["id"] == "o3")
        self.assertEqual([c["output"] for c in mapped["columns"]], ["0", "1", "2"])

    def test_python_only_handler_addition_reuses_identical_archive_bytes(self):
        fixture = Fixture()
        predicate = fixture.lambda_(lambda arg: fixture.call("Xor", [
            fixture.call(">", [fixture.member(arg, "1"), fixture.literal(10)], data_type("Bool")),
            fixture.call(">", [fixture.member(arg, "0"), fixture.literal(10)], data_type("Bool")),
        ], data_type("Bool")), result_type=data_type("Bool"))
        fixture.raw["operators"][1]["expressions"][0]["root"] = predicate
        captured = json.dumps(fixture.raw, sort_keys=True)
        old_handlers = {key: value for key, value in DEFAULT_SCALAR_HANDLERS.items() if key != "Xor"}
        with self.assertRaisesRegex(AdapterError, "unsupported callable 'Xor'"):
            lower(fixture.raw, scalar_handlers=old_handlers)
        lowered = lower(fixture.raw)
        self.assertEqual(next(n for n in lowered["plan"]["nodes"] if n["id"] == "o1")["predicate"]["kind"], "or")
        self.assertEqual(json.dumps(fixture.raw, sort_keys=True), captured)

    def test_cycle_dangling_duplicate_and_unknown_fields_rejected(self):
        mutations = [
            lambda raw: raw.update(unknown="ignored"),
            lambda raw: raw["operators"][1]["children"].append("missing"),
            lambda raw: raw["operators"][1]["children"].__setitem__(0, "o0"),
            lambda raw: raw["operators"].append(copy.deepcopy(raw["operators"][0])),
            lambda raw: raw["expressions"][-1]["children"].append(raw["expressions"][-1]["id"]),
            lambda raw: raw["expressions"][-1].update(has_result=True, result=raw["expressions"][-1]["id"]),
            lambda raw: raw["expressions"][-1].update(unknown=1),
            lambda raw: raw["expressions"][0].update(content_hex="ABC"),
        ]
        for change in mutations:
            with self.subTest(change=change):
                raw = Fixture().raw
                change(raw)
                with self.assertRaises(AdapterError):
                    NativeArchive(raw)

    def test_leaf_first_serialization_does_not_bypass_depth_limit(self):
        fixture = Fixture()
        child = fixture.literal(0)
        for _ in range(140):
            child = fixture.call("Unreviewed", [child], data_type())
        with self.assertRaisesRegex(AdapterError, "depth budget"):
            NativeArchive(fixture.raw)

    def test_expression_scope_and_type_changes_rejected(self):
        for mutation in ("member_type", "predicate_type", "row_type", "foreign_row", "map_output_type"):
            raw = Fixture().raw
            members = [n for n in raw["expressions"] if n["content_hex"] == hx("Member")]
            if mutation == "member_type":
                members[0]["type"] = data_type("Uint32")
            elif mutation == "predicate_type":
                next(n for n in raw["expressions"] if n["content_hex"] == hx(">"))["type"] = data_type("Bool", True)
            elif mutation == "row_type":
                raw["expressions"][0]["type"]["items"].pop()
            elif mutation == "foreign_row":
                members[0]["children"][0] = members[0]["children"][1]
            else:
                raw["operators"][1]["type"]["item"]["items"][0]["type"] = data_type("Uint32")
            with self.subTest(mutation=mutation), self.assertRaises(AdapterError):
                lower(raw)

    def test_physical_partial_effect_and_unhandled_properties_rejected(self):
        mutations = [
            lambda raw: raw.update(logical_complete=False),
            lambda raw: raw.update(catalog_complete=False),
            lambda raw: raw.update(with_effects=True),
            lambda raw: raw.update(with_returning=True),
            lambda raw: raw.update(pg_syntax=True),
            lambda raw: raw.update(stage_graph_present=True),
            lambda raw: raw["stage_graph"]["stage_ids"].append(1),
            lambda raw: raw["subplans"].append({}),
            lambda raw: raw["operators"][1].update(complete=False),
            lambda raw: raw["operators"][1].update(physical_properties_present=True),
            lambda raw: raw["operators"][1].update(native_kind=2),
            lambda raw: raw["operators"][1].update(stage_id=0),
            lambda raw: raw["operators"][1]["fields"].update(partially_pushed_down=True),
            lambda raw: raw["operators"][1]["fields"].update(unknown=False),
            lambda raw: raw["operators"][2]["fields"].update(has_range_info=True),
            lambda raw: raw["operators"][2]["fields"].update(sort_direction=1),
        ]
        for mutation in mutations:
            raw = Fixture().raw
            mutation(raw)
            with self.subTest(mutation=mutation), self.assertRaises(AdapterError):
                lower(raw)

    def test_scalar_metadata_is_not_erased(self):
        for field, value in (("position_aware", True), ("has_side_effects", True),
                             ("cse_safe", False), ("unordered_children", True)):
            raw = Fixture().raw
            next(n for n in raw["expressions"] if n["content_hex"] == hx(">"))[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(AdapterError, "unsafe scalar metadata"):
                lower(raw)

    def test_default_endpoint_verification_refuses_lowered_logical_after(self):
        parsed = parse_snapshot(lower(Fixture().raw))
        with self.assertRaises(VerificationError):
            build_problem(parsed, parsed, 2)

    def test_duplicate_json_keys_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input.json"
            path.write_text('{"format": "a", "format": "b"}')
            with self.assertRaisesRegex(AdapterError, "duplicate JSON key"):
                load_archive(path)

    def test_cli_receipt_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "native.json"
            output = Path(temporary) / "semantic.json"
            source.write_text(json.dumps(Fixture().raw))
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main(["lower", str(source), "--output", str(output)]), 0)
            receipt = json.loads(Path(str(output) + ".provenance.json").read_text())
            self.assertEqual(receipt["comparison_scope"], "OPTIMIZER_TRANSFORMATION_PAIR")
            self.assertEqual(receipt["semantic_snapshot_sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(receipt["source_archive_sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertIn("native_adapter/lower.py", receipt["adapter_source"]["files"])
            previous = output.read_bytes()
            with redirect_stderr(io.StringIO()):
                self.assertEqual(main(["lower", str(source), "--output", str(output)]), 2)
            self.assertEqual(output.read_bytes(), previous)


@unittest.skipUnless(SOLVER, "set RBO_Z3 to an existing executable; no native build is needed")
class SolverTest(unittest.TestCase):
    def verdict(self, before, after):
        initial, final = parse_snapshot(lower(before)), parse_snapshot(lower(after))
        problem = build_transformation_pair_problem(initial, final, 2, 2000, observation_snapshot=initial)
        return solve(problem, SOLVER, 2, 2000).status

    def test_identity_proves_and_dropped_filter_has_counterexample(self):
        fixture = Fixture(nullable=True)
        self.assertEqual(self.verdict(fixture.raw, fixture.raw), "VERIFIED_BOUNDED")
        self.assertEqual(self.verdict(fixture.raw, fixture.drop_filter()), "COUNTEREXAMPLE")

    def test_xor_extension_matches_boolean_inequality_and_detects_or_mutation(self):
        fixture = Fixture()
        predicate = fixture.lambda_(lambda arg: fixture.call("Xor", [
            fixture.call(">", [fixture.member(arg, "1"), fixture.literal(10)], data_type("Bool")),
            fixture.call(">", [fixture.member(arg, "0"), fixture.literal(10)], data_type("Bool")),
        ], data_type("Bool")), result_type=data_type("Bool"))
        fixture.raw["operators"][1]["expressions"][0]["root"] = predicate
        expected = copy.deepcopy(fixture.raw)
        next(n for n in expected["expressions"] if n["content_hex"] == hx("Xor"))["content_hex"] = hx("!=")
        self.assertEqual(self.verdict(fixture.raw, expected), "VERIFIED_BOUNDED")
        changed = copy.deepcopy(fixture.raw)
        next(n for n in changed["expressions"] if n["content_hex"] == hx("Xor"))["content_hex"] = hx("Or")
        self.assertEqual(self.verdict(fixture.raw, changed), "COUNTEREXAMPLE")


if __name__ == "__main__":
    unittest.main()
