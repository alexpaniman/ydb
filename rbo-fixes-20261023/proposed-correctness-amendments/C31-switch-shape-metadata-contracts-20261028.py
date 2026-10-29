"""Generate bounded observer inputs only in memory; persist safe metadata only."""
import ast
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import ModuleType

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
DORMANT = ROOT / 'proposed-correctness-amendments'
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))

CHECKS = 0


def check(value):
    global CHECKS
    if not value:
        raise AssertionError('C31 observation metadata contract failed')
    CHECKS += 1


def sha(value):
    return hashlib.sha256(value).hexdigest()


def load(path, filename=None):
    module = ModuleType(path.stem)
    module.__file__ = str(filename or path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


helper_path = DORMANT / 'c31_shared_field_probe_tuple_guard_20261028.candidate.py'
fields = load(helper_path)
sys.modules['c31_shared_field_probe'] = fields


class ShapeBuilder:
    """Compositional syntax model; no serialized graph is stored."""
    def __init__(self):
        self.counter = 0

    def symbol(self):
        self.counter += 1
        return '$observer_' + str(self.counter)

    def branch(self, form='map', foreign=None, block=False):
        argument, row = self.symbol(), self.symbol()
        source = foreign or argument
        value = ['Member', row, "'private_input_field"]
        projection = ['AsStruct', ["'private_output_field", value]]
        mapping = ['OrderedMap', source, ['lambda', [row], projection]]
        if form == 'identity': body = source
        elif form == 'map': body = mapping
        elif form == 'flow_map': body = ['FromFlow', ['OrderedMap', ['ToFlow', source], mapping[2]]]
        elif form == 'wide':
            wide = self.symbol()
            body = ['NarrowMap', ['ExpandMap', source, ['lambda', [row], value]],
                    ['lambda', [wide], ['AsStruct', ["'private_output_field", wide]]]]
        elif form == 'unknown': body = ['PrivateUnlistedCallable', source]
        elif form == 'foreign_member': body = ['Map', source, ['lambda', [row], ['AsStruct',
                                                       ["'private_output_field", ['Member', argument, "'private_input_field"]]]]]
        elif form == 'depth':
            body = source
            for _ in range(13): body = ['ToFlow', body]
        else: raise ValueError('Unsupported generated shape')
        if block:
            local = self.symbol()
            body = ['block', [['let', local, body], ['return', local]]]
        return ['lambda', [argument], body]

    def switch(self, form='map', foreign=None, block=False, count=2):
        result = ['Switch', ['AsList'], "'private_buffer"]
        for _ in range(count):
            result += [["'0"], self.branch(form, foreign, block)]
        return result

    def stage(self, body, arguments=()):
        return ['DqPhyStage', [], ['lambda', list(arguments), body]]

    def document(self, body, arguments=(), dead=None):
        local = self.symbol()
        statements = [] if dead is None else [['let', self.symbol(), dead]]
        statements += [['let', local, self.stage(body, arguments)], ['return', local]]
        return self.render(statements)

    def tuple_aliases(self, depth):
        statements = []
        current = self.symbol()
        statements.append(['let', current, ["'private_input_field"]])
        for _ in range(depth):
            previous, current = current, self.symbol()
            statements.append(['let', current, [previous, previous]])
        stage = self.symbol()
        statements += [['let', stage, self.stage(current)], ['return', stage]]
        return self.render(statements)

    @staticmethod
    def render(value):
        return '(' + ' '.join(ShapeBuilder.render(item) if isinstance(item, list) else item for item in value) + ')'


module_path = DORMANT / 'c31_switch_shape_observation_20261028.candidate.py'
wrapper_path = DORMANT / 'c31_scope_observation_switch_shapes_20261028.candidate.py'
patch_path = DORMANT / 'C31-lexical-switch-shape-observation-20261028.patch'
probe = load(module_path)
proof_flags = ('exact_shared_producer_origin_proved', 'aggregation_value_to_identity_binding_proved',
               'full_shared_producer_contract_pass', 'candidate_complete_key_pair_lineage_observed')
case = sorted(fields.CASES)[0]
observations = []


def observe(text, selected=case):
    result = probe.observe(selected, text)
    check(result['physical_ast_sha256'] == sha(text.encode()))
    check(result['observation_only'] is True)
    check(result['raw_graph_material_persisted'] is False)
    for flag in proof_flags:
        check(result[flag] is False)
    encoded = json.dumps(result, sort_keys=True)
    for forbidden in ('$observer_', 'private_input_field', 'private_output_field', 'private_buffer',
                      'PrivateUnlistedCallable', '$', 'let ', 'return '):
        check(forbidden not in encoded)
    observations.append(result)
    return result


for selected in sorted(fields.CASES):
    for form in ('identity', 'map', 'flow_map', 'wide', 'unknown', 'foreign_member', 'depth'):
        for block in (False, True):
            builder = ShapeBuilder()
            text = builder.document(builder.switch(form, block=block))
            result = observe(text, selected)
            check(result['switch_shape_inventory_complete'])
            check(result['stage_count'] == result['switch_count'] == 1)
            counters = result['counters']
            if form == 'unknown': check(counters['input_chain_terminal'] == {'unsupported_shape': 2})
            elif form == 'depth': check(counters['input_chain_terminal'] == {'depth_limit': 2})
            else: check(counters['input_chain_terminal'] == {'same_branch_argument': 2})
            if form == 'foreign_member': check(counters['struct_values'] == {'foreign_row_member': 2})
            if form in ('map', 'flow_map'): check(counters['struct_values'] == {'own_row_member': 2})
            if form == 'identity': check(counters['branch_status'] == {'direct_same_argument': 2})
            check(result['lexical_block_count'] == (2 if block else 0))
            check(probe.observe(selected, text) == result)

builder = ShapeBuilder()
outer = builder.symbol()
result = observe(builder.document(builder.switch(foreign=outer), (outer,)))
check(result['counters']['input_chain_terminal'] == {'different_argument': 2})
builder = ShapeBuilder()
dead = ['GraceJoinCore', ['AsList'], ['AsList']]
result = observe(builder.document(builder.switch(), dead=dead))
check(result['join_count'] == 0)

for malformed in ('duplicate_name', 'unbound', 'bad_selector', 'bad_branch', 'extra_body', 'oversize_switch'):
    builder = ShapeBuilder()
    body = builder.switch(count=17 if malformed == 'oversize_switch' else 2)
    if malformed == 'duplicate_name': body[4][1] = body[6][1]
    elif malformed == 'unbound': body[4][2][1] = builder.symbol()
    elif malformed == 'bad_selector': body[3] = ["'private_selector"]
    elif malformed == 'bad_branch': body[4] = ['AsList']
    elif malformed == 'extra_body': body[4].append(['AsList'])
    result = observe(builder.document(body))
    if malformed in ('duplicate_name', 'unbound'):
        check(not result['switch_shape_inventory_complete'])
    elif malformed == 'bad_selector': check(result['counters']['switch_status'].get('other_selector') == 1)
    elif malformed == 'bad_branch': check(result['counters']['branch_status'].get('not_lambda') == 1)
    elif malformed == 'extra_body': check(result['counters']['branch_status'].get('unsupported_arity') == 1)
    else: check(result['counters']['switch_status'].get('unsupported_arity') == 1)

for join_name, (kind_index, left_index, right_index) in probe._JOIN.items():
    for kind_shape, keys_shape in [('known', 'numeric'), ('unknown', 'nonnumeric'), ('expression', 'nonlist')]:
        builder = ShapeBuilder()
        children = [['AsList'] for _ in range(max(kind_index, left_index, right_index) + 1)]
        children[kind_index] = "'Left" if kind_shape == 'known' else "'PrivateKind" if kind_shape == 'unknown' else ['AsList']
        children[left_index] = ["'0", "'1"] if keys_shape == 'numeric' else ["'private_key"] if keys_shape == 'nonnumeric' else ['PrivateKeyCallable']
        children[right_index] = ["'0", "'1"]
        result = observe(builder.document([join_name, *children]))
        check(result['switch_shape_inventory_complete'])
        check(result['join_count'] == 1)
        check('PrivateKind' not in json.dumps(result) and 'PrivateKeyCallable' not in json.dumps(result))
        counters = result['counters']
        if kind_shape == 'known': check(counters['join_kinds'] == {'Left': 1})
        elif kind_shape == 'unknown': check(counters['join_kinds'] == {'other_kind': 1})
        else: check(counters['join_status'].get('kind_not_atom') == 1)
        if keys_shape == 'numeric': check(counters['join_status'].get('equal_key_counts') == 1)
        elif keys_shape == 'nonlist': check(counters['join_status'].get('left_keys_not_list') == 1)

builder = ShapeBuilder()
result = observe(builder.document(['AsTuple', *[builder.switch() for _ in range(33)]]))
check(result['switch_shape_inventory_rejection'] == 'inventory_budget')
check(not observe('not a graph')['switch_shape_inventory_complete'])
check(observe('not a graph', 'unselected')['scope_supported'] is False)

# Repeated tuple aliases must expand by object identity, not once per path.
helper_before = (ROOT / 'c31_shared_field_probe.py').read_bytes()
helper_after = helper_path.read_bytes()
check(b'seen_tuples = set()' in helper_after)
check(b'identity = id(item)' in helper_after)
expected_helper = helper_before.replace(
    b'        self.nodes = set()\n        pending = list(self.roots)\n',
    b'        self.nodes = set()\n        seen_tuples = set()\n        pending = list(self.roots)\n')
expected_helper = expected_helper.replace(
    b'            elif isinstance(item, tuple):\n                pending.extend(item)\n',
    b'            elif isinstance(item, tuple):\n                identity = id(item)\n'
    b'                if identity in seen_tuples:\n                    continue\n'
    b'                seen_tuples.add(identity)\n                pending.extend(item)\n')
check(helper_after == expected_helper)
walk_line = next(index for index, line in enumerate(helper_after.decode().splitlines(), 1)
                 if line.strip() == 'item = pending.pop()')
walk_samples = []
for depth in (4, 8, 12, 64, 256):
    builder = ShapeBuilder()
    text = builder.tuple_aliases(depth)
    visits = [0]
    def trace(frame, event, arg):
        if event == 'line' and frame.f_code.co_filename == str(helper_path) and frame.f_lineno == walk_line:
            visits[0] += 1
        return trace
    prior_trace = sys.gettrace()
    try:
        sys.settrace(trace)
        graph = fields._LexicalGraph(text)
    finally:
        sys.settrace(prior_trace)
    check(len(graph.nodes) == 2)
    check(visits[0] <= 3 * depth + 12)
    check(graph.count <= 4 * depth + 20)
    walk_samples.append({'tuple_alias_depth': depth, 'normalization_visits': graph.count,
                         'reachability_visits': visits[0], 'reachable_callable_nodes': len(graph.nodes)})

# Scope integration changes only the observer attachment and its snapshot/version.
before = (ROOT / 'c31_scope_observation.py').read_bytes()
after = wrapper_path.read_bytes()
functions = lambda data: {node.name: ast.dump(node, include_attributes=False) for node in ast.parse(data).body if isinstance(node, ast.FunctionDef)}
old_functions, new_functions = functions(before), functions(after)
check({name for name in old_functions if old_functions[name] != new_functions[name]} == {'check_physical_ast'})
with tempfile.TemporaryDirectory(prefix='c31-observation-source-only-') as temporary:
    directory = Path(temporary)
    (directory / 'c31_scope_observation.py').write_bytes(before)
    (directory / 'c31_shared_field_probe.py').write_bytes(helper_before)
    applied = subprocess.run(['git', 'apply', str(patch_path)], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(applied.returncode == 0)
    check((directory / 'c31_scope_observation.py').read_bytes() == after)
    check((directory / 'c31_switch_shape_observation.py').read_bytes() == module_path.read_bytes())
    check((directory / 'c31_shared_field_probe.py').read_bytes() == helper_after)
    for name in ('quality_window_checks', 'c31_identity_checks', 'c31_shared_origin_probe'):
        (directory / (name + '.py')).write_bytes((ROOT / (name + '.py')).read_bytes())
    # Dependency modules can import existing generator APIs; no generated source is written.
    wrapper = load(directory / 'c31_scope_observation.py')
    check(wrapper._fields is wrapper._switches.fields)
    check(wrapper._HASHES['c31_shared_field_probe'] == sha(helper_after))
    for selected in sorted(fields.CASES):
        metadata = wrapper.metadata(selected, 24)
        check(metadata['table_service_overrides']['EnableSpillingNodes'] == 'None')
        check(metadata['table_service_overrides']['SpillingServiceConfig'] == '{ LocalFileConfig { Enable: true MaxTotalSize: 1 } }')
    builder = ShapeBuilder()
    text = builder.document(builder.switch('flow_map', block=True))
    base_result = {'physical_ast_sha256': sha(text.encode()), 'structural_oracle_pass': False,
                   'structural_assertions': {'sentinel': False}}
    wrapper._base.check_physical_ast = lambda *args: copy.deepcopy(base_result)
    result = wrapper.check_physical_ast(case, text)
    check(result['structural_oracle_pass'] is False and result['structural_assertions'] == {'sentinel': False})
    nested = result['c31_scope_observation']['lexical_switch_shape_observation']
    check(nested['switch_shape_inventory_complete'])
    check(result['c31_scope_observation']['source_snapshot_sha256']['c31_switch_shape_observation'] == sha(module_path.read_bytes()))
    actual_observer = wrapper._switches.observe
    for flag in proof_flags:
        def forged(*args, flag=flag):
            value = actual_observer(*args)
            value[flag] = True
            return value
        wrapper._switches.observe = forged
        try: wrapper.check_physical_ast(case, text)
        except ValueError: check(True)
        else: check(False)
    wrapper._switches.observe = lambda *args: dict(actual_observer(*args), physical_ast_sha256='0' * 64)
    try: wrapper.check_physical_ast(case, text)
    except ValueError: check(True)
    else: check(False)

check((ROOT / 'c31_scope_observation.py').read_bytes() == before)
check((ROOT / 'c31_shared_field_probe.py').read_bytes() == helper_before)
report = {'status': 'metadata_passed_actual_graph_observation_pending', 'assertions': CHECKS,
          'synthetic_observations': len(observations), 'native_executed': False,
          'build_or_toolchain_executed': False, 'live_helpers_changed': False,
          'generated_graphs_persisted': False, 'full_lineage_flags_remain_false': True,
          'patch_sha256': sha(patch_path.read_bytes()), 'module_sha256': sha(module_path.read_bytes()),
          'wrapper_before_sha256': sha(before), 'wrapper_after_sha256': sha(after)}
report.update(lexical_helper_before_sha256=sha(helper_before), lexical_helper_after_sha256=sha(helper_after),
              tuple_alias_walk_samples=walk_samples)
output = ROOT / 'verification/C31-switch-shape-metadata-contracts-20261028.json'
output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
