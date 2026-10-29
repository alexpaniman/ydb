"""Dormant C31 field observation: source reviewed, actual graph still required.

This module never grants finding acceptance or the three existing full-proof
flags. Raw graph nodes, field names and provenance tokens remain in memory.
Only the two existing domain/spilling profiles are supported. Unknown value
computations erase provenance instead of inheriting generic dependencies.
"""
from dataclasses import dataclass, replace
import hashlib
import json

import quality_window_checks as windows

VERSION = 'c31_constrained_shared_fields_v1'
CASES = frozenset(('c31_stability_float_domain__spilling__left',
                   'c31_stability_integer_domain__spilling__left'))
_FLOW = frozenset(('ToFlow', 'FromFlow', 'ToStream', 'Iterator', 'Collect'))
_CONNECTIONS = frozenset(('DqCnMap', 'DqCnHashShuffle', 'DqCnBroadcast',
                          'DqCnUnionAll', 'DqCnParallelUnionAll', 'DqCnMerge'))
_JOIN_NAMES = frozenset(('GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore',
                         'BlockHashJoinCore', 'BlockMapJoinCore'))
_UNKNOWN = object()


class _Reject(ValueError):
    pass


@dataclass(eq=False)
class _Node:
    head: str
    children: tuple


class _Arg:
    pass


@dataclass(frozen=True)
class _Field:
    hub: object
    port: int
    source: str
    codecs: tuple = ()
    domains: frozenset = frozenset()
    joins: frozenset = frozenset()


@dataclass
class _Stream:
    layout: str = 'unknown'
    values: object = None


def _head(node):
    return node.head if isinstance(node, _Node) else None


def _atom(value):
    if not isinstance(value, str):
        raise _Reject('atom_shape')
    value = value[1:] if value.startswith("'") else value
    value = json.loads(value) if value.startswith('"') else value
    if not isinstance(value, str):
        raise _Reject('atom_shape')
    return value


def _index(value):
    value = _atom(value)
    if not value.isdigit() or len(value) > 6 or int(value) > 65536:
        raise _Reject('column_index')
    return int(value)


def _lambda(node):
    if _head(node) != 'lambda' or not node.children:
        raise _Reject('lambda_shape')
    return node.children[0], node.children[1:]


class _LexicalGraph:
    """Resolve printer let/block scopes without flattening lambda returns."""
    def __init__(self, text):
        self.count = 0
        self.names = set()
        self.block_count = 0
        self.roots = self._function(windows._parse_ast(text), {})
        if len(self.roots) != 1:
            raise _Reject('root_return_arity')
        self.nodes = set()
        seen_tuples = set()
        pending = list(self.roots)
        while pending:
            item = pending.pop()
            if isinstance(item, _Node):
                if item in self.nodes:
                    continue
                self.nodes.add(item)
                pending.extend(item.children)
            elif isinstance(item, tuple):
                identity = id(item)
                if identity in seen_tuples:
                    continue
                seen_tuples.add(identity)
                pending.extend(item)

    def _new_name(self, value):
        if not isinstance(value, str) or not value.startswith('$') or value in self.names:
            raise _Reject('binding_identity')
        self.names.add(value)

    def _function(self, statements, parent):
        if not isinstance(statements, list):
            raise _Reject('function_shape')
        env = dict(parent)
        returned = None
        for statement in statements:
            if returned is not None or not isinstance(statement, list) or not statement:
                raise _Reject('function_statement')
            if statement[0] == 'let' and len(statement) == 3:
                name = statement[1]
                self._new_name(name)
                env[name] = self._normal(statement[2], env)
            elif statement[0] == 'return':
                returned = tuple(self._normal(value, env) for value in statement[1:])
            else:
                # Parameter declarations and other statement languages are
                # intentionally outside these two fixed fixture profiles.
                raise _Reject('function_statement')
        if returned is None:
            raise _Reject('missing_return')
        return returned

    def _normal(self, value, env):
        self.count += 1
        if self.count > 200000:
            raise _Reject('graph_budget')
        if isinstance(value, str):
            if value.startswith('$'):
                if value not in env:
                    raise _Reject('unbound_reference')
                return env[value]
            return value
        if not isinstance(value, list):
            raise _Reject('expression_shape')
        head = value[0] if value and isinstance(value[0], str) else None
        if head == 'lambda':
            if len(value) < 2 or not isinstance(value[1], list):
                raise _Reject('lambda_arguments')
            nested, args = dict(env), []
            for name in value[1]:
                self._new_name(name)
                arg = _Arg()
                nested[name] = arg
                args.append(arg)
            bodies = value[2:]
            if len(bodies) == 1 and isinstance(bodies[0], list) and bodies[0][:1] == ['block']:
                if len(bodies[0]) != 2:
                    raise _Reject('block_shape')
                self.block_count += 1
                bodies = self._function(bodies[0][1], nested)
            else:
                bodies = tuple(self._normal(body, nested) for body in bodies)
            return _Node('lambda', (tuple(args), *bodies))
        if head == 'block':
            if len(value) != 2:
                raise _Reject('block_shape')
            self.block_count += 1
            bodies = self._function(value[1], env)
            if len(bodies) != 1:
                raise _Reject('expression_block_arity')
            return bodies[0]
        callable_head = head is not None and not head.startswith(('$', "'", '"'))
        if callable_head:
            return _Node(head, tuple(self._normal(child, env) for child in value[1:]))
        return tuple(self._normal(child, env) for child in value)


