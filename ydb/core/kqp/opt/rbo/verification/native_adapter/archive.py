"""Bounded structural decoder. Decoding a diagnostic archive is never a proof."""

from collections import Counter
import json
from pathlib import Path
import re


MAX_BYTES = 64 << 20
MAX_NODES = 65_536
MAX_REFERENCES = MAX_NODES * 8
MAX_DEPTH = 128


class AdapterError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AdapterError(message)


def keys(value, required, optional=()):
    require(isinstance(value, dict), "expected object")
    missing = set(required) - value.keys()
    extra = value.keys() - set(required) - set(optional)
    require(not missing and not extra,
            f"object fields differ: missing={sorted(missing)}, unknown={sorted(extra)}")


def uint(value):
    require(type(value) is int and value >= 0, "expected nonnegative integer")
    return value


def boolean(value):
    require(type(value) is bool, "expected Boolean")
    return value


def string(value):
    require(isinstance(value, str), "expected string")
    return value


def array(value):
    require(isinstance(value, list) and len(value) <= MAX_NODES, "invalid or oversized array")
    return value


def unhex(value, *, text=True):
    string(value)
    require(len(value) <= MAX_BYTES and re.fullmatch(r"(?:[0-9a-f]{2})*", value) is not None,
            "expected lowercase byte hex")
    result = bytes.fromhex(value)
    if not text:
        return result
    try:
        return result.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AdapterError("this adapter requires a UTF-8 name or callable") from error


def unique_map(items, key="id"):
    result = {}
    for item in array(items):
        require(isinstance(item, dict), "expected indexed object")
        name = string(item.get(key))
        require(name and name not in result, f"duplicate/empty {key}: {name!r}")
        result[name] = item
    return result


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_archive(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), "archive must be a regular file")
    require(path.stat().st_size <= MAX_BYTES, "archive byte budget exceeded")
    try:
        with path.open("rb") as source:
            data = source.read(MAX_BYTES + 1)
        require(len(data) <= MAX_BYTES, "archive byte budget exceeded")
        raw = json.loads(data, object_pairs_hook=_json_object,
                         parse_constant=lambda value: (_ for _ in ()).throw(AdapterError(f"invalid JSON constant: {value}")))
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise AdapterError(f"invalid archive JSON: {error}") from error
    return NativeArchive(raw)


