"""Compositional checks; rendered inputs and observed values remain in memory."""
import generate_and_verify as g

CASE_SPECIFIC_SCHEMA = True


def recipes(n):
    count = g.call('COUNT', '*')
    arm = lambda divisor: g.select([g.alias('id', 'v')], 't', where=g.op(g.op('id', '%', divisor), '=', 0))
    matching = g.op('a.k', '=', 'b.k')
    residual = g.op(matching, 'AND', g.op('a.id', '<', 'b.id'))
    selected = ['a.id AS lid', 'b.id AS rid']
    result = {
        'c29_union_intersect': arm(2) + ' UNION ' + arm(3) + ' INTERSECT ' + arm(4),
        'c29_except_intersect': arm(2) + ' EXCEPT ' + arm(3) + ' INTERSECT ' + arm(4),
        'c29_left_chain_control': g.combine([arm(2), arm(3), arm(4)], 'UNION'),
        'c29_one_arm_control': arm(2),
        'c31_residual_left': g.select(selected, g.join('t AS a', 't AS b', residual, 'LEFT')),
        'c31_equality_control': g.select(selected, g.join('t AS a', 't AS b', g.op('a.id', '=', 'b.id'), 'LEFT')),
    }
    for kind in ('FULL', 'INNER', 'LEFT'):
        result['c32_' + kind.lower()] = g.select(selected, g.join('l AS a', 'r AS b', g.op('a.id', '=', 'b.id'), kind))
    result['c33_first_arm_order'] = g.combine([
        g.select(['id AS z', 'k AS a'], 't', where=g.op('id', '=', 1)),
        g.select(['k AS a', 'id AS z'], 't', where=g.op('id', '=', 2)),
    ])
    result['c33_same_order_control'] = g.combine([
        g.select(['id AS z', 'k AS a'], 't', where=g.op('id', '=', value)) for value in (1, 2)
    ])
    for kind in ('FULL', 'INNER', 'LEFT'):
        condition = g.op(g.op('a.id', '=', 'b.id'), 'AND', g.op('a.k', '<', 'b.k'))
        result['c39_' + kind.lower() + '_residual'] = g.select(selected, g.join('l AS a', 'r AS b', condition, kind))
    base = g.recipes(n)
    result['c40_multiple_json'] = base['olap_multiple_json']
    result['c40_single_json_control'] = g.select([g.alias(g.call('JSON_VALUE', 'doc', g.quote('$.a')), 'a')], 'col')
    result['c42_nested_nullable'] = base['nested_union_nullable']
    result['c42_flat_nullable_control'] = g.combine([
        g.select([g.alias(g.typed('Int32', 1), 'v')]),
        g.select([g.alias(g.call('Nothing', 'Int32?'), 'v')]),
    ])
    result['c43_nested_tuple_compare'] = base['focused_composite_compare']
    result['c43_scalar_compare_control'] = g.select(['id'], 't', where=g.op('id', '=', 'k'))
    return result


def schema_and_seeds(n, case):
    return g.schema_and_seeds(n, 'olap_multiple_json' if case.startswith('c40_') else case)


def metadata(case, n):
    settings = {}
    pragmas = []
    result = {'table_service_overrides': settings, 'pragmas': pragmas}
    if case.startswith('c31_'):
        settings.update(UseBlockHashJoin=False, EnableQueryServiceSpilling=False, EnableSpillingNodes='"None"')
        pragmas.append(('kikimr.HashJoinMode', 'grace'))
    if case.startswith('c32_'):
        pragmas += [('kikimr.HashJoinMode', 'map'), ('kikimr.CostBasedOptimizationLevel', '0')]
    if case.startswith('c39_'):
        settings.update(UseBlockHashJoin=True, EnableInlineJoinFiltersAfterCBO=False)
        pragmas.append(('kikimr.HashJoinMode', 'grace'))
        if case == 'c39_full_residual':
            result['expected_rejection_token'] = 'Residual join filters require BlockHashJoin or logical filter inlining'
    if case.startswith('c43_'):
        pragmas.append(('ydb.EnableNewRBOPhysicalStagePeephole', 'false'))
    return result


def check_rows(case, rows, n):
    canonical = lambda values: sorted(repr(sorted(row.items())) for row in values)
    if case.startswith('c29_'):
        arms = [{i for i in range(1, n + 1) if i % d == 0} for d in (2, 3, 4)]
        sets = {
            'c29_union_intersect': arms[0] | (arms[1] & arms[2]),
            'c29_except_intersect': arms[0] - (arms[1] & arms[2]),
            'c29_left_chain_control': set.union(*arms),
            'c29_one_arm_control': arms[0],
        }
        expected = [{'v': i} for i in sets[case]]
    elif case.startswith('c31_'):
        expected = ([{'lid': i, 'rid': j} for i in range(1, n + 1) for j in range(i + 1, n + 1)] + [{'lid': n, 'rid': None}]
                    if case.endswith('residual_left') else [{'lid': i, 'rid': i} for i in range(1, n + 1)])
    elif case.startswith('c32_'):
        expected = [{'lid': 1, 'rid': 1}]
    elif case.startswith('c33_'):
        return {'semantic_oracle_pass': canonical(rows) == canonical([{'z': i, 'a': 1} for i in (1, 2)]),
                'column_order_oracle_pass': all(list(row) == ['z', 'a'] for row in rows)}
    elif case.startswith('c39_'):
        expected = {'c39_full_residual': [{'lid': 1, 'rid': None}, {'lid': None, 'rid': 1}],
                    'c39_inner_residual': [], 'c39_left_residual': [{'lid': 1, 'rid': None}]}[case]
    elif case.startswith('c40_'):
        expected = [{'a': str(i), 'b': str(k)} for i, k in enumerate((1, 1, 2, 3), 1)]
        if case.endswith('control'):
            expected = [{'a': row['a']} for row in expected]
    elif case.startswith('c42_'):
        expected = [{'v': None}] if case.endswith('nested_nullable') else [{'v': 1}, {'v': None}]
    elif case.startswith('c43_'):
        expected = [{'id': 1}]
    else:
        raise ValueError(case)
    return canonical(rows) == canonical(expected)