class _Proof:
    def __init__(self, graph):
        self.graph = graph
        self.stages = {node for node in graph.nodes if node.head == 'DqPhyStage'}
        if not 1 <= len(self.stages) <= 128:
            raise _Reject('stage_count')
        self.cache, self.active = {}, set()
        self.hubs, self.joins, self.domains = {}, {}, {}
        self.unknown_calls = set()

    def _apply(self, node, values, env):
        args, bodies = _lambda(node)
        if len(args) != len(values):
            raise _Reject('lambda_input_arity')
        nested = dict(env)
        nested.update(zip(args, values))
        return tuple(self._scalar(body, nested) for body in bodies)

    def _scalar(self, node, env):
        if isinstance(node, _Arg):
            return env.get(node, _UNKNOWN)
        if isinstance(node, tuple):
            return tuple(self._scalar(value, env) for value in node)
        head = _head(node)
        if head == 'Member' and len(node.children) == 2:
            row = self._scalar(node.children[0], env)
            return row.get(_atom(node.children[1]), _UNKNOWN) if isinstance(row, dict) else _UNKNOWN
        if head == 'Nth' and len(node.children) == 2:
            row = self._scalar(node.children[0], env)
            index = _index(node.children[1])
            return row[index] if isinstance(row, tuple) and index < len(row) else _UNKNOWN
        if head == 'AsStruct':
            fields = {}
            for pair in node.children:
                if not isinstance(pair, tuple) or len(pair) != 2:
                    raise _Reject('struct_field')
                name = _atom(pair[0])
                if name in fields:
                    raise _Reject('duplicate_struct_field')
                fields[name] = self._scalar(pair[1], env)
            return fields
        if head == 'StablePickle' and len(node.children) == 1:
            field = self._scalar(node.children[0], env)
            if isinstance(field, _Field):
                return replace(field, codecs=field.codecs + ('StablePickle',))
        return _UNKNOWN

    def _connection(self, node):
        if _head(node) not in _CONNECTIONS or not node.children:
            return _Stream()
        output = node.children[0]
        if _head(output) != 'TDqOutput' or len(output.children) != 2:
            raise _Reject('connection_output')
        stage, port = output.children
        port = _index(port)
        outputs = self._stage(stage)
        if port >= len(outputs):
            raise _Reject('output_port')
        return outputs[port]

    def _hub(self, stage, body):
        while _head(body) in _FLOW and len(body.children) == 1:
            body = body.children[0]
        if _head(body) != 'Switch':
            return None
        children = body.children
        if len(children) < 6 or (len(children) - 2) % 2:
            raise _Reject('switch_arity')
        maps, source_sets = [], []
        for index in range(2, len(children), 2):
            selector, branch = children[index:index + 2]
            if not isinstance(selector, tuple) or len(selector) != 1 or _index(selector[0]) != 0:
                raise _Reject('switch_selector')
            args, bodies = _lambda(branch)
            if len(args) != 1 or len(bodies) != 1:
                raise _Reject('switch_branch')
            mapping = bodies[0]
            if _head(mapping) not in ('Map', 'OrderedMap') or len(mapping.children) != 2 or mapping.children[0] is not args[0]:
                raise _Reject('switch_projection')
            rows, structures = _lambda(mapping.children[1])
            if len(rows) != 1 or len(structures) != 1 or _head(structures[0]) != 'AsStruct':
                raise _Reject('switch_struct')
            fields, sources = {}, set()
            for pair in structures[0].children:
                if not isinstance(pair, tuple) or len(pair) != 2:
                    raise _Reject('switch_field')
                output, member = _atom(pair[0]), pair[1]
                if _head(member) != 'Member' or len(member.children) != 2 or member.children[0] is not rows[0]:
                    raise _Reject('switch_computation')
                source = _atom(member.children[1])
                if output in fields or source in sources:
                    raise _Reject('switch_nonbijective')
                fields[output] = _Field(stage, len(maps), source)
                sources.add(source)
            if not sources:
                raise _Reject('empty_switch_projection')
            maps.append(_Stream('row', fields))
            source_sets.append(frozenset(sources))
        if not 2 <= len(maps) <= 16 or any(value != source_sets[0] for value in source_sets):
            raise _Reject('switch_port_fields')
        self.hubs[stage] = source_sets[0]
        return tuple(maps)

    def _stage(self, stage):
        if stage not in self.stages:
            raise _Reject('stage_reference')
        if stage in self.cache:
            return self.cache[stage]
        if stage in self.active:
            raise _Reject('stage_cycle')
        self.active.add(stage)
        if len(stage.children) < 2 or not isinstance(stage.children[0], tuple):
            raise _Reject('stage_shape')
        args, bodies = _lambda(stage.children[1])
        if len(args) != len(stage.children[0]) or len(bodies) != 1:
            raise _Reject('stage_program')
        result = self._hub(stage, bodies[0])
        if result is None:
            env = dict(zip(args, (self._connection(item) for item in stage.children[0])))
            result = (self._stream(bodies[0], env),)
        self.active.remove(stage)
        self.cache[stage] = result
        return result

    @staticmethod
    def _wide(stream):
        if stream.layout != 'wide' or not isinstance(stream.values, tuple):
            raise _Reject('wide_input')
        return stream.values

    def _domain(self, node, env):
        if len(node.children) != 6:
            raise _Reject('combine_arity')
        source = self._wide(self._stream(node.children[0], env))
        keys, init, update, finish = (_lambda(value) for value in node.children[2:])
        key_args, key_bodies = keys
        key_count = len(key_bodies)
        if not key_count or len(key_args) != len(source) or any(body not in key_args for body in key_bodies):
            return _Stream()
        if len(set(key_bodies)) != key_count:
            return _Stream()
        if init[1] or update[1] or len(init[0]) != key_count + len(source) or len(update[0]) != key_count + len(source):
            return _Stream()
        if len(finish[0]) != key_count or len(finish[1]) != key_count or set(finish[1]) != set(finish[0]):
            return _Stream()
        values = tuple(source[key_args.index(body)] for body in key_bodies)
        tagged = tuple(replace(value, domains=value.domains | {node}) if isinstance(value, _Field) else _UNKNOWN
                       for value in values)
        self.domains[node] = tagged
        return _Stream('wide', tuple(tagged[finish[0].index(body)] for body in finish[1]))

    def _join(self, node, env):
        if len(node.children) != 10 or node.children[9] != ():
            raise _Reject('grace_shape')
        left = self._wide(self._stream(node.children[0], env))
        right = self._wide(self._stream(node.children[1], env))
        kind = _atom(node.children[2])
        if kind not in ('Inner', 'Left'):
            raise _Reject('join_kind')
        key_values = []
        for columns, source in zip(node.children[3:5], (left, right)):
            if not isinstance(columns, tuple):
                raise _Reject('join_key_list')
            positions = tuple(_index(value) for value in columns)
            if any(index >= len(source) for index in positions):
                raise _Reject('join_key_position')
            key_values.append(tuple(source[index] for index in positions))
        if len(key_values[0]) != len(key_values[1]):
            raise _Reject('join_key_arity')
        outputs = {}
        for side, (renames, source) in enumerate(zip(node.children[5:7], (left, right))):
            if not isinstance(renames, tuple) or len(renames) % 2:
                raise _Reject('join_rename_list')
            for offset in range(0, len(renames), 2):
                old, new = (_index(value) for value in renames[offset:offset + 2])
                if old >= len(source) or new in outputs:
                    raise _Reject('join_rename_position')
                value = source[old]
                if side == 1 and kind == 'Left':
                    value = _UNKNOWN
                elif isinstance(value, _Field):
                    value = replace(value, joins=value.joins | {(node, side)})
                outputs[new] = value
        if set(outputs) != set(range(len(outputs))):
            raise _Reject('join_output_layout')
        self.joins[node] = (kind, left, right, *key_values)
        return _Stream('wide', tuple(outputs[index] for index in range(len(outputs))))

    def _stream(self, node, env):
        if isinstance(node, _Arg):
            value = env.get(node)
            return value if isinstance(value, _Stream) else _Stream()
        head = _head(node)
        if head in _FLOW and len(node.children) == 1:
            return self._stream(node.children[0], env)
        if head == 'DqPhyHashCombine':
            return self._domain(node, env)
        if head == 'GraceJoinCore':
            return self._join(node, env)
        if head in ('Map', 'OrderedMap', 'ExpandMap', 'WideMap', 'NarrowMap') and len(node.children) == 2:
            source = self._stream(node.children[0], env)
            if head in ('WideMap', 'NarrowMap'):
                values = self._wide(source)
            else:
                if source.layout not in ('row', 'unknown'):
                    raise _Reject('narrow_input')
                values = (source.values if source.layout == 'row' else _UNKNOWN,)
            results = self._apply(node.children[1], values, env)
            if head in ('ExpandMap', 'WideMap'):
                return _Stream('wide', results)
            if len(results) != 1 or not isinstance(results[0], dict):
                return _Stream()
            return _Stream('row', results[0])
        if head in ('Filter', 'OrderedFilter', 'WideFilter') and len(node.children) == 2:
            source = self._stream(node.children[0], env)
            args, bodies = _lambda(node.children[1])
            width = len(self._wide(source)) if head == 'WideFilter' else 1
            if len(args) != width or len(bodies) != 1:
                raise _Reject('filter_arity')
            return source
        if head == 'ExtractMembers' and len(node.children) == 2:
            source = self._stream(node.children[0], env)
            if source.layout != 'row' or not isinstance(node.children[1], tuple):
                return _Stream()
            names = tuple(_atom(value) for value in node.children[1])
            if len(set(names)) != len(names):
                raise _Reject('projection_fields')
            return _Stream('row', {name: source.values.get(name, _UNKNOWN) for name in names})
        if head in ('SkipNullMembers', 'FilterNullMembers') and len(node.children) == 2:
            source = self._stream(node.children[0], env)
            if source.layout != 'row' or not isinstance(node.children[1], tuple):
                return _Stream()
            fields = dict(source.values)
            # These operators may unwrap optional fields. Preserve only fields
            # whose representation they do not affect.
            for name in node.children[1]:
                fields[_atom(name)] = _UNKNOWN
            return _Stream('row', fields)
        if head is not None:
            self.unknown_calls.add(head)
        return _Stream()

    def verify(self, case):
        actual_joins = {node for node in self.graph.nodes if node.head in _JOIN_NAMES}
        if len(actual_joins) != 2 or any(node.head != 'GraceJoinCore' for node in actual_joins):
            raise _Reject('supported_join_inventory')
        # Start at the actual join programs. A terminal shared hub is the
        # intentional provenance boundary; evaluating every upstream producer
        # would require unrelated reduction semantics and defeat that boundary.
        for stage in self.stages:
            if len(stage.children) < 2:
                raise _Reject('stage_shape')
            pending, seen, local_joins = [stage.children[1]], set(), set()
            seen_tuples = set()
            while pending:
                value = pending.pop()
                if isinstance(value, _Node):
                    if value in seen or value.head == 'DqPhyStage':
                        continue
                    seen.add(value)
                    if value in actual_joins:
                        local_joins.add(value)
                    pending.extend(value.children)
                elif isinstance(value, tuple):
                    identity = id(value)
                    if identity in seen_tuples:
                        continue
                    seen_tuples.add(identity)
                    pending.extend(value)
            if local_joins:
                self._stage(stage)
        if set(self.joins) != actual_joins:
            raise _Reject('untraced_join')
        inners = [node for node, value in self.joins.items() if value[0] == 'Inner']
        outers = [node for node, value in self.joins.items() if value[0] == 'Left']
        if len(inners) != 1 or len(outers) != 1:
            raise _Reject('reconstruction_join_inventory')
        inner, outer = inners[0], outers[0]
        left_keys, right_keys = self.joins[outer][3:]
        if not left_keys or not all(isinstance(value, _Field) for value in (*left_keys, *right_keys)):
            raise _Reject('identity_field_unknown')
        hub, left_port, right_port = left_keys[0].hub, left_keys[0].port, right_keys[0].port
        if left_port == right_port or hub not in self.hubs:
            raise _Reject('distinct_shared_ports')
        fields = self.hubs[hub]
        if len(left_keys) != len(fields) or {value.source for value in left_keys} != fields:
            raise _Reject('complete_identity_coverage')
        for left, right in zip(left_keys, right_keys):
            if (left.hub is not hub or right.hub is not hub or left.port != left_port or right.port != right_port
                    or left.source != right.source or left.codecs != right.codecs
                    or left.codecs not in ((), ('StablePickle',)) or left.domains
                    or (inner, 0) not in right.joins
                    or (inner, 0) in left.joins or (inner, 1) in left.joins):
                raise _Reject('identity_pair_binding')
        encoded = sum(bool(value.codecs) for value in left_keys)
        if encoded != (1 if case.startswith('c31_stability_float_') else 0):
            raise _Reject('profile_codec_coverage')
        inner_values = [value for value in self.joins[inner][1] if isinstance(value, _Field)
                        and value.hub is hub and value.port == right_port and not value.codecs]
        if {value.source for value in inner_values} != fields:
            raise _Reject('inner_domain_field_coverage')
        common = set.intersection(*(set(value.domains) for value in (*inner_values, *right_keys)))
        complete_domains = [domain for domain in common if
                            len(self.domains[domain]) == len(fields)
                            and all(isinstance(value, _Field) and value.hub is hub
                                    and value.port == right_port and not value.codecs
                                    for value in self.domains[domain])
                            and {value.source for value in self.domains[domain]} == fields]
        if not complete_domains:
            raise _Reject('complete_key_only_domain')
        return {'candidate_complete_key_pair_lineage_observed': True,
                'candidate_identity_field_count': len(fields),
                'candidate_matching_codec_pair_count': encoded,
                'candidate_complete_domain_count': len(complete_domains),
                'candidate_stages_traced': len(self.cache),
                'candidate_distinct_shared_ports': True,
                'candidate_inner_output_binding': True}


