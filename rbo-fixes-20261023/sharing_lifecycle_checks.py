"""Compositional sharing-lifecycle checks; rendered workloads stay in RAM.

C31 profiles distinguish early row-join reconstruction, retained block
predicates, forced early reconstruction, and a real lookup control. C47
profiles separately probe an already-physical shared index read at import.
There is no execution or workload-persistence entry point.
"""
from collections import Counter
import hashlib
import itertools

import generate_and_verify as g
import quality_window_checks as windows
import access_effect_checks as access

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'sharing_lifecycle_v1'
_KINDS = {'left': 'LEFT', 'semi': 'LEFT SEMI', 'only': 'LEFT ONLY'}
CASES = tuple('c31_' + profile + '_' + kind
              for profile in ('row', 'block', 'forced', 'lookup_control')
              for kind in _KINDS) + (
    'c47_shared_covering', 'c47_shared_noncovering',
    'c47_shared_noncovering_spilling_control',
)


def _validate(case):
    if case not in CASES:
        raise ValueError('Unknown sharing lifecycle case')


def _rows(n, side):
    if n < 8:
        raise ValueError('Sharing checks require at least eight rows')
    rows = [{'id': i, 'k': i % 4, 'v': (i * (3 if side == 'l' else 5)) % 11}
            for i in range(1, n + 1)]
    if side == 'l':
        rows[-1]['k'] = n + 3
    return rows


def schema_and_seeds(n, case):
    _validate(case)
    if case.startswith('c47_'):
        return access.schema_and_seeds(n, 'c47_noncovering')
    definitions = ['id Int64 NOT NULL', 'k Int64 NOT NULL', 'v Int64 NOT NULL',
                   'PRIMARY KEY (id)']
    schema = ' '.join('CREATE TABLE ' + side + ' (' + ', '.join(definitions) + ');'
                      for side in ('l', 'r'))
    seeds = []
    for side in ('l', 'r'):
        arms = [g.select([g.alias(g.typed('Int64', row[name]), name)
                          for name in ('id', 'k', 'v')]) for row in _rows(n, side)]
        seeds.append('INSERT INTO ' + side + ' ' + g.combine(arms))
    return schema, seeds


def _all_recipes(n):
    result = {}
    for case in CASES:
        if case.startswith('c47_'):
            fields = ['z', 'a', 'k']
            if case != 'c47_shared_covering':
                fields.append('payload')
            producer = g.select(fields, 'ix VIEW byk')
            arms = [g.select([g.alias(g.typed('Int32', tag), 'tag')] + fields,
                             '$shared', where=g.op(g.op('z', '%', divisor), '=', 0))
                    for tag, divisor in enumerate((2, 3))]
            result[case] = '$shared = (' + producer + '); ' + g.combine(arms)
            continue
        kind = case.rsplit('_', 1)[1]
        lookup = case.startswith('c31_lookup_control_')
        predicate = g.op('l.k', '=', 'r.id' if lookup else 'r.k')
        if not lookup:
            predicate = g.op(predicate, 'AND', g.op('l.v', '<', 'r.v'))
        fields = [g.alias('l.id', 'lid')]
        if kind == 'left':
            fields.append(g.alias('r.id', 'rid'))
        result[case] = g.select(fields, g.join('l AS l', 'r AS r', predicate, _KINDS[kind]))
    return result



def recipes(n):
    # Forced YqlSelect rejects explicit SEMI/ONLY syntax before RBO import.
    # Preserve those builders separately; a parser failure is not a C31 test.
    return {case: query for case, query in _all_recipes(n).items()
            if not case.startswith('c31_') or case.endswith('_left')}


def unavailable_recipes(n):
    """Retain rejected frontend profiles without counting optimizer coverage."""
    return {case: query for case, query in _all_recipes(n).items()
            if not (not case.startswith('c31_') or case.endswith('_left'))}

def metadata(case, n):
    _validate(case)
    spilling = case.endswith('spilling_control')
    block = case.startswith(('c31_block_', 'c31_forced_', 'c31_lookup_control_'))
    config = {'EnableFallbackToYqlOptimizer': False,
              'EnableQueryServiceSpilling': spilling,
              'EnableSpillingNodes': 'All' if spilling else 'None',
              'UseBlockHashJoin': block,
              'EnableInlineJoinFiltersAfterCBO': case.startswith('c31_forced_'),
              'EnableKqpDataQueryStreamIdxLookupJoin': True}
    pragmas = [('kikimr.CostBasedOptimizationLevel', '4'),
               ('kikimr.HashJoinMode', 'grace'),
               ('ydb.OptDisableAutoIndexSelection', 'true')]
    if case.startswith(('c31_block_', 'c31_lookup_control_')):
        # A hint cannot bypass the residual-filter exclusion from CBO packing.
        pragmas.append(('ydb.OptimizerHints', 'JoinOrder(l r) JoinType(l r Lookup)'))
    return {'bug': case[:3].upper(), 'generator_version': GENERATOR_VERSION,
            'table_service_overrides': config, 'pragmas': pragmas,
            'node_count': 1, 'peak_native_concurrency': 1,
            'correctness_requires_physical_ast': True,
            'native_target_reachability_pending': True,
            'source_reachability_claim_requires_physical_ast': True}


