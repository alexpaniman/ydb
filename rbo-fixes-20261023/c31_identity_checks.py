"""Composable C31 identity acceptance; workloads and rows stay in memory.

These cases supplement the distinct late-sharing checks. They do not claim
support for excluded row types or volatile residual predicates.
"""
from collections import Counter
import hashlib
import itertools

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'c31_scalar_identity_and_copy_stability_v2'
_KINDS = {'left': 'LEFT', 'semi': 'LEFT SEMI', 'only': 'LEFT ONLY'}
_SHAPES = ('unique_mixed', 'unique_allnull', 'domain_mixed', 'domain_allnull',
           'domain_empty_left', 'domain_empty_right', 'domain_constant')
_MODES = ('row', 'forced', 'spilling', 'block_control')
_IDENTITY_CASES = tuple('c31_identity_' + shape + '__' + mode + '__' + kind
                       for shape, mode, kind in itertools.product(_SHAPES, _MODES, _KINDS))
_STABILITY_CASES = tuple('c31_stability_' + value_type + '_' + identity + '__' + mode + '__' + kind
                        for value_type, identity, mode, kind in itertools.product(
                            ('float', 'integer'), ('keyed', 'domain'), _MODES, _KINDS))
CASES = _IDENTITY_CASES + _STABILITY_CASES
_STABILITY_GUARD = 'Join filter reconstruction requires stable copied values'


def _parts(case):
    if case not in CASES:
        raise ValueError('Unknown C31 identity case')
    prefix = 'c31_stability_' if case in _STABILITY_CASES else 'c31_identity_'
    shape, mode, kind = case.removeprefix(prefix).split('__')
    return shape, mode, kind


def _stability_tables(n):
    if n < 8:
        raise ValueError('Copy-stability checks require at least eight rows')
    # Small integral floating values make every reduction order exact. The
    # conservative type guard is tested independently of numerical luck.
    left = [{'id': i + 1, 'k': i % 3, 'v': (i * 7) % 5 - 2} for i in range(n)]
    right = [{'id': i + 1, 'k': i % 3, 'v': (i * 5 + 2) % 13 - 4}
             for i in range(n + 3)]
    return left, right


def _stability_schema_and_seeds(n, shape):
    value_type = 'Double' if shape.startswith('float_') else 'Int64'
    schema = 'CREATE TABLE l (id Int64 NOT NULL, k Int64 NOT NULL, v ' + value_type
    schema += ' NOT NULL, PRIMARY KEY (id)); '
    schema += 'CREATE TABLE r (id Int64 NOT NULL, k Int64 NOT NULL, v Int64 NOT NULL, PRIMARY KEY (id));'
    left, right = _stability_tables(n)
    seeds = []
    for table, rows in (('l', left), ('r', right)):
        seeds.append('INSERT INTO ' + table + ' ' + g.combine([
            g.select([g.alias(g.typed(value_type if table == 'l' and field == 'v' else 'Int64', row[field]), field)
                      for field in ('id', 'k', 'v')]) for row in rows]))
    return schema, seeds


def _stability_recipe(shape, kind):
    reduced = g.select(['k', g.alias(g.call('SUM', 'v'), 's')], 'l', group='k')
    # The outer aggregate removes the floating intermediate from its output
    # schema. A scalar output whitelist alone cannot detect the inner SUM.
    converted = g.select(['k', g.alias(g.call('MIN', 'CAST(s AS Int64)'), 'v')],
                         g.derived(reduced, 'a'), group='k')
    if shape.endswith('_domain'):
        converted = g.combine([converted, converted])
    predicate = g.op(g.op('x.k', '=', 'r.k'), 'AND', g.op('x.v', '<', 'r.v'))
    outputs = [g.alias('x.' + field, field) for field in ('k', 'v')]
    if kind == 'left':
        outputs.append(g.alias('r.id', 'rid'))
    return g.select(outputs, g.join(g.derived(converted, 'x'), 'r AS r', predicate, _KINDS[kind]))


