"""Q14 four-node SQL builders and join-bound connection observations.

This module does not grant whole-finding acceptance. The twelve direct CBO
identity contracts remain separate; unexpected SQL/operator shapes stay pending.
"""
from collections import Counter
import hashlib
import json
import re

import generate_and_verify as g
from quality_window_checks import parse_physical_graph

CASE_SPECIFIC_SCHEMA = True
VERSION = 'q14_join_bound_stage_edges_v2'


def case_specs():
    return {**{'q14_' + algorithm + '_chain_' + enabled: (algorithm, True, enabled == 'on')
               for algorithm in ('reverse', 'grace') for enabled in ('on', 'off')},
            **{'q14_reverse_single_' + enabled + '_control': ('reverse', False, enabled == 'on')
               for enabled in ('on', 'off')}}


def _size(n):
    if type(n) is not int or not 8 <= n <= 128:
        raise ValueError('Q14 observation row count outside the bounded range')


def _rows(n, side):
    _size(n)
    multiplier, modulus = {'a': (1, 7), 'b': (3, 11), 'c': (5, 13)}[side]
    return [{'id': i, 'k': (i * multiplier) % modulus, 'p': i % 3} for i in range(1, n + 1)]


def _predicate(left, right):
    return g.op(g.op(left + '.k', '=', right + '.k'), 'AND', g.op(left + '.p', '=', right + '.p'))


def recipes(n):
    _size(n)
    result = {}
    for case, (_, chain, _) in case_specs().items():
        source = g.join('qa AS a', 'qb AS b', _predicate('a', 'b'), 'LEFT')
        fields = [g.alias('a.id', 'ai'), g.alias('b.id', 'bi')]
        if chain:
            source = g.join(source, 'qc AS c', _predicate('a', 'c'), 'LEFT')
            fields.append(g.alias('c.id', 'ci'))
        result[case] = g.select(fields, source)
    return result


def schema_and_seeds(n, case):
    _size(n)
    _, chain, _ = case_specs()[case]
    schemas, seeds = [], []
    for side in ('a', 'b', 'c') if chain else ('a', 'b'):
        table = 'q' + side
        fields = ['id Uint64 NOT NULL', 'k Int64 NOT NULL', 'p Int64 NOT NULL', 'PRIMARY KEY (id)']
        schemas.append('CREATE TABLE ' + table + ' (' + ', '.join(fields) + ') WITH (UNIFORM_PARTITIONS = 4);')
        values = [g.select([g.alias(g.typed('Uint64' if column == 'id' else 'Int64', row[column]), column)
                            for column in ('id', 'k', 'p')]) for row in _rows(n, side)]
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(values))
    return ''.join(schemas), seeds


def metadata(case, n):
    _size(n)
    algorithm, chain, enabled = case_specs()[case]
    hints = ['JoinOrder(' + ('(a b) c' if chain else 'a b') + ')']
    for aliases, rows in ((['a'], 100), (['b'], 1000000), (['c'], 1000000), (['a', 'b'], 100)):
        if not chain and ('c' in aliases or len(aliases) > 1):
            continue
        for name, value in (('Rows', rows), ('Bytes', rows * 24)):
            hints.append(name + '(' + ' '.join(aliases) + ' # ' + str(value) + ')')
    if algorithm == 'grace':
        hints += ['JoinType(a b Shuffle)', 'JoinType(a b c Shuffle)']
    return {'generator_version': VERSION, 'node_count': 4, 'peak_native_concurrency': 1,
            'pragmas': [('kikimr.CostBasedOptimizationLevel', '4'), ('kikimr.HashJoinMode', 'grace'),
                        ('kikimr.OptShuffleElimination', 'true' if enabled else 'false'),
                        ('ydb.OptimizerHints', ' '.join(hints))],
            'table_service_overrides': {'EnableFallbackToYqlOptimizer': False, 'UseBlockHashJoin': True,
                'EnableKqpDataQueryStreamIdxLookupJoin': False, 'DqHashOperatorsUseBlocks': False,
                'DefaultEnableShuffleElimination': enabled, 'EnableQueryServiceSpilling': True,
                'EnableSpillingInHashJoinShuffleConnections': True, 'EnableSpillingNodes': 'All'},
            'quality_requires_separate_plan_evidence': True}