def _expected(case, n):
    _validate(case)
    if case.startswith('c47_'):
        fields = ['z', 'a', 'k']
        if case != 'c47_shared_covering':
            fields.append('payload')
        return [dict(tag=tag, **{field: row[field] for field in fields})
                for tag, divisor in enumerate((2, 3)) for row in access._index_rows(n)
                if row['z'] % divisor == 0]
    kind = case.rsplit('_', 1)[1]
    lookup = case.startswith('c31_lookup_control_')
    expected = []
    for left in _rows(n, 'l'):
        matches = [right for right in _rows(n, 'r')
                   if left['k'] == right['id' if lookup else 'k']
                   and (lookup or left['v'] < right['v'])]
        if kind == 'left':
            expected.extend({'lid': left['id'], 'rid': right['id']} for right in matches)
            if not matches:
                expected.append({'lid': left['id'], 'rid': None})
        elif bool(matches) == (kind == 'semi'):
            expected.append({'lid': left['id']})
    return expected


def check_rows(case, rows, n):
    canonical = lambda values: Counter(repr(sorted(row.items())) for row in values)
    return canonical(rows) == canonical(_expected(case, n))


_CALLABLES = ('DqPhyStage', 'Replicate', 'BlockHashJoinCore', 'GraceJoinCore',
              'GraceSelfJoinCore', 'MapJoinCore', 'KqpIndexLookupJoin', 'KqpCnStreamLookup')


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
    _validate(case)
    resolve, nodes = windows.parse_physical_graph(text)
    kinds = _join_kind_counts(resolve, nodes)
    counts = {name: sum(bool(node) and node[0] == name for node in nodes)
              for name in _CALLABLES}
    assertions = {}
    if not case.endswith('spilling_control'):
        assertions['no_shared_stage_replicate'] = counts['Replicate'] == 0
    if case.startswith('c31_block_'):
        assertions['retained_residual_uses_block_join'] = counts['BlockHashJoinCore'] >= 1
        assertions['lookup_hint_does_not_create_lookup_residual'] = counts['KqpIndexLookupJoin'] == 0
    elif case.startswith('c31_row_'):
        assertions['row_reconstruction_uses_row_join'] = sum(counts[name] for name in
            ('GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore')) >= 2
    elif case.startswith('c31_lookup_control_'):
        assertions['lookup_control_reaches_lookup_join'] = counts['KqpIndexLookupJoin'] >= 1
    elif case.startswith('c31_forced_'):
        assertions['forced_reconstruction_has_join'] = sum(counts[name] for name in (
            'BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore', 'KqpIndexLookupJoin')) >= 2
    elif case == 'c47_shared_covering':
        assertions['covering_read_avoids_lookup'] = counts['KqpCnStreamLookup'] == 0
    else:
        assertions['noncovering_read_keeps_base_lookup'] = counts['KqpCnStreamLookup'] >= 1
    if case.startswith(('c31_row_', 'c31_forced_')):
        assertions['reconstruction_has_inner_and_left_kinds'] = kinds['Inner'] >= 1 and kinds['Left'] >= 1
    elif case.startswith('c31_block_'):
        assertions['retained_residual_has_left_kind'] = kinds['Left'] >= 1
    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
            'physical_ast_parsed': True, 'callables': counts, 'join_kind_counts': kinds,
            'structural_assertions': assertions,
            'structural_oracle_pass': bool(assertions) and all(assertions.values())}


def lifecycle_model_checks():
    """Check the source-derived eligibility implication without query fixtures.

    This is a consistency check of the documented proof, not execution of C++.
    A filtered non-inner join cannot acquire an algorithm from CBO; the only
    later predicate attachment targets Cross, whose rewrite has no sharing.
    """
    checked = 0
    for block, force, keys, kind in itertools.product((False, True), (False, True),
                                                     (False, True), _KINDS):
        early_retained = block and keys and not force
        for later_filters in (False, True):
            cbo_can_pack = not early_retained
            late_lookup_with_filters = cbo_can_pack and early_retained and later_filters
            late_requires_share = early_retained and later_filters and (
                not block or force or not keys or late_lookup_with_filters)
            if late_requires_share:
                raise AssertionError('Source eligibility implication violated')
            checked += 1
    return {'abstract_transition_checks': checked,
            'new_late_left_family_replication_paths': 0,
            'not_a_native_execution': True}