def _tables(n, shape):
    if n < 8:
        raise ValueError('Identity checks require at least eight rows')
    left = []
    for i in range(n):
        left.append({'id': i + 1, 'a': None if i % 3 == 0 else i,
                     'b': None if i % 3 == 1 else i,
                     'k': i % 3, 'v': (i * 3) % 9})
    left[0]['a'] = left[0]['b'] = None
    if shape == 'unique_allnull':
        left = left[:1]
    elif shape == 'domain_allnull':
        for row in left:
            row['a'] = row['b'] = None
    elif shape == 'domain_constant':
        for row in left:
            row.update(a=None, b=None, k=0, v=0)
    right = [{'id': i + 1, 'k': i % 3, 'v': (i * 5 + 2) % 13}
             for i in range(n + 3)]
    if shape == 'domain_empty_left':
        left = []
    if shape == 'domain_empty_right':
        right = []
    return left, right


def _literal(value):
    return 'CAST(NULL AS Int64?)' if value is None else g.typed('Int64', value)


def schema_and_seeds(n, case):
    shape, _, _ = _parts(case)
    if case in _STABILITY_CASES:
        return _stability_schema_and_seeds(n, shape)
    primary = 'a, b' if shape.startswith('unique_') else 'id'
    schema = 'CREATE TABLE l (' + ', '.join([
        'id Int64 NOT NULL', 'a Int64', 'b Int64', 'k Int64 NOT NULL',
        'v Int64 NOT NULL', 'PRIMARY KEY (' + primary + ')']) + '); '
    schema += 'CREATE TABLE r (id Int64 NOT NULL, k Int64 NOT NULL, v Int64 NOT NULL, PRIMARY KEY (id));'
    left, right = _tables(n, shape)
    seeds = []
    for table, rows, fields in (('l', left, ('id', 'a', 'b', 'k', 'v')),
                                ('r', right, ('id', 'k', 'v'))):
        if rows:
            seeds.append('INSERT INTO ' + table + ' ' + g.combine([
                g.select([g.alias(_literal(row[field]), field) for field in fields])
                for row in rows]))
    return schema, seeds


def _all_recipes(n):
    result = {}
    for case in CASES:
        shape, _, kind = _parts(case)
        if case in _STABILITY_CASES:
            result[case] = _stability_recipe(shape, kind)
            continue
        fields = ('a', 'b', 'k', 'v')
        projection = g.select(list(fields), 'l')
        if shape.startswith('domain_'):
            projection = g.combine([projection, projection])
        left = g.derived(projection, 'x')
        predicate = g.op(g.op('x.k', '=', 'r.k'), 'AND', g.op('x.v', '<', 'r.v'))
        outputs = [g.alias('x.' + field, field) for field in fields]
        if kind == 'left':
            outputs.append(g.alias('r.id', 'rid'))
        result[case] = g.select(outputs, g.join(left, 'r AS r', predicate, _KINDS[kind]))
    return result



def recipes(n):
    # Forced YqlSelect rejects explicit SEMI/ONLY syntax before RBO import.
    # Preserve those builders separately; a parser failure is not a C31 test.
    return {case: query for case, query in _all_recipes(n).items()
            if case.endswith('__left')}


def unavailable_recipes(n):
    """Retain rejected frontend profiles without counting optimizer coverage."""
    return {case: query for case, query in _all_recipes(n).items()
            if not (case.endswith('__left'))}

def metadata(case, n):
    shape, mode, _ = _parts(case)
    block = mode in ('forced', 'block_control')
    stability = case in _STABILITY_CASES
    guarded = stability and shape.startswith('float_') and mode in ('row', 'forced')
    result = {'bug': 'C31', 'acceptance_scope': 'copied_producer_stability' if stability else 'archived_row_identity',
            'generator_version': GENERATOR_VERSION,
            'table_service_overrides': {
                'EnableFallbackToYqlOptimizer': False,
                'EnableQueryServiceSpilling': mode == 'spilling',
                'EnableSpillingNodes': 'All' if mode == 'spilling' else 'None',
                'UseBlockHashJoin': block,
                'EnableInlineJoinFiltersAfterCBO': mode == 'forced'},
            'pragmas': [('kikimr.CostBasedOptimizationLevel', '4'),
                        ('kikimr.HashJoinMode', 'grace'),
                        ('ydb.OptDisableAutoIndexSelection', 'true')],
            'node_count': 1, 'peak_native_concurrency': 1,
            'correctness_requires_physical_ast': not guarded,
            'native_target_reachability_pending': True,
            'expected_identity_admission': mode != 'block_control' and not guarded,
            'expected_domain_deduplication': (shape.startswith('domain_') or shape.endswith('_domain'))
                and mode != 'block_control' and not guarded}
    if stability:
        result.update(expected_shared_producer=mode == 'spilling',
                      exact_reduction_order_independent_fixture=True,
                      expected_guard_is_not_semantic_query_acceptance=guarded,
                      hidden_float_reduction=shape.startswith('float_'))
    if guarded:
        result['expected_rejection_token'] = _STABILITY_GUARD
    return result


