"""Conservative anonymous C31 stage/port probe; never saves plan material.

This closes only physical shared-stage origin. Aggregate-value column lineage
remains a separate requirement and is deliberately never asserted here.
"""
import hashlib
import json

import quality_window_checks as windows

VERSION = 'c31_shared_stage_origin_v2'
_JOINS = {'MapJoinCore': 3, 'GraceJoinCore': 3, 'BlockHashJoinCore': 3}
_CONNECTIONS = frozenset(('DqCnMap', 'DqCnHashShuffle', 'DqCnBroadcast',
                         'DqCnUnionAll', 'DqCnParallelUnionAll', 'DqCnMerge'))
_WRAPPERS = frozenset(('ToFlow', 'FromFlow', 'Iterator', 'Collect', 'ToStream'))


def _head(node):
    return node[0] if isinstance(node, list) and node and isinstance(node[0], str) else None


def _atom(node, resolve):
    node = resolve(node)
    if not isinstance(node, str):
        raise ValueError('Expected atom')
    node = node[1:] if node.startswith("'") else node
    return json.loads(node) if node.startswith('"') else node


def _lambda_bodies(node, resolve):
    node = resolve(node)
    if _head(node) != 'lambda' or len(node) < 2:
        raise ValueError('Expected lambda')
    args = resolve(node[1])
    if not isinstance(args, list) or any(not isinstance(arg, str) for arg in args):
        raise ValueError('Expected lambda arguments')
    if len(set(args)) != len(args):
        raise ValueError('Duplicate lambda argument')
    return args, node[2:]


def _lambda(node, resolve):
    args, bodies = _lambda_bodies(node, resolve)
    if len(bodies) != 1:
        raise ValueError('Expected single-body lambda')
    return args, bodies[0]


def _dependencies(node, env, resolve):
    if isinstance(node, str) and node in env:
        return env[node]
    node = resolve(node)
    if not isinstance(node, list):
        return frozenset()
    if _head(node) == 'DqPhyStage':
        raise ValueError('Nested stage in program')
    if _head(node) == 'lambda':
        args, bodies = _lambda_bodies(node, resolve)
        nested = dict(env, **{arg: frozenset() for arg in args})
        return frozenset().union(*(_dependencies(body, nested, resolve) for body in bodies))
    children = node[1:] if _head(node) and not _head(node).startswith(("'", '"', '$')) else node
    return frozenset().union(*(_dependencies(child, env, resolve) for child in children))


def _program_nodes(node, env, resolve):
    node = resolve(node)
    if not isinstance(node, list):
        return
    yield node, env
    if _head(node) == 'DqPhyStage':
        raise ValueError('Nested stage in program')
    children = node[1:] if _head(node) and not _head(node).startswith(("'", '"', '$')) else node
    inputs = [child for child in children if _head(resolve(child)) != 'lambda']
    dependencies = frozenset().union(*(_dependencies(child, env, resolve) for child in inputs))
    for child in children:
        if _head(resolve(child)) == 'lambda':
            args, bodies = _lambda_bodies(child, resolve)
            nested = dict(env, **{arg: dependencies for arg in args})
            for body in bodies:
                yield from _program_nodes(body, nested, resolve)
        else:
            yield from _program_nodes(child, env, resolve)


def _rename_projection(branch, resolve):
    args, body = _lambda(branch, resolve)
    body = resolve(body)
    if len(args) != 1 or _head(body) not in ('Map', 'OrderedMap') or len(body) != 3 or body[1] != args[0]:
        raise ValueError('Non-projection shared branch')
    rows, structure = _lambda(body[2], resolve)
    structure = resolve(structure)
    if len(rows) != 1 or _head(structure) != 'AsStruct':
        raise ValueError('Non-struct shared branch')
    output_names, source_names = set(), set()
    for pair in structure[1:]:
        pair = resolve(pair)
        if not isinstance(pair, list) or len(pair) != 2:
            raise ValueError('Malformed shared output field')
        member = resolve(pair[1])
        if _head(member) != 'Member' or len(member) != 3 or member[1] != rows[0]:
            raise ValueError('Computed shared output field')
        output = _atom(pair[0], resolve)
        source = _atom(member[2], resolve)
        if output in output_names or source in source_names:
            raise ValueError('Non-bijective shared projection')
        output_names.add(output)
        source_names.add(source)
    if not source_names:
        raise ValueError('Empty shared projection')
    return source_names