def expected_rows(case, n):
    _, chain, _ = case_specs()[case]
    result = []
    for left in _rows(n, 'a'):
        matches = lambda side: [row for row in _rows(n, side)
                                if (row['k'], row['p']) == (left['k'], left['p'])]
        for right in matches('b') or [None]:
            item = {'ai': left['id'], 'bi': right['id'] if right else None}
            if chain:
                result.extend(dict(item, ci=other['id'] if other else None)
                              for other in matches('c') or [None])
            else:
                result.append(item)
    return result


def check_rows(case, rows, n):
    canonical = lambda values: Counter(json.dumps(row, sort_keys=True) for row in values)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_four_node_bag_oracle': True, 'quality_improvement_verified': False}


def _head(node):
    return node[0] if isinstance(node, list) and node and isinstance(node[0], str) else None


def _atom(node, resolve):
    node = resolve(node)
    if not isinstance(node, str):
        raise ValueError('Expected a physical atom')
    node = node[1:] if node.startswith("'") else node
    if node.startswith('"'):
        node = json.loads(node)
    if not isinstance(node, str):
        raise ValueError('Expected a string atom')
    return node


def _atoms(node, resolve):
    node = resolve(node)
    if not isinstance(node, list):
        raise ValueError('Expected a physical atom list')
    return [_atom(value, resolve) for value in node]


def _lambda(node, resolve):
    node = resolve(node)
    if _head(node) != 'lambda' or len(node) < 3:
        raise ValueError('Expected a complete lambda')
    args = resolve(node[1])
    if (not isinstance(args, list) or any(not isinstance(value, str) for value in args)
        or len(set(args)) != len(args)):
        raise ValueError('Ambiguous lambda arguments')
    if any(resolve(arg) != arg for arg in args):
        raise ValueError('Lambda argument collides with an expression binding')
    return args, node[2:]


def _deps(node, env, resolve, active=None):
    if isinstance(node, str) and node in env:
        return env[node]
    node = resolve(node)
    if not isinstance(node, list):
        if isinstance(node, str) and node.startswith('$'):
            raise ValueError('Unbound argument or expression alias in a stage program')
        return frozenset()
    active = set() if active is None else active
    if id(node) in active or len(active) > 256 or _head(node) in ('DqPhyStage', 'block', 'let', 'return'):
        raise ValueError('Unsupported nested or cyclic stage program')
    active.add(id(node))
    if _head(node) == 'lambda':
        args, children = _lambda(node, resolve)
        if any(arg in env for arg in args):
            raise ValueError('Shadowed stage or enclosing lambda argument')
        env = dict(env, **{arg: frozenset() for arg in args})
    else:
        children = node[1:] if _head(node) and not _head(node).startswith(("'", '"', '$')) else node
    result = frozenset().union(*(_deps(child, env, resolve, active) for child in children))
    active.remove(id(node))
    return result


def _walk(node, env, resolve, active=None):
    if isinstance(node, str) and node in env:
        return
    node = resolve(node)
    if not isinstance(node, list):
        if isinstance(node, str) and node.startswith('$'):
            raise ValueError('Unbound argument or expression alias in a stage program')
        return
    active = set() if active is None else active
    if id(node) in active or len(active) > 256 or _head(node) in ('DqPhyStage', 'block', 'let', 'return'):
        raise ValueError('Unsupported nested or cyclic stage program')
    active.add(id(node))
    yield node, env
    if _head(node) == 'lambda':
        args, children = _lambda(node, resolve)
        if any(arg in env for arg in args):
            raise ValueError('Shadowed stage or enclosing lambda argument')
        env = dict(env, **{arg: frozenset() for arg in args})
    else:
        children = node[1:] if _head(node) and not _head(node).startswith(("'", '"', '$')) else node
    for child in children:
        yield from _walk(child, env, resolve, active)
    active.remove(id(node))


def _source_side(node, resolve):
    node = resolve(node)
    if _head(node) != 'DqSource' or len(node) != 3:
        raise ValueError('Expected the physical row source')
    settings = resolve(node[2])
    if _head(settings) != 'KqpRowsSourceSettings' or len(settings) not in (5, 6):
        raise ValueError('Unexpected source kind')
    table = resolve(settings[1])
    if _head(table) != 'KqpTable' or len(table) != 5:
        raise ValueError('Unknown source table')
    paths = {'/Root/q' + side: side for side in ('a', 'b', 'c')}
    path = _atom(table[1], resolve)
    if path not in paths or _atom(table[3], resolve) != '':
        raise ValueError('Unexpected source table identity')
    return paths[path]


