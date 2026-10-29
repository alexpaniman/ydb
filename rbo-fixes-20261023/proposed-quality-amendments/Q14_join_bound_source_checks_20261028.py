"""Dormant observer checks. Synthetic expressions exist only in process memory."""
from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]
PATH = Path(__file__).with_name('quality_join_shuffle_checks_20261028.candidate.py')
SPEC = importlib.util.spec_from_file_location('quality_join_shuffle_checks', PATH)
m = importlib.util.module_from_spec(SPEC)
exec(compile(PATH.read_bytes(), str(PATH), 'exec'), m.__dict__)


def graph(case, fixed):
    algorithm, chain, enabled = m.case_specs()[case]
    definitions = []
    atom = json.dumps
    def define(name, value):
        definitions.append(['let', name, value])
        return name
    def stage(inputs, args, body):
        return ['DqPhyStage', inputs, ['lambda', args, body], []]
    def shuffle(producer, side):
        return ['DqCnHashShuffle', ['TDqOutput', producer, atom('0')],
                [atom(side + '_k'), atom(side + '_p')], atom('true')]
    def join(left, right, left_side, right_side):
        settings = [[atom('BuildSide'), atom('Left')]] if algorithm == 'reverse' else []
        return ['BlockHashJoinCore', left, right, atom('Left'), [atom('0'), atom('1')],
                [atom('0'), atom('1')], [atom(left_side + '_k'), atom(left_side + '_p')],
                [atom(right_side + '_k'), atom(right_side + '_p')], settings]
    sources = {}
    for side in ('a', 'b', 'c') if chain else ('a', 'b'):
        table = ['KqpTable', atom('/Root/q' + side), atom('identity'), atom(''), atom('1')]
        source = ['DqSource', ['DataSource', atom('kikimr')],
                  ['KqpRowsSourceSettings', table, [atom('id'), atom('k'), atom('p')], [], ['Void']]]
        arg = '$source_' + side
        sources[side] = define('$stage_' + side, stage([source], [arg], arg))
    first_inputs = [shuffle(sources['a'], 'a'), shuffle(sources['b'], 'b')]
    first_core = join('$first_l', '$first_r', 'a', 'b')
    first = define('$stage_first', stage(first_inputs, ['$first_l', '$first_r'], first_core))
    handles = {'first_inputs': first_inputs, 'first_core': first_core, 'definitions': definitions}
    root = first
    if chain:
        eliminated = enabled and (algorithm == 'grace' or fixed)
        left = ['DqCnMap', ['TDqOutput', first, atom('0')]] if eliminated else shuffle(first, 'a')
        second_inputs = [left, shuffle(sources['c'], 'c')]
        second_core = join('$second_l', '$second_r', 'a', 'c')
        root = define('$stage_second', stage(second_inputs, ['$second_l', '$second_r'], second_core))
        handles.update(second_inputs=second_inputs, second_core=second_core)
    return definitions + [['return', root]], handles


def render(node):
    return '(' + ' '.join(render(value) for value in node) + ')' if isinstance(node, list) else node