def _terminal_shared_switch(body, resolve):
    body = resolve(body)
    while _head(body) in _WRAPPERS and len(body) == 2:
        body = resolve(body[1])
    if _head(body) != 'Switch' or len(body) < 7 or (len(body) - 3) % 2:
        return None
    sources = []
    for index in range(3, len(body), 2):
        selector = resolve(body[index])
        if not isinstance(selector, list) or len(selector) != 1 or _atom(selector[0], resolve) != '0':
            return None
        sources.append(_rename_projection(body[index + 1], resolve))
    if any(source != sources[0] for source in sources):
        return None
    return {'ports': len(sources), 'field_count': len(sources[0]), 'input': body[1]}


def check_shared_origin(case, text):
    """Return hashes/counters/booleans only; do not grant complete admission."""
    result = {'shared_origin_probe_version': VERSION,
              'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
              'reconstruction_stage_port_topology_observed': False,
              'exact_shared_producer_origin_proved': False,
              'aggregation_value_to_identity_binding_proved': False,
              'full_shared_producer_contract_pass': False}
    if not case.startswith('c31_stability_') or not case.endswith('_domain__spilling__left'):
        result['scope_supported'] = False
        return result
    result['scope_supported'] = True
    try:
        resolve, nodes = windows.parse_physical_graph(text)
        stages = {id(node): node for node in nodes if _head(node) == 'DqPhyStage'}
        if not 1 <= len(stages) <= 128:
            raise ValueError('Unsupported stage graph size')
        programs, connections, hubs, joins = {}, {}, {}, []
        for identity, stage in stages.items():
            inputs = resolve(stage[1])
            args, body = _lambda(stage[2], resolve)
            if not isinstance(inputs, list) or len(args) != len(inputs):
                raise ValueError('Stage argument/input mismatch')
            env = {arg: frozenset((index,)) for index, arg in enumerate(args)}
            programs[identity] = _dependencies(body, env, resolve)
            connections[identity] = []
            for item in inputs:
                item = resolve(item)
                connection = None
                if _head(item) in _CONNECTIONS:
                    output = resolve(item[1])
                    if _head(output) != 'TDqOutput' or len(output) != 3:
                        raise ValueError('Unrecognized stage output')
                    producer = resolve(output[1])
                    port = _atom(output[2], resolve)
                    if id(producer) not in stages or not port.isdigit():
                        raise ValueError('Unrecognized producer port')
                    connection = (id(producer), int(port))
                connections[identity].append(connection)
            hubs[identity] = _terminal_shared_switch(body, resolve)
            local = list(_program_nodes(body, env, resolve))
            for node, nested in local:
                if _head(node) in _JOINS:
                    kind = _atom(node[_JOINS[node[0]]], resolve)
                    joins.append((identity, kind, _dependencies(node[1], nested, resolve),
                                  _dependencies(node[2], nested, resolve)))

        def first(stage, dependencies):
            if len(dependencies) != 1:
                return None
            return connections[stage][next(iter(dependencies))]

        def unary_path(connection):
            path, seen = [], set()
            while connection is not None:
                stage, port = connection
                if (stage, port) in seen:
                    raise ValueError('Cyclic stage path')
                seen.add((stage, port))
                path.append((stage, port))
                if hubs[stage] is not None:
                    break
                connection = first(stage, programs[stage])
            return path

        inner = [join for join in joins if join[1] == 'Inner']
        outer = [join for join in joins if join[1] == 'Left']
        result.update(stage_count=len(stages), recognized_join_count=len(joins),
                      terminal_identity_switch_count=sum(hub is not None for hub in hubs.values()))
        if len(joins) != 2 or len(inner) != 1 or len(outer) != 1:
            return result
        inner, outer = inner[0], outer[0]
        paths = [unary_path(first(join[0], join[2])) for join in (inner, outer)]
        right_path = unary_path(first(outer[0], outer[3]))
        if not all(paths) or inner[0] not in {stage for stage, _ in right_path}:
            return result
        (producer, inner_port), (other, outer_port) = (path[-1] for path in paths)
        hub = hubs[producer]
        exact = (producer == other and inner_port != outer_port and hub is not None
                 and max(inner_port, outer_port) < hub['ports'])
        result['reconstruction_stage_port_topology_observed'] = bool(exact)
        if exact:
            result.update(shared_port_count=hub['ports'], shared_projection_field_count=hub['field_count'],
                          shared_branches_are_bijective_member_projections=True,
                          inner_and_outer_left_paths_have_single_stage_input_dependencies=True,
                          outer_right_path_contains_inner_join=True,
                          aggregation_value_lineage_still_required=True)
    except (ValueError, TypeError, IndexError, KeyError, RecursionError) as error:
        result['probe_rejected_shape'] = type(error).__name__
    return result
