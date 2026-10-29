"""Additional correlation controls; generated inputs remain in anonymous memory.

The EXISTS controls deliberately use a compound predicate. They exercise the
generic mark-join route and remain useful when scalar domain elimination is
disabled by the independent scalar-domain fix.
"""
import importlib.util
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    '_correlation_control_builders', Path(__file__).resolve().parent.parent /
    'rbo-review-20261021' / 'generate_and_verify.py')
g = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(g)
CASE_SPECIFIC_SCHEMA = True

CASES = {
    'c05_demanded_cardinality': 'C05',
    'c05_demanded_single_control': 'C05',
    'c06_generic_exists_mixed_types': 'C06',
    'c06_generic_exists_optional_mismatch': 'C06',
    'c06_generic_exists_same_type': 'C06',
    'c07_generic_exists_nan': 'C07',
    'c07_generic_exists_null': 'C07',
    'c07_generic_exists_finite': 'C07',
    'c08_scalar_empty_projection': 'C08',
    'c08_grouped_empty_projection': 'C08',
    'c08_scalar_nonempty_projection': 'C08',
    'c08_grouped_nonempty_projection': 'C08',
}
EXPECTED_FAILURES = {'c05_demanded_cardinality': 'scalar cardinality'}


def _magnitude(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('Use an integer fixture size of at least eight')
    return n + 3


def _definition(case):
    outer_type = 'Double' if case.startswith('c07_') else 'Int64'
    inner_type = 'Uint64' if case == 'c06_generic_exists_mixed_types' else outer_type
    inner_required = case == 'c06_generic_exists_optional_mismatch'
    return outer_type, inner_type, inner_required


def schema_and_seeds(n, case):
    magnitude = _magnitude(n)
    outer_type, inner_type, inner_required = _definition(case)
    schema = []
    seeds = []
    for table, typ, required in [('outer_rows', outer_type, False),
                                 ('inner_rows', inner_type, inner_required)]:
        schema.append('CREATE TABLE ' + table + ' (' + ', '.join([
            'id Uint64 NOT NULL', 'k ' + typ + (' NOT NULL' if required else ''),
            'v Int64', 'PRIMARY KEY (id)']) + ')')
        key = ('nan' if case == 'c07_generic_exists_nan' else
               None if case == 'c07_generic_exists_null' else magnitude)
        rows = [(1, key, magnitude)]
        if table == 'inner_rows':
            if case.startswith('c05_'):
                rows = [(1, magnitude, magnitude * 13)]
                if case == 'c05_demanded_cardinality':
                    rows.append((2, magnitude, magnitude * 17))
            elif case.startswith('c08_'):
                rows = [(1, magnitude + ('nonempty' not in case), magnitude * 13)]
        arms = []
        for identifier, key, value in rows:
            key_expression = (g.call('Nothing', typ + '?') if key is None else g.typed(typ, key))
            if key is not None and not required:
                key_expression = g.call('Just', key_expression)
            arms.append(g.select([
                g.alias(g.typed('Uint64', identifier), 'id'),
                g.alias(key_expression, 'k'),
                g.alias(g.call('Just', g.typed('Int64', value)), 'v')]))
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(arms))
    return '; '.join(schema) + ';', seeds


def recipes(n):
    magnitude = _magnitude(n)
    result = {}
    for case in CASES:
        predicate = g.op('i.k', '=', 'o.k')
        if case.startswith(('c06_', 'c07_')):
            subquery = g.select(['i.v'], 'inner_rows AS i', where=predicate)
            exists = 'EXISTS (' + subquery + ')'
            compound = g.op(exists, '=', g.op('o.v', '>', g.typed('Int64', 0)))
            result[case] = g.select([g.alias('o.id', 'observed')], 'outer_rows AS o', where=compound)
        else:
            grouped = case.startswith('c08_grouped_')
            expression = 'i.v'
            if case.startswith('c08_'):
                expression = g.call('Coalesce', g.call('SUM', 'i.v'), g.typed('Int64', magnitude * 17))
            subquery = g.select([expression], 'inner_rows AS i', where=predicate,
                                group='i.k' if grouped else None)
            result[case] = g.select([g.alias('(' + subquery + ')', 'observed')], 'outer_rows AS o')
    return result


def metadata(case, n):
    _magnitude(n)
    return {'bug': CASES[case], 'generator_version': 'correlation-controls-v1',
            'deterministic': True, 'compare_legacy_hash': False, 'repetitions': 1,
            'pragmas': [], 'table_service_overrides': {},
            'assertions': ['Independent domain and cardinality semantics'],
            'legacy_note': 'Legacy rejects correlated forms; the independent oracle is authoritative'}


def check_rows(case, rows, n):
    magnitude = _magnitude(n)
    if case in EXPECTED_FAILURES:
        return {'semantic_oracle_pass': False, 'expected_failure_observed': False}
    if case in ('c07_generic_exists_nan', 'c07_generic_exists_null'):
        expected = []
    elif case.startswith(('c06_', 'c07_')):
        expected = [{'observed': 1}]
    else:
        value = (None if case == 'c08_grouped_empty_projection' else
                 magnitude * 17 if case == 'c08_scalar_empty_projection' else magnitude * 13)
        expected = [{'observed': value}]
    return {'semantic_oracle_pass': rows == expected}