def _logical_inputs(n, shape):
    left, right = _tables(n, shape)
    projected = [{field: row[field] for field in ('a', 'b', 'k', 'v')} for row in left]
    return projected * (2 if shape.startswith('domain_') else 1), right


def _expected(case, n):
    shape, _, kind = _parts(case)
    if case in _STABILITY_CASES:
        source, right = _stability_tables(n)
        left = [{'k': key, 'v': sum(row['v'] for row in source if row['k'] == key)}
                for key in sorted({row['k'] for row in source})]
        if shape.endswith('_domain'):
            left *= 2
    else:
        left, right = _logical_inputs(n, shape)
    result = []
    for row in left:
        matches = [other for other in right if row['k'] == other['k'] and row['v'] < other['v']]
        if kind == 'left':
            result.extend(dict(row, rid=other['id']) for other in matches)
            if not matches:
                result.append(dict(row, rid=None))
        elif bool(matches) == (kind == 'semi'):
            result.append(dict(row))
    return result


def _bag(rows):
    return Counter(repr(sorted(row.items())) for row in rows)


def check_rows(case, rows, n):
    _parts(case)
    return _bag(rows) == _bag(_expected(case, n))


def _join_kind_counts(resolve, nodes):
    # Public expression-node indices, plus one for the callable head. Count
    # actual callable names, not their C++ wrapper class names.
    offsets = {'BlockHashJoinCore': 3, 'MapJoinCore': 3, 'GraceJoinCore': 3,
               'GraceSelfJoinCore': 2, 'KqpIndexLookupJoin': 2}
    kinds = {kind: 0 for kind in ('Inner', 'Left', 'Right', 'Full', 'LeftSemi',
        'LeftOnly', 'RightSemi', 'RightOnly', 'Exclusion', 'Cross')}
    for node in nodes:
        if not node or not isinstance(node[0], str) or node[0] not in offsets:
            continue
        offset = offsets[node[0]]
        if len(node) <= offset:
            raise ValueError('Incomplete physical join shape')
        kind = resolve(node[offset])
        if not isinstance(kind, str):
            raise ValueError('Non-atom physical join kind')
        kind = kind.strip("\"'")
        if kind not in kinds:
            raise ValueError('Unrecognized physical join kind')
        kinds[kind] += 1
    return kinds