def _summary(text):
    resolve, nodes = parse_physical_graph(text)
    stages = {id(node): node for node in nodes if _head(node) == 'DqPhyStage'}
    if not 1 <= len(stages) <= 128:
        raise ValueError('Unsupported stage count')
    connections, dependencies, cores = {}, {}, {}
    for sid, stage in stages.items():
        if len(stage) not in (4, 5):
            raise ValueError('Incomplete physical stage')
        if len(stage) == 5 and resolve(stage[4]) != []:
            raise ValueError('Annotated stage outputs are outside this query profile')
        inputs = resolve(stage[1])
        args, bodies = _lambda(stage[2], resolve)
        if len(bodies) != 1 or not isinstance(inputs, list) or len(inputs) != len(args):
            raise ValueError('Ambiguous stage program inputs')
        env = {arg: frozenset((index,)) for index, arg in enumerate(args)}
        dependencies[sid] = _deps(bodies[0], env, resolve)
        connections[sid] = []
        for item in inputs:
            item = resolve(item)
            kind = _head(item)
            if kind == 'DqSource':
                connections[sid].append({'kind': kind, 'table': _source_side(item, resolve)})
                continue
            if kind not in ('DqCnMap', 'DqCnHashShuffle', 'DqCnUnionAll', 'DqCnParallelUnionAll'):
                raise ValueError('Unsupported join observation connection')
            if not (3 <= len(item) <= 5 if kind == 'DqCnHashShuffle' else len(item) == 2):
                raise ValueError('Malformed connection arity')
            output = resolve(item[1])
            if _head(output) != 'TDqOutput' or len(output) != 3 or _atom(output[2], resolve) != '0':
                raise ValueError('Only an unambiguous single output is supported')
            producer = id(resolve(output[1]))
            if producer not in stages:
                raise ValueError('Unknown connection producer')
            keys = _atoms(item[2], resolve) if kind == 'DqCnHashShuffle' and len(item) >= 3 else []
            if kind == 'DqCnHashShuffle' and (not keys or len(keys) != len(set(keys))):
                raise ValueError('Malformed shuffle key list')
            options = [_atom(value, resolve) for value in item[3:]] if kind == 'DqCnHashShuffle' else []
            if options and options[0] not in ('true', 'false'):
                raise ValueError('Unknown shuffle spilling option')
            connections[sid].append({'kind': kind, 'producer': producer, 'keys': keys, 'options': options})
        local = {}
        for node, scope in _walk(bodies[0], env, resolve):
            if _head(node) in ('GraceJoinCore', 'MapJoinCore', 'ScalarHashJoinCore'):
                raise ValueError('The intended block hash join was not selected')
            if _head(node) == 'BlockHashJoinCore':
                if id(node) in local and local[id(node)][1] != scope:
                    raise ValueError('Ambiguous shared join program')
                local[id(node)] = (node, scope)
        if len(local) > 1:
            raise ValueError('Multiple joins inside one stage are not admitted')
        if local:
            cores[sid] = next(iter(local.values()))

    def origins(sid, active=frozenset()):
        if sid in active or len(active) > 128:
            raise ValueError('Cyclic source topology')
        result = set()
        for index in dependencies[sid]:
            edge = connections[sid][index]
            result.update((edge['table'],) if edge['kind'] == 'DqSource' else origins(edge['producer'], active | {sid}))
        return result

    def side(sid, node, scope, keys):
        used = _deps(node, scope, resolve)
        if len(used) != 1:
            raise ValueError('Join side is not bound to exactly one stage input')
        edge = connections[sid][next(iter(used))]
        if edge['kind'] not in ('DqCnMap', 'DqCnHashShuffle'):
            raise ValueError('Join side does not use a distributed connection')
        permutation = []
        if edge['kind'] == 'DqCnHashShuffle':
            if len(edge['keys']) != len(keys) or set(edge['keys']) != set(keys):
                raise ValueError('Shuffle columns do not match the intended join side keys')
            permutation = [keys.index(key) for key in edge['keys']]
        return {'kind': edge['kind'], 'origins': sorted(origins(edge['producer'])),
                'key_permutation': permutation, 'shuffle_options': edge['options'],
                'producer_contains_join': edge['producer'] in cores}, edge['producer']

    result, private = [], {}
    for sid, (node, scope) in cores.items():
        if len(node) not in (9, 12) or _atom(node[3], resolve) != 'Left':
            raise ValueError('The intended LEFT join was not selected')
        for filter_node in node[9:]:
            _, bodies = _lambda(filter_node, resolve)
            if len(bodies) != 1 or resolve(bodies[0]) != ['Void']:
                raise ValueError('Unexpected residual join filter')
        keys = [_atoms(node[index], resolve) for index in (6, 7)]
        indexes = [_atoms(node[index], resolve) for index in (4, 5)]
        if any(len(value) != 2 or len(set(value)) != 2 for value in keys + indexes):
            raise ValueError('Expected two distinct physical join keys')
        if any(not re.fullmatch('[0-9]+', value) for values in indexes for value in values):
            raise ValueError('Unknown block-key column index')
        settings = resolve(node[8])
        if not isinstance(settings, list):
            raise ValueError('Unresolved block-join settings')
        build_side = 'Right'
        seen = set()
        for setting in settings:
            setting = resolve(setting)
            if not isinstance(setting, list) or len(setting) != 2:
                raise ValueError('Unsupported block-join setting')
            name = _atom(setting[0], resolve)
            if name in seen or name != 'BuildSide':
                raise ValueError('Unexpected block-join setting')
            seen.add(name)
            build_side = _atom(setting[1], resolve)
        if build_side not in ('Left', 'Right'):
            raise ValueError('Unknown block build side')
        left, lp = side(sid, node[1], scope, keys[0])
        right, rp = side(sid, node[2], scope, keys[1])
        if (left['kind'] == right['kind'] == 'DqCnHashShuffle'
            and (left['key_permutation'], left['shuffle_options']) != (right['key_permutation'], right['shuffle_options'])):
            raise ValueError('Join input hash-key permutations differ')
        signature = ''.join(left['origins']) + ':' + ''.join(right['origins'])
        if signature in private:
            raise ValueError('Ambiguous intended join identity')
        descriptor = {'signature': signature, 'build_side': build_side, 'left': left, 'right': right,
                      'join_kind': 'Left', 'join_key_count': 2, 'input_binding_checked': True,
                      'shuffle_key_binding_checked': True}
        result.append(descriptor)
        private[signature] = (sid, lp, rp, keys)
    if 'ab:c' in private:
        if 'a:b' not in private or private['ab:c'][1] != private['a:b'][0]:
            raise ValueError('The second join is not directly fed by the intended first join')
        if private['ab:c'][3][0] != private['a:b'][3][0]:
            raise ValueError('The chained LEFT key identities changed')
        named = {item['signature']: item for item in result}
        if (named['ab:c']['left']['kind'] == 'DqCnMap'
            and any(named['ab:c']['right'][key] != named['a:b']['left'][key]
                    for key in ('key_permutation', 'shuffle_options'))):
            raise ValueError('The preserved left distribution does not match the second right shuffle')
    return sorted(result, key=lambda item: item['signature'])


