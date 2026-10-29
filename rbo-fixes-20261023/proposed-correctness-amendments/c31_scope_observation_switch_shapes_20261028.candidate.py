"""Dormant C31 observation module; install at artifact root only after review.

Workloads are delegated to the existing generator and stay in memory. This
module observes physical shapes without granting shared-value admission.
"""
from collections import Counter
import hashlib
import importlib.util
from pathlib import Path
import sys

_ROOT = Path(__file__).resolve().parent
_HASHES = {}


def _snapshot(name):
    path = _ROOT / (name + '.py')
    source = path.read_bytes()
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    exec(compile(source, str(path), 'exec'), module.__dict__)
    _HASHES[name] = hashlib.sha256(source).hexdigest()
    return module


_windows = _snapshot('quality_window_checks')
_base = _snapshot('c31_identity_checks')
_origin = _snapshot('c31_shared_origin_probe')
_fields = _snapshot('c31_shared_field_probe')
_switches = _snapshot('c31_switch_shape_observation')
CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'c31_shared_scope_observation_v4_lexical_switch_inventory'
CASES = ('c31_stability_float_domain__spilling__left',
         'c31_stability_integer_domain__spilling__left')
_CALLABLES = frozenset(('DqPhyStage', 'TDqOutput', 'Switch', 'Map', 'OrderedMap',
    'FlatMap', 'OrderedFlatMap', 'ExpandMap', 'NarrowMap', 'WideMap',
    'WideFilter', 'WideFlatMap', 'Member', 'Nth', 'AsStruct', 'StablePickle',
    'ToFlow', 'FromFlow', 'Collect', 'Iterator', 'ToStream', 'Extend',
    'OrderedExtend', 'WideCombiner', 'WideCombinerWithSpilling',
    'WideLastCombiner', 'DqPhyHashCombine', 'DqHashCombine', 'DqHashFinalize',
    'MapJoinCore', 'GraceJoinCore', 'BlockHashJoinCore', 'ToHashedDict',
    'SqueezeToDict', 'NarrowSqueezeToDict', 'IfPresent', 'Just', 'Unwrap',
    'Cast', 'SafeCast', 'AggrAdd', 'AggrMin', 'AggrMax', 'block'))
_KINDS = frozenset(('Inner', 'Left', 'LeftSemi', 'LeftOnly', 'Cross'))


def recipes(n):
    source = _base.recipes(n)
    if any(case not in source for case in CASES):
        raise ValueError('Missing selected C31 profile')
    return {case: source[case] for case in CASES}


def metadata(case, n):
    if case not in CASES:
        raise ValueError('Unselected C31 profile')
    if type(n) is not int or n != 24:
        raise ValueError('C31 spill-storage observation is reviewed only at 24 rows')
    value = _base.metadata(case, n)
    value['generator_version'] = GENERATOR_VERSION
    # Keep channel sharing enabled. A one-byte service limit rejects the
    # reviewed channel and hash-aggregation blob formats before file creation.
    # The node mask disables GraceJoin/WideSort and generic aggregation spills;
    # new hash aggregation remains covered by its packed-batch minimum size.
    value['table_service_overrides'].update({
        'EnableSpillingNodes': 'None',
        'SpillingServiceConfig': '{ LocalFileConfig { Enable: true MaxTotalSize: 1 } }',
    })
    return value


schema_and_seeds = _base.schema_and_seeds
check_rows = _base.check_rows


def _inventory(text):
    """Observe only allowlisted callable names, arities and Boolean checks."""
    result = {'scope_shape_inventory_complete': False}
    try:
        resolve, nodes = _windows.parse_physical_graph(text)
        if len(nodes) > 200000:
            raise ValueError('Observation graph exceeds node budget')
        counts = Counter()
        arities = Counter()
        joins = []
        block_bodies = 0
        for node in nodes:
            head = _origin._head(node)
            if head in _CALLABLES:
                counts[head] += 1
            if head == 'lambda':
                if len(node) < 2:
                    raise ValueError('Malformed lambda')
                arities[len(node) - 2] += 1
                block_bodies += sum(_origin._head(resolve(body)) == 'block'
                                    for body in node[2:])
            if head not in ('MapJoinCore', 'GraceJoinCore', 'BlockHashJoinCore'):
                continue
            if len(node) < 6:
                raise ValueError('Incomplete join')
            kind = _origin._atom(node[3], resolve)
            if kind not in _KINDS:
                raise ValueError('Unexpected join kind')
            keys = []
            for child in node[4:6]:
                child = resolve(child)
                if not isinstance(child, list):
                    raise ValueError('Non-list join keys')
                values = [_origin._atom(value, resolve) for value in child]
                keys.append({'count': len(values),
                             'all_numeric_positions': all(value.isdigit() for value in values)})
            joins.append({'callable': head, 'kind': kind,
                          'left_keys': keys[0], 'right_keys': keys[1],
                          'key_counts_match': keys[0]['count'] == keys[1]['count']})
        result.update(scope_shape_inventory_complete=True,
                      allowlisted_callable_counts=dict(sorted(counts.items())),
                      lambda_body_arity_counts={str(key): value for key, value in sorted(arities.items())},
                      block_lambda_body_count=block_bodies,
                      recognized_join_key_shapes=joins)
    except (ValueError, TypeError, IndexError, KeyError, RecursionError) as error:
        result['scope_shape_inventory_rejection'] = type(error).__name__
    return result


def check_physical_ast(case, text, baseline=None):
    if case not in CASES:
        raise ValueError('Unselected C31 profile')
    result = _base.check_physical_ast(case, text, baseline)
    observation = _origin.check_shared_origin(case, text)
    if observation['physical_ast_sha256'] != result['physical_ast_sha256']:
        raise ValueError('Observation AST binding mismatch')
    for field in ('exact_shared_producer_origin_proved',
                  'aggregation_value_to_identity_binding_proved',
                  'full_shared_producer_contract_pass'):
        if observation[field] is not False:
            raise ValueError('Observation cannot grant lineage admission')
    field_observation = _fields.check_shared_fields(case, text)
    if field_observation['physical_ast_sha256'] != result['physical_ast_sha256']:
        raise ValueError('Field observation AST binding mismatch')
    for field in ('exact_shared_producer_origin_proved',
                  'aggregation_value_to_identity_binding_proved',
                  'full_shared_producer_contract_pass'):
        if field_observation[field] is not False:
            raise ValueError('Field observation cannot grant lineage admission')
    observation['constrained_field_observation'] = field_observation
    switch_observation = _switches.observe(case, text)
    if switch_observation['physical_ast_sha256'] != result['physical_ast_sha256']:
        raise ValueError('Switch shape observation AST binding mismatch')
    for field in ('exact_shared_producer_origin_proved',
                  'aggregation_value_to_identity_binding_proved',
                  'full_shared_producer_contract_pass',
                  'candidate_complete_key_pair_lineage_observed'):
        if switch_observation[field] is not False:
            raise ValueError('Switch shape observation cannot grant lineage admission')
    observation['lexical_switch_shape_observation'] = switch_observation
    observation.update(_inventory(text))
    observation.update(source_snapshot_sha256=dict(_HASHES),
                       observation_only=True,
                       raw_plan_material_persisted=False,
                       field_lineage_check_still_required=True)
    result['c31_scope_observation'] = observation
    return result