def check_physical_ast(case, text, baseline=None):
    shape, mode, _ = _parts(case)
    resolve, nodes = windows.parse_physical_graph(text)
    kinds = _join_kind_counts(resolve, nodes)
    names = ('DqPhyStage', 'Replicate', 'Switch', 'BlockHashJoinCore', 'GraceJoinCore',
             'GraceSelfJoinCore', 'MapJoinCore', 'StablePickle', 'DqPhyHashCombine', 'DqHashCombine', 'DqHashFinalize',
             'WideCombiner', 'WideCombinerWithSpilling', 'WideLastCombiner')
    counts = {name: sum(bool(node) and node[0] == name for node in nodes) for name in names}
    assertions = {'has_physical_stage': counts['DqPhyStage'] > 0}
    if mode == 'block_control':
        assertions['retained_residual_has_left_kind'] = kinds['Left'] >= 1
    else:
        assertions['reconstruction_has_inner_and_left_kinds'] = kinds['Inner'] >= 1 and kinds['Left'] >= 1
    if case in _STABILITY_CASES:
        aggregations = sum(counts[name] for name in ('DqPhyHashCombine', 'DqHashCombine',
            'DqHashFinalize', 'WideCombiner', 'WideCombinerWithSpilling', 'WideLastCombiner'))
        assertions['producer_reduction_retained'] = aggregations > 0
        if mode == 'spilling':
            assertions['shared_multi_output_switch_present'] = any(
                node and node[0] == 'Switch' and len(node) >= 7 and (len(node) - 3) % 2 == 0
                for node in nodes)
        if mode == 'block_control':
            assertions['retained_residual_has_block_join'] = counts['BlockHashJoinCore'] > 0
        else:
            assertions['reconstruction_has_both_joins'] = sum(counts[name] for name in
                ('BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore')) >= 2
        if mode in ('row', 'forced'):
            assertions['copyable_producer_has_no_remaining_replicate'] = counts['Replicate'] == 0
        return {'physical_ast_parsed': True,
                'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
                'callables': counts, 'join_kind_counts': kinds, 'structural_assertions': assertions,
                'structural_oracle_pass': all(assertions.values()),
                'native_reachability_requires_actual_ast': True,
                'shared_switch_exact_origin_requires_native_review': shape.endswith('_domain') and mode == 'spilling'}
    if mode != 'spilling':
        assertions['copyable_producer_has_no_remaining_replicate'] = counts['Replicate'] == 0
    if mode == 'block_control':
        assertions['retained_residual_has_block_join'] = counts['BlockHashJoinCore'] > 0
    else:
        assertions['reconstruction_has_both_joins'] = sum(counts[name] for name in
            ('BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore')) >= 2
        if not shape.startswith('domain_empty_'):
            assertions['nullable_identity_encoding_present'] = counts['StablePickle'] >= 2
        if shape.startswith('domain_') and not shape.startswith('domain_empty_'):
            assertions['domain_aggregation_present'] = sum(counts[name] for name in
                ('DqPhyHashCombine', 'DqHashCombine', 'DqHashFinalize', 'WideCombiner',
                 'WideCombinerWithSpilling', 'WideLastCombiner')) > 0
    return {'physical_ast_parsed': True,
            'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
            'callables': counts, 'join_kind_counts': kinds, 'structural_assertions': assertions,
            'structural_oracle_pass': bool(assertions) and all(assertions.values()),
            'native_reachability_requires_actual_ast': True}


_EXACT_SCALARS = frozenset(('Bool', 'Int8', 'Uint8', 'Int16', 'Uint16', 'Int32',
    'Uint32', 'Int64', 'Uint64', 'Date', 'Datetime', 'Timestamp', 'Interval',
    'Date32', 'Datetime64', 'Timestamp64', 'Interval64', 'String', 'Utf8', 'Decimal'))


def identity_admission(types, unique, predicate_repeatable=True, subplans_repeatable=True):
    """Admission contract, independent of SQL planner metadata reachability."""
    if not unique and not (predicate_repeatable and subplans_repeatable):
        return False
    for item in types:
        optional = item.startswith('Optional<') and item.endswith('>')
        scalar = item[9:-1] if optional else item
        if (not unique or optional or item == 'Null') and scalar not in _EXACT_SCALARS:
            return False
    return True


def reconstructed_model(left, right, kind, *, unique=False, deduplicate=True):
    """Independent relational-domain model, with no generated query persistence."""
    key = (lambda row: (row['a'], row['b'])) if unique else (lambda row: tuple(sorted(row.items())))
    domain = list({key(row): row for row in left}.values()) if deduplicate and not unique else list(left)
    inner = [(key(row), other['id']) for row in domain for other in right
             if row['k'] == other['k'] and row['v'] < other['v']]
    result = []
    for row in left:
        matches = [rid for identity, rid in inner if identity == key(row)]
        if kind == 'left':
            result.extend(dict(row, rid=rid) for rid in matches)
            if not matches:
                result.append(dict(row, rid=None))
        elif bool(matches) == (kind == 'semi'):
            result.append(dict(row))
    return result