def shape_contract(case, descriptors, fixed):
    if not isinstance(case, str) or case not in case_specs() or type(fixed) is not bool:
        return False
    algorithm, chain, enabled = case_specs()[case]
    signatures = ['a:b', 'ab:c'] if chain else ['a:b']
    if (not isinstance(descriptors, list) or any(not isinstance(value, dict) for value in descriptors)
        or [value.get('signature') for value in descriptors] != signatures):
        return False
    for item in descriptors:
        if (set(item) != {'signature', 'build_side', 'left', 'right', 'join_kind', 'join_key_count',
                         'input_binding_checked', 'shuffle_key_binding_checked'}
            or item.get('build_side') != ('Left' if algorithm == 'reverse' else 'Right')
            or item.get('join_kind') != 'Left' or type(item.get('join_key_count')) is not int
            or item['join_key_count'] != 2 or item.get('input_binding_checked') is not True
            or item.get('shuffle_key_binding_checked') is not True):
            return False
        expected_left = ('DqCnMap' if item['signature'] == 'ab:c' and enabled
                         and (algorithm == 'grace' or fixed) else 'DqCnHashShuffle')
        for name, expected_kind, expected_origins in (
                ('left', expected_left, ['a', 'b'] if item['signature'] == 'ab:c' else ['a']),
                ('right', 'DqCnHashShuffle', ['c'] if item['signature'] == 'ab:c' else ['b'])):
            edge = item.get(name)
            if (not isinstance(edge, dict)
                or set(edge) != {'kind', 'origins', 'key_permutation', 'shuffle_options', 'producer_contains_join'}
                or edge.get('kind') != expected_kind or edge.get('origins') != expected_origins
                or edge.get('producer_contains_join') is not (name == 'left' and item['signature'] == 'ab:c')):
                return False
            options = edge.get('shuffle_options')
            if (not isinstance(options, list) or any(not isinstance(value, str) for value in options)
                or len(options) > 2 or options and options[0] not in ('true', 'false')
                or expected_kind == 'DqCnMap' and options):
                return False
            permutation = edge.get('key_permutation')
            if (not isinstance(permutation, list) or any(type(value) is not int for value in permutation)
                or (permutation != [] if expected_kind == 'DqCnMap' else sorted(permutation) != [0, 1])):
                return False
        if (expected_left == 'DqCnHashShuffle'
            and any(item['left'][key] != item['right'][key] for key in ('key_permutation', 'shuffle_options'))):
            return False
    if (chain and descriptors[1]['left']['kind'] == 'DqCnMap'
        and any(descriptors[1]['right'][key] != descriptors[0]['left'][key]
                for key in ('key_permutation', 'shuffle_options'))):
        return False
    return True