class NativeArchive:
    def __init__(self, raw):
        keys(raw, {"format", "version", "observation", "root", "catalog", "catalog_complete",
                   "logical_complete", "operators", "expressions", "registry", "subplans",
                   "stage_graph_present", "stage_graph", "pg_syntax", "with_effects", "with_returning"},
             {"catalog_error"})
        require(raw["format"] == "ydb-rbo-native-structure" and type(raw["version"]) is int and raw["version"] == 1,
                "unsupported native archive format/version")
        require(raw["observation"] == "diagnostic_only", "native structures are diagnostic-only")
        for name in ("catalog_complete", "logical_complete", "stage_graph_present", "pg_syntax", "with_effects", "with_returning"):
            boolean(raw[name])
        self.raw = raw
        self.operators = unique_map(raw["operators"])
        self.expressions = unique_map(raw["expressions"])
        self.registry = unique_map(raw["registry"])
        require(string(raw["root"]) in self.operators, "missing root operator")
        self._type_nodes = 0
        for item in self.registry.values():
            keys(item, {"id", "alias_hex", "column_hex"})
            require(re.fullmatch(r"0|[1-9][0-9]*", item["id"]) is not None, "IU ID must be canonical decimal")
            unhex(item["alias_hex"], text=False)
            unhex(item["column_hex"], text=False)
        for node in self.operators.values():
            keys(node, {"id", "kind", "native_kind", "complete", "type", "children", "fields",
                        "expressions", "stage_id", "physical_properties_present"})
            string(node["kind"])
            uint(node["native_kind"])
            boolean(node["complete"])
            boolean(node["physical_properties_present"])
            if node["stage_id"] is not None:
                uint(node["stage_id"])
            self._type(node["type"])
            require(isinstance(node["fields"], dict), "operator fields must be an object")
            for child in array(node["children"]):
                require(string(child) in self.operators, "dangling operator child")
            roles = set()
            for expression in array(node["expressions"]):
                keys(expression, {"role", "root"})
                role = string(expression["role"])
                require(role not in roles, "duplicate expression role")
                roles.add(role)
                self._ref(expression["root"])
        for node in self.expressions.values():
            common = {"id", "kind", "content_hex", "flags", "state", "type", "children", "has_result",
                      "result", "position_aware", "has_side_effects", "cse_safe", "unordered_children",
                      "dependency_scope", "constraints_available", "constraints_text_hex"}
            keys(node, common | ({"argument_index", "used_in_depends_on"} if node["kind"] == "Argument" else set()))
            require(node["kind"] in {"Atom", "Callable", "Lambda", "Argument", "Arguments", "List", "World"},
                    "unknown expression node kind")
            unhex(node["content_hex"], text=False)
            uint(node["flags"])
            uint(node["state"])
            self._type(node["type"])
            for flag in ("has_result", "position_aware", "has_side_effects", "cse_safe", "unordered_children", "constraints_available"):
                boolean(node[flag])
            require(node["has_result"] == (node["result"] is not None), "inconsistent result metadata")
            self._ref(node["result"])
            for child in array(node["children"]):
                self._ref(child, optional=False)
            scope = node["dependency_scope"]
            if scope is not None:
                keys(scope, {"outer", "inner"})
                self._ref(scope["outer"])
                self._ref(scope["inner"])
            for constraint in array(node["constraints_text_hex"]):
                unhex(constraint, text=False)
            require(node["constraints_available"] or not node["constraints_text_hex"], "constraints unavailable but supplied")
            if node["kind"] == "Argument":
                uint(node["argument_index"])
                boolean(node["used_in_depends_on"])
            if node["kind"] in {"Atom", "Argument", "World"}:
                require(not node["children"], "leaf expression has children")
        self._acyclic(self.operators)
        self._acyclic(self.expressions, results=True)
        # Stage/subplan payloads remain diagnostic. Their schemas are not an
        # adapter contract, and every nonempty payload is rejected by lower().
        array(raw["subplans"])
        require(isinstance(raw["stage_graph"], dict), "stage_graph must be an object")

    def _ref(self, value, optional=True):
        require((optional and value is None) or isinstance(value, str) and value in self.expressions,
                "dangling expression reference")

    def _type(self, value, depth=0):
        if value is None:
            return
        self._type_nodes += 1
        require(depth <= 64 and self._type_nodes <= MAX_REFERENCES, "type depth/node budget exceeded")
        require(isinstance(value, dict), "invalid type annotation")
        kind = value.get("kind")
        required = {"kind", "text", "flags"} | ({"item"} if kind in {5, 8} else {"items"} if kind == 3 else set())
        keys(value, required)
        uint(kind)
        uint(value["flags"])
        string(value["text"])
        if kind in {5, 8}:
            require(value["item"] is not None, "missing item type")
            self._type(value["item"], depth + 1)
        if kind == 3:
            names = set()
            for item in array(value["items"]):
                keys(item, {"name_hex", "type"})
                name = unhex(item["name_hex"], text=False)
                require(name not in names and item["type"] is not None, "duplicate or untyped struct member")
                names.add(name)
                self._type(item["type"], depth + 1)

    @staticmethod
    def _acyclic(nodes, results=False):
        done = set()
        active = set()
        heights = {}
        references = 0
        for root in nodes:
            if root in done:
                continue
            stack = [(root, False, 0)]
            while stack:
                key, leaving, depth = stack.pop()
                if leaving:
                    active.remove(key)
                    done.add(key)
                    children = list(nodes[key]["children"])
                    if results and nodes[key]["result"] is not None:
                        children.append(nodes[key]["result"])
                    heights[key] = 1 + max((heights[child] for child in children), default=0)
                    require(heights[key] <= MAX_DEPTH + 1, "structural graph depth budget exceeded")
                    continue
                if key in done:
                    continue
                require(key not in active, "cyclic structural graph")
                require(depth <= MAX_DEPTH, "structural graph depth budget exceeded")
                active.add(key)
                stack.append((key, True, depth))
                children = list(nodes[key]["children"])
                if results and nodes[key]["result"] is not None:
                    children.append(nodes[key]["result"])
                references += len(children)
                require(references <= MAX_REFERENCES, "structural reference budget exceeded")
                stack.extend((child, False, depth + 1) for child in reversed(children))

    def inspect(self):
        return {"status": "DIAGNOSTIC_ONLY", "logical_complete": self.raw["logical_complete"],
                "operators": dict(sorted(Counter(n["kind"] for n in self.operators.values()).items())),
                "callables": dict(sorted(Counter(unhex(n["content_hex"]) for n in self.expressions.values()
                                                  if n["kind"] == "Callable").items())),
                "incomplete_operators": [n["id"] for n in self.operators.values() if not n["complete"]],
                "note": "Structural completeness and inspection establish no equivalence."}