def run():
    from optimizer_mode_contract import optimizer_mode_configuration
    checked = 0
    def check(condition):
        nonlocal checked
        if condition is not True:
            raise AssertionError('Sanitized source-only assertion failed at ordinal ' + str(checked + 1))
        checked += 1
    cases = list(m.case_specs())
    hashes, observations = {}, {}
    query_hashes = {case: hashlib.sha256(query.encode()).hexdigest() for case, query in m.recipes(24).items()}
    for case in cases:
        meta = m.metadata(case, 24)
        check(meta['node_count'] == 4)
        check(dict(meta['pragmas'])['kikimr.CostBasedOptimizationLevel'] == '4')
        schema, seeds = m.schema_and_seeds(24, case)
        check(len(seeds) == (3 if m.case_specs()[case][1] else 2))
        check(schema.count('UNIFORM_PARTITIONS = 4') == len(seeds))
        expected = m.expected_rows(case, 24)
        check(m.check_rows(case, expected, 24)['semantic_oracle_pass'])
        check(m.check_rows(case, list(reversed(expected)), 24)['semantic_oracle_pass'])
        check(not m.check_rows(case, expected[:-1], 24)['semantic_oracle_pass'])
        check(not m.check_rows(case, expected + expected[:1], 24)['semantic_oracle_pass'])
        check(any(row['bi'] is None for row in expected))
        for fixed in (False, True):
            tree, handles = graph(case, fixed)
            text = render(tree)
            result = m.check_physical_ast(case, text)
            check(result['physical_ast_parsed'])
            check(m.shape_contract(case, result['q14_join_inputs'], fixed))
            check(result['quality_acceptance_pass'] is False)
            check(result['quality_improvement_verified'] is False)
            observations[(case, fixed)] = result
            hashes[case + ('_fixed' if fixed else '_baseline')] = hashlib.sha256(text.encode()).hexdigest()
            # Same global connection counts, wrong actual first-join source binding.
            handles['first_inputs'].reverse()
            wrong = m.check_physical_ast(case, render(tree))
            check(wrong.get('q14_fixed_connection_shape_observed') is False)
            # Malformed descriptors cannot be promoted into typed shape evidence.
            for malformed in (None, [None], [{'signature': 'a:b'}], [False]):
                check(m.shape_contract(case, malformed, fixed) is False)
            for field, value in (('producer_contains_join', 0), ('origins', ['c']),
                                 ('key_permutation', [False, 1]), ('shuffle_options', ['unknown'])):
                wrong = deepcopy(result['q14_join_inputs'])
                wrong[0]['left'][field] = value
                check(m.shape_contract(case, wrong, fixed) is False)
    positive = 'q14_reverse_chain_on'
    for mutation in ('mismatched_outer_keys', 'mismatched_hash_options', 'wrong_core_binding',
                     'unknown_source', 'wrong_build_side', 'bad_join_index', 'nested_arg_shadow'):
        tree, handles = graph(positive, True)
        if mutation == 'mismatched_outer_keys':
            handles['second_inputs'][1][2].reverse()
        elif mutation == 'mismatched_hash_options':
            handles['second_inputs'][1].append(json.dumps('other'))
        elif mutation == 'wrong_core_binding':
            handles['second_core'][1] = '$second_r'
        elif mutation == 'unknown_source':
            handles['definitions'][0][2][1][0][2][1][1] = json.dumps('/Root/unknown')
        elif mutation == 'wrong_build_side':
            handles['second_core'][8] = []
        elif mutation == 'bad_join_index':
            handles['second_core'][4][0] = json.dumps('not-an-index')
        else:
            handles['second_core'][1] = ['Map', ['AsList'], ['lambda', ['$second_l'], '$second_l']]
        result = m.check_physical_ast(positive, render(tree))
        check(result.get('q14_fixed_connection_shape_observed') is False)
    # A reachable unrelated map does not compensate for the wrong intended edge.
    tree, handles = graph(positive, False)
    handles['second_inputs'].append(['DqCnMap', ['TDqOutput', '$stage_c', json.dumps('0')]])
    handles['definitions'][-1][2][2][1].append('$unrelated')
    check(m.check_physical_ast(positive, render(tree))['q14_fixed_connection_shape_observed'] is False)
    # Optional outputs and void filter lambdas are admitted using public schemas.
    tree, handles = graph(positive, True)
    for definition in handles['definitions']:
        definition[2].append([])
    for name in ('first_core', 'second_core'):
        handles[name].extend([['lambda', ['$filter_arg'], ['Void']] for _ in range(3)])
    check(m.check_physical_ast(positive, render(tree))['q14_fixed_connection_shape_observed'])
    lexical_rejections = ('dead_local_dependency', 'dead_local_join', 'local_let_without_block',
                          'global_argument_collision', 'nested_stage_argument_shadow',
                          'nested_local_argument_shadow', 'duplicate_argument', 'duplicate_binding',
                          'unbound_argument')
    for mutation in lexical_rejections:
        tree, handles = graph(positive, True)
        if mutation == 'dead_local_dependency':
            first = handles['definitions'][-2][2]
            first[2][2] = ['block', ['let', '$unused_local', '$first_r'], ['return', first[2][2]]]
        elif mutation == 'dead_local_join':
            source = handles['definitions'][0][2]
            source[2][2] = ['block', ['let', '$unused_join', deepcopy(handles['first_core'])],
                            ['return', source[2][2]]]
        elif mutation == 'local_let_without_block':
            handles['first_core'][1] = ['let', '$local_alias', '$first_l']
        elif mutation == 'global_argument_collision':
            tree.insert(0, ['let', '$first_l', ['Void']])
        elif mutation == 'nested_stage_argument_shadow':
            handles['first_core'][1] = ['Map', '$first_l', ['lambda', ['$first_l'], '$first_l']]
        elif mutation == 'nested_local_argument_shadow':
            handles['first_core'][1] = ['Map', '$first_l', ['lambda', ['$item'],
                                       ['Map', ['AsList'], ['lambda', ['$item'], '$item']]]]
        elif mutation == 'duplicate_argument':
            handles['definitions'][-2][2][2][1][1] = '$first_l'
        elif mutation == 'duplicate_binding':
            tree.insert(0, ['let', '$stage_a', ['Void']])
        else:
            handles['first_core'][1] = '$unknown_argument'
        result = m.check_physical_ast(positive, render(tree))
        check(result['physical_ast_parsed'] is False)
        check(result['q14_fixed_connection_shape_observed'] is False)
    # Ordinary distinct nested lambda arguments retain the parent input binding.
    tree, handles = graph(positive, True)
    handles['first_core'][1] = ['Map', '$first_l', ['lambda', ['$item'], '$item']]
    check(m.check_physical_ast(positive, render(tree))['q14_fixed_connection_shape_observed'])
    current = hashlib.sha256(PATH.read_bytes()).hexdigest()
    digest = lambda value: hashlib.sha256(value.encode()).hexdigest()
    base, revision = '1' * 40, '2' * 40
    profile = digest('synthetic source-only profile, not native evidence')
    settings = 'EnableNewRBO: true\nEnableFallbackToYqlOptimizer: false\n'
    def record(case, fixed):
        result = dict(observations[(case, fixed)])
        result.update(case=case, bug='Q14', mode='new', rows=24, repetition=0, phase='fixed' if fixed else 'baseline',
                      revision=revision if fixed else base, generator_module='quality_join_shuffle_checks',
                      generator_sha256=current, generator_version=m.VERSION, quality_profile_sha256=profile,
                      settings_sha256=digest(settings), schema_sha256=digest('synthetic schema'),
                      fixture_sha256=[digest('synthetic fixture')], query_sha256=query_hashes[case], validation_sha256=[],
                      query_syntax='yql', node_count=4, table_service_overrides=m.metadata(case, 24)['table_service_overrides'],
                      binary_provenance_verified=True, binary_provenance={'sha256': digest('synthetic executable')},
                      build_receipt_sha256=digest('synthetic receipt'), semantic_oracle_pass=True,
                      independent_four_node_bag_oracle=True, exit_code=0)
        result.update(optimizer_mode_configuration(settings, 'new', 'yql', {'YqlSelect': 'force'}))
        return result
    for case in cases:
        before, after = record(case, False), record(case, True)
        check(m.pair_observations(before, after, base, revision, current, profile))
        for field, value in (('phase', 'baseline'), ('node_count', 1), ('semantic_oracle_pass', False),
                             ('physical_ast_sha256', None), ('quality_profile_sha256', digest('stale')),
                             ('generator_sha256', digest('stale')), ('binary_provenance_verified', False)):
            bad = dict(after, **{field: value})
            check(m.pair_observations(before, bad, base, revision, current, profile) is False)
        check(m.pair_observations(before, after, base, revision, digest('stale'), profile) is False)
        check(m.pair_observations(before, after, base, revision, current, digest('stale')) is False)
        bad = deepcopy(after)
        bad['optimizer_mode_configuration']['enable_fallback'] = True
        check(m.pair_observations(before, bad, base, revision, current, profile) is False)
    return {'status': 'source_only_checks_passed_native_pending', 'assertions_passed': checked,
            'case_count': len(cases), 'ordered_case_vector': cases, 'candidate_sha256': current,
            'rendered_query_sha256': query_hashes, 'synthetic_physical_ast_sha256': hashes,
            'lexical_rejection_controls': list(lexical_rejections),
            'rendered_queries_or_plans_persisted': False, 'native_executions': 0}


if __name__ == '__main__':
    print(json.dumps(run(), sort_keys=True))