def check_physical_ast(case, text):
    case_specs()[case]
    result = {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
              'q14_stage_observer_version': VERSION, 'physical_ast_parsed': False,
              'quality_improvement_verified': False, 'q14_direct_contracts_still_required': True}
    try:
        descriptors = _summary(text)
        result.update(physical_ast_parsed=True, q14_join_inputs=descriptors,
                      q14_fixed_connection_shape_observed=shape_contract(case, descriptors, True))
    except (ValueError, TypeError, IndexError, KeyError, RecursionError) as error:
        result.update(q14_observation_rejected=type(error).__name__, q14_fixed_connection_shape_observed=False)
    # Keep generic quality acceptance closed until the paired local CBO contracts
    # and all SQL observations are explicitly combined by the series owner.
    result['quality_acceptance_pass'] = False
    return result


def pair_observations(before, after, base, revision, generator_sha256, profile_sha256):
    """Check one bounded pair against owner-computed current source fingerprints.

    The owner must compute fingerprints from installed helpers and require all
    six cases. This function never grants whole-finding quality acceptance.
    """
    from optimizer_mode_contract import optimizer_mode_matches_record
    from quality_pairing import _digest, _matching, _provenance
    from verification_contract import record_verdict
    if not isinstance(before, dict) or not isinstance(after, dict):
        return False
    case = after.get('case')
    if (not isinstance(case, str) or case not in case_specs() or before.get('case') != case
        or not all(isinstance(value, str) and re.fullmatch('[0-9a-f]{40}', value) for value in (base, revision))
        or base == revision or not all(_digest(value) for value in (generator_sha256, profile_sha256))
        or not _matching(before, after) or not _provenance(before, 'baseline', base)
        or not _provenance(after, 'fixed', revision)
        or any(record.get('bug') != 'Q14' or record.get('mode') != 'new'
               or record.get('generator_module') != 'quality_join_shuffle_checks'
               or record.get('generator_sha256') != generator_sha256
               or record.get('quality_profile_sha256') != profile_sha256
               or record.get('generator_version') != VERSION
               or type(record.get('node_count')) is not int or record['node_count'] != 4
               or type(record.get('rows')) is not int or not 8 <= record['rows'] <= 128
               or record.get('table_service_overrides') != metadata(case, record['rows'])['table_service_overrides']
               or not optimizer_mode_matches_record(record, require_exclusive=True)
               or record_verdict(record) != 'functional' or not _digest(record.get('physical_ast_sha256'))
               or record.get('independent_four_node_bag_oracle') is not True for record in (before, after))
        or any(record.get('q14_stage_observer_version') != VERSION
               or record.get('physical_ast_parsed') is not True for record in (before, after))):
        return False
    b, a = before.get('q14_join_inputs'), after.get('q14_join_inputs')
    if not shape_contract(case, b, False) or not shape_contract(case, a, True):
        return False
    # Only one named join-side edge is allowed to change; even controls require
    # the exact same descriptors, including source sets and key permutations.
    algorithm, chain, enabled = case_specs()[case]
    if not (algorithm == 'reverse' and chain and enabled):
        return b == a
    normalized = json.loads(json.dumps(a))
    normalized[1]['left'] = dict(normalized[1]['left'], kind=b[1]['left']['kind'],
                                 key_permutation=b[1]['left']['key_permutation'],
                                 shuffle_options=b[1]['left']['shuffle_options'])
    return b == normalized