def check_shared_fields(case, text):
    """Return only sanitized observations; acceptance remains unchanged."""
    result = {'shared_field_probe_version': VERSION,
              'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
              'candidate_complete_key_pair_lineage_observed': False,
              'exact_shared_producer_origin_proved': False,
              'aggregation_value_to_identity_binding_proved': False,
              'full_shared_producer_contract_pass': False,
              'source_exact_scalar_domain_precondition_required': True,
              'upstream_aggregate_field_coverage_still_required': True,
              'actual_graph_and_independent_review_still_required': True,
              'raw_graph_material_persisted': False}
    if case not in CASES:
        result['scope_supported'] = False
        return result
    result['scope_supported'] = True
    try:
        if len(text.encode()) > 16 * 1024 * 1024:
            raise _Reject('graph_budget')
        graph = _LexicalGraph(text)
        result.update(lexical_graph_normalized=True,
                      lexical_block_count=graph.block_count,
                      normalized_node_count=len(graph.nodes))
        result.update(_Proof(graph).verify(case))
    except _Reject as error:
        # Every _Reject message is an internal constant, never raw material.
        result['field_probe_rejected_shape'] = str(error)
    except (ValueError, TypeError, IndexError, KeyError, RecursionError, AttributeError):
        result['field_probe_rejected_shape'] = 'unclassified_shape'
    return result
