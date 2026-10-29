"""Closed, local window dataflow observations; raw ASTs remain in memory.

An unknown final lowering is pending, never a successful count-based fallback.
This observer proves a local producer/collector/field contract, not general AST
equivalence or an amortized runtime bound.
"""
import hashlib

import quality_window_checks as windows

VERSION = 'q01_concrete_window_path_v1'
_STREAM_CHILD = frozenset(('FromFlow', 'ToFlow', 'ToStream', 'Iterator',
                          'Map', 'OrderedMap', 'ExpandMap', 'NarrowMap', 'Chain1Map'))


def _inspect(spec, resolve, nodes):
    def head(node):
        node = resolve(node)
        return node[0] if isinstance(node, list) and node and isinstance(node[0], str) else None

    def atom(node):
        node = resolve(node)
        if not isinstance(node, str):
            raise ValueError('non_atom_field')
        return node.strip("'\"")

    def stream_reaches(node, target):
        seen = set()
        while isinstance(node := resolve(node), list) and id(node) not in seen:
            if node is target:
                return True
            seen.add(id(node))
            if head(node) not in _STREAM_CHILD or len(node) < 2:
                return False
            node = node[1]
        return False

    def descendants(node):
        seen, result, todo = set(), [], [node]
        while todo:
            value = resolve(todo.pop())
            if not isinstance(value, list) or id(value) in seen:
                continue
            seen.add(id(value))
            result.append(value)
            todo.extend(value[1:] if head(value) else value)
        return result

    def frame_queue(node):
        node = resolve(node)
        if head(node) == 'WinFrame' and len(node) == 7:
            return resolve(node[1])
        return None

    def member_outputs(map_node, names):
        # Only direct output members owned by this map's own row argument.
        # Nested lambdas and arbitrary expressions are deliberately excluded.
        if head(map_node) not in ('Map', 'OrderedMap', 'ExpandMap', 'NarrowMap') or len(map_node) != 3:
            return False
        lam = resolve(map_node[2])
        if head(lam) != 'lambda' or len(lam) < 3:
            return False
        args = resolve(lam[1])
        if not isinstance(args, list) or len(args) != 1 or not isinstance(args[0], str):
            return False
        row = args[0]
        result = set()
        todo = list(lam[2:])
        while todo:
            value = resolve(todo.pop())
            if not isinstance(value, list):
                continue
            if head(value) == 'Member' and len(value) == 3 and value[1] == row:
                result.add(atom(value[2]))
            elif head(value) == 'AsStruct':
                for pair in value[1:]:
                    pair = resolve(pair)
                    if not isinstance(pair, list) or len(pair) != 2:
                        return False
                    todo.append(pair[1])
            elif head(value) is None:
                todo.extend(value)
        return names <= result

    def prefix_fields(map_node, queue):
        """Identify direct outputs using two endpoints of this collector queue.

        This recognizes the source builder's local arithmetic shape. It is not
        a proof that every later primitive rewrite preserves the arithmetic.
        Unknown peephole shapes are pending until the actual pilot is reviewed.
        """
        if head(map_node) not in ('Map', 'OrderedMap', 'ExpandMap', 'NarrowMap') or len(map_node) != 3:
            return set()
        lam = resolve(map_node[2])
        if head(lam) != 'lambda' or len(lam) < 3:
            return set()
        args = resolve(lam[1])
        if not isinstance(args, list) or len(args) != 1:
            return set()
        output = []
        for value in lam[2:]:
            value = resolve(value)
            if head(value) == 'AsStruct':
                output.extend(resolve(pair)[1] for pair in value[1:]
                              if isinstance(resolve(pair), list) and len(resolve(pair)) == 2)
            else:
                output.append(value)
        fields = set()
        for expression in output:
            expression = resolve(expression)
            if head(expression) != 'IfPresent' or len(expression) != 5:
                continue
            first, last, arithmetic = map(resolve, expression[1:4])
            if head(first) != 'Head' or len(first) != 2:
                continue
            first = resolve(first[1])
            if frame_queue(first) is not queue or frame_queue(last) is not queue:
                continue
            if head(first[5]) != 'Bool' or head(last[5]) != 'Bool':
                continue
            if atom(resolve(first[5])[1]) != 'false' or atom(resolve(last[5])[1]) != 'true':
                continue
            if any(resolve(first[i]) != resolve(last[i]) for i in (1, 2, 3, 4, 6)):
                continue
            dependency = resolve(first[6])
            if head(dependency) != 'DependsOn' or len(dependency) != 2 or dependency[1] != args[0]:
                continue
            if head(arithmetic) != 'lambda' or len(arithmetic) != 3:
                continue
            endpoint_args = resolve(arithmetic[1])
            if not isinstance(endpoint_args, list) or len(endpoint_args) != 2:
                continue
            first_arg, last_arg = endpoint_args
            def endpoint_member(value, owner):
                value = resolve(value)
                if head(value) == 'Coalesce' and len(value) == 3:
                    value = resolve(value[1])
                return atom(value[2]) if head(value) == 'Member' and len(value) == 3 and value[1] == owner else None
            output_fields = set()
            for value in descendants(arithmetic[2]):
                if head(value) != '+' or len(value) != 3:
                    continue
                difference = resolve(value[1])
                if head(difference) != '-' or len(difference) != 3:
                    continue
                left = endpoint_member(difference[1], last_arg)
                right = endpoint_member(difference[2], first_arg)
                if left is not None and left == right:
                    output_fields.add(left)
            if output_fields:
                fields.add(frozenset(output_fields))
        return fields

    collectors = [n for n in nodes if head(n) == 'WinFramesCollector']
    ranges = [n for n in nodes if head(n) == 'KqpRangeMinMax']
    queues = [n for n in nodes if head(n) == 'QueueCreate']
    if spec['family'] == 'setup_scan':
        return {'setup_has_no_window_pipeline': not collectors and not ranges and not queues}, 'setup'
    if len(collectors) > 1 or len(ranges) > 1:
        raise ValueError('multiple_window_streams')
    fallback = bool(spec.get('fallback'))
    is_range_minmax = spec['kind'] == 'range' and spec['minmax']
    checks = {}
    if ranges:
        node = ranges[0]
        if not is_range_minmax or len(node) != 4:
            raise ValueError('unexpected_range_operator')
        mappings = resolve(node[3])
        if not isinstance(mappings, list) or len(mappings) != 2:
            raise ValueError('range_mapping_arity')
        decoded = []
        for mapping in mappings:
            mapping = resolve(mapping)
            if not isinstance(mapping, list) or len(mapping) != 3:
                raise ValueError('range_mapping_shape')
            decoded.append(tuple(atom(value) for value in mapping))
        outputs = {row[1] for row in decoded}
        checks['two_distinct_result_fields'] = len(outputs) == 2
        checks['same_input_and_minmax_operations'] = (len({row[0] for row in decoded}) == 1
                                                     and {row[2] for row in decoded} == {'min', 'max'})
        mixed = bool(spec['collectors'] or fallback)
        checks['collector_branch_matches_recipe'] = len(collectors) == len(queues) == int(mixed)
        target = node
        if mixed:
            if len(collectors) != 1 or len(collectors[0]) != 4:
                raise ValueError('mixed_collector_shape')
            target = collectors[0]
            checks['same_stream_prepass_to_collector'] = stream_reaches(target[1], node)
            checks['same_bound_object'] = resolve(target[3]) is resolve(node[2])
        checks['result_members_owned_by_same_stream'] = any(
            len(n) >= 2 and stream_reaches(n[1], target) and member_outputs(n, outputs)
            for n in nodes if head(n) in ('Map', 'OrderedMap', 'ExpandMap', 'NarrowMap'))
        route = 'fixed_range_mixed' if mixed else 'fixed_range_only'
    else:
        if len(collectors) != 1:
            raise ValueError('collector_not_unique')
        route = 'baseline_general' if any(head(n) == 'Fold1' for n in nodes) else 'fixed_existing_primitives'
        if is_range_minmax and not any(head(n) == 'Fold1' for n in nodes):
            raise ValueError('range_optimization_path_absent')
    if collectors:
        collector = collectors[0]
        if len(collector) != 4 or head(collector[2]) != 'QueueCreate':
            raise ValueError('collector_queue_unresolved')
        queue = resolve(collector[2])
        frames = [n for n in nodes if frame_queue(n) is queue]
        frame_folds = [n for n in nodes if head(n) == 'Fold1' and len(n) == 4
                       and frame_queue(n[1]) is queue]
        checks['collector_owns_frame_views'] = bool(frames)
        chains = [n for n in nodes if head(n) == 'Chain1Map' and len(n) == 4
                  and stream_reaches(n[1], collector)]
        def stack_lambda(lam, update):
            lam = resolve(lam)
            if head(lam) != 'lambda' or len(lam) != 3 or len(resolve(lam[1])) != (2 if update else 1):
                return False
            folds = [n for n in descendants(lam[2]) if head(n) == 'Fold' and len(n) == 4
                     and any(head(x) == 'Prepend' for x in descendants(n[3]))]
            entering = [n for n in folds if (frame_queue(n[1]) is queue if not update else
                        head(n[1]) == 'If' and any(frame_queue(x) is queue for x in descendants(n[1])))]
            transfers = [n for n in folds if head(n[1]) == 'Nth'
                         and not any(frame_queue(x) is queue for x in descendants(n[1]))]
            return len(entering) == len(transfers) == 2
        stacks = [chain for chain in chains if stack_lambda(chain[2], False) and stack_lambda(chain[3], True)]
        if ranges:
            checks['same_queue_fallback_policy'] = bool(frame_folds) == fallback
        elif spec['kind'] == 'rows' and spec['minmax'] and stacks:
            route = 'fixed_rows_stacks'
            checks['same_stream_two_stack_chain'] = True
            checks['same_queue_fallback_policy'] = bool(frame_folds) == fallback
        elif frame_folds:
            route = 'baseline_general'
            checks['baseline_same_queue_frame_fold'] = True
        else:
            checks['eligible_frame_fold_removed'] = not any(head(n) == 'Fold1' for n in nodes)
            if spec['minmax']:
                checks['same_stream_two_stack_chain'] = False
        if fallback and not frame_folds:
            checks['fallback_retained'] = False
        if spec['collectors'] and route != 'baseline_general':
            checks['same_queue_prefix_endpoint_outputs'] = any(
                len(n) >= 2 and stream_reaches(n[1], collector) and len(prefix_fields(n, queue)) >= 3
                for n in nodes if head(n) in ('Map', 'OrderedMap', 'ExpandMap', 'NarrowMap'))
    return checks, route


def observe(spec, text):
    result = {'schema_version': VERSION, 'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
              'complete': False, 'path_pass': False, 'checks': {}, 'route': 'unrecognized',
              'resource_scaling_verified': False, 'optimization_acceptance_pass': False}
    try:
        resolve, nodes = windows.parse_physical_graph(text)
        checks, route = _inspect(spec, resolve, nodes)
        result.update(complete=True, checks=checks, route=route,
                      path_pass=bool(checks) and all(value is True for value in checks.values()))
    except (ValueError, KeyError, IndexError, TypeError, RecursionError):
        result['reason'] = 'unsupported_or_incomplete_window_dataflow'
    return result
