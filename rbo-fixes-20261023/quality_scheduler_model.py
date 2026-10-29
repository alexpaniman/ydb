"""Sanitized Q28 worklist equivalence/cost model; graphs stay in RAM."""
from dataclasses import dataclass, field
import copy
import random


@dataclass(eq=False)
class Slot:
    node: object


@dataclass(eq=False)
class Node:
    kind: str
    children: list = field(default_factory=list)
    definitions: dict = field(default_factory=dict)
    columns: tuple = ()
    push: bool = False
    calls: tuple = ()
    cached: object = None


class Graph:
    def __init__(self):
        self.next_id = 0
        self.subplans = {}
        self.attempts = self.output_visits = self.matches = self.candidate_visits = 0

    def fresh(self):
        self.next_id += 1
        return self.next_id

    def source(self):
        key = self.fresh()
        return Node('Source', columns=(key,)), key


def traversal(slot, graph, seen=None):
    if seen is None:
        seen = set()
    node = slot.node
    if id(node) in seen:
        return []
    seen.add(id(node))
    entries = []
    for call in node.calls:
        entries += traversal(graph.subplans[call], graph, seen)
    for child in node.children:
        entries += traversal(child, graph, seen)
    return entries + [(slot, node)]


def refresh(node, graph):
    graph.output_visits += 1
    if node.kind == 'Source':
        node.cached = frozenset(node.columns)
    elif node.kind == 'Map':
        available = node.children[0].node.cached
        assert not available.intersection(node.definitions)
        assert set(node.definitions.values()).issubset(available)
        node.cached = available.union(node.definitions)
    elif node.kind == 'Port':
        assert set(node.definitions.values()).issubset(node.children[0].node.cached)
        node.cached = frozenset(node.definitions)
    elif node.kind == 'Union':
        for column in node.columns:
            assert len(node.definitions[column]) == len(node.children)
            assert all(source in child.node.cached for source, child in zip(node.definitions[column], node.children))
        node.cached = frozenset(node.columns)
    elif node.kind in ('Join', 'Right'):
        a, b = (child.node.cached for child in node.children)
        assert not a.intersection(b)
        node.cached = a.union(b)
    else:
        node.cached = node.children[0].node.cached


def refresh_all(root, graph):
    for _, node in traversal(root, graph):
        refresh(node, graph)


def apply(slot, graph, rules):
    graph.candidate_visits += 1
    node = slot.node
    for rule in rules:
        graph.attempts += 1
        if rule == 'right':
            if node.kind != 'Right':
                continue
            slot.node = Node('Join', list(reversed(node.children)), calls=node.calls)
            graph.matches += 1
            return 0
        if node.kind != 'Map':
            continue
        child = node.children[0].node
        if rule == 'through':
            if not node.push or child.kind not in ('Filter', 'Join', 'Map') or (child.kind == 'Map' and child.push):
                continue
            kept, moved = {}, [{} for _ in child.children]
            for output, source in node.definitions.items():
                index = next((i for i, branch in enumerate(child.children) if source in branch.node.cached), None)
                (kept if index is None else moved[index])[output] = source
            if not any(moved):
                continue
            for index, definitions in enumerate(moved):
                if definitions:
                    child.children[index] = Slot(Node('Map', [child.children[index]], definitions, push=True))
            child.cached = None
        elif rule == 'into':
            if child.kind != 'Map':
                continue
            available = child.children[0].node.cached
            kept = {out: src for out, src in node.definitions.items() if src not in available}
            moved = {out: src for out, src in node.definitions.items() if src in available}
            if not moved:
                continue
            child.definitions.update(moved)
            child.cached = None
        else:
            continue
        slot.node = Node('Map', [Slot(child)], kept, calls=node.calls) if kept else child
        graph.matches += 1
        return 2
    return None


def conservative(root, graph, rules):
    while True:
        refresh_all(root, graph)
        for slot, _ in traversal(root, graph):
            if apply(slot, graph, rules) is not None:
                break
        else:
            return
        if graph.matches >= 1000:
            raise RuntimeError('match ceiling')


def bounded(slot, depth):
    node = slot.node
    entries = []
    if depth:
        for child in node.children:
            entries += bounded(child, depth - 1)
    return entries + [(slot, node)]


def local(root, graph, rules, *, admitted=True):
    initial = traversal(root, graph)
    owners = {}
    def record(slot):
        nonlocal admitted
        previous = owners.setdefault(id(slot.node), id(slot))
        admitted &= previous == id(slot)
    record(root)
    for _, node in initial:
        for child in node.children:
            record(child)
        for call in node.calls:
            record(graph.subplans[call])
    if admitted:
        refresh_all(root, graph)
        pending = list(reversed(initial))
        while pending:
            slot, expected = pending.pop()
            if slot.node is not expected:
                break
            previous = slot.node.cached
            depth = apply(slot, graph, rules)
            if depth is None:
                continue
            region = bounded(slot, depth)
            for _, node in region:
                refresh(node, graph)
            assert slot.node.cached == previous
            pending.extend(reversed(region))
            if graph.matches >= 1000:
                raise RuntimeError('match ceiling')
    conservative(root, graph, rules)


def fingerprint(node):
    return (node.kind, tuple(sorted(node.definitions.items())), node.columns, node.push,
            tuple(fingerprint(child.node) for child in node.children))


def evaluate(node):
    if node.kind == 'Source':
        return [{key: key * 7 for key in node.columns}]
    rows = [evaluate(child.node) for child in node.children]
    if node.kind == 'Map':
        return [row | {out: row[src] for out, src in node.definitions.items()} for row in rows[0]]
    if node.kind == 'Port':
        return [{out: row[src] for out, src in node.definitions.items()} for row in rows[0]]
    if node.kind in ('Join', 'Right'):
        return [left | right for left in rows[0] for right in rows[1]]
    if node.kind == 'Union':
        return [{out: row[node.definitions[out][index]] for out in node.columns}
                for index, branch in enumerate(rows) for row in branch]
    return rows[0]


def make_graph(seed, branches=5, depth=3, *, shared=False, subplans=False):
    rng, graph = random.Random(seed), Graph()
    leaves = []
    for _ in range(branches):
        node, key = graph.source()
        for _ in range(depth):
            choice = rng.randrange(4)
            if choice < 2:
                node = Node('Filter', [Slot(node)])
            elif choice == 2:
                alias = graph.fresh()
                node = Node('Map', [Slot(node)], {alias: key})
            else:
                right, _ = graph.source()
                node = Node('Join', [Slot(node), Slot(right)])
            output = graph.fresh()
            definitions = {output: key}
            if node.kind == 'Map' and rng.randrange(2):
                definitions[graph.fresh()] = next(iter(node.definitions))
            node = Node('Map', [Slot(node)], definitions, push=bool(rng.randrange(2)))
        leaves.append(Slot(node))
    if shared:
        producer = leaves.pop()
        refresh_all(producer, graph)
        for _ in range(2):
            rebinding = {graph.fresh(): key for key in producer.node.cached}
            port = Node('Port', [producer], rebinding)
            key = next(iter(rebinding))
            leaves.append(Slot(Node('Map', [Slot(port)], {graph.fresh(): key}, push=True)))
    for leaf in leaves:
        refresh_all(leaf, graph)
    output = graph.fresh()
    root = Slot(Node('Union', leaves, {output: tuple(min(leaf.node.cached) for leaf in leaves)}, columns=(output,)))
    if subplans:
        sub, key = graph.source()
        sub = Node('Map', [Slot(Node('Filter', [Slot(sub)]))], {graph.fresh(): key}, push=True)
        graph.subplans[1] = Slot(sub)
        root.node.calls = (1,)
    graph.attempts = graph.output_visits = graph.matches = graph.candidate_visits = 0
    return graph, root


def verify(seed=2828, iterations=160):
    rng = random.Random(seed)
    compared = semantics = fallbacks = raw_fallbacks = right_compared = 0
    for _ in range(iterations):
        graph, root = make_graph(rng.randrange(1 << 30), rng.randrange(1, 10), rng.randrange(1, 7),
                                 shared=bool(rng.randrange(2)), subplans=bool(rng.randrange(2)))
        expected = evaluate(root.node)
        graph_a, root_a = copy.deepcopy((graph, root))
        graph_b, root_b = copy.deepcopy((graph, root))
        conservative(root_a, graph_a, ('through', 'into'))
        local(root_b, graph_b, ('through', 'into'))
        assert fingerprint(root_a.node) == fingerprint(root_b.node)
        assert evaluate(root_b.node) == expected
        assert graph_a.matches == graph_b.matches
        for call in graph.subplans:
            assert fingerprint(graph_a.subplans[call].node) == fingerprint(graph_b.subplans[call].node)
        compared += 1 + len(graph.subplans)
        semantics += 1
        graph_c, root_c = copy.deepcopy((graph, root))
        local(root_c, graph_c, ('through', 'into'), admitted=False)
        assert fingerprint(root_c.node) == fingerprint(root_a.node)
        assert graph_c.attempts == graph_a.attempts
        fallbacks += 1
    for iteration in range(24):
        graph, root = make_graph(seed + iteration, 6, 3)
        producer_output = root.node.columns[0]
        output = graph.fresh()
        root = Slot(Node('Union', [Slot(root.node), Slot(root.node)],
                         {output: (producer_output, producer_output)}, columns=(output,)))
        expected = evaluate(root.node)
        graph_a, root_a = copy.deepcopy((graph, root))
        graph_b, root_b = copy.deepcopy((graph, root))
        conservative(root_a, graph_a, ('through', 'into'))
        local(root_b, graph_b, ('through', 'into'))
        assert fingerprint(root_a.node) == fingerprint(root_b.node)
        assert evaluate(root_b.node) == expected
        assert graph_a.attempts == graph_b.attempts
        assert graph_a.output_visits == graph_b.output_visits
        assert graph_a.candidate_visits == graph_b.candidate_visits
        raw_fallbacks += 1
    for iteration in range(24):
        graph = Graph()
        node, key = graph.source()
        for _ in range(1 + iteration):
            right, _ = graph.source()
            node = Node('Right', [Slot(node), Slot(right)])
        root = Slot(node)
        expected = evaluate(node)
        graph_a, root_a = copy.deepcopy((graph, root))
        graph_b, root_b = copy.deepcopy((graph, root))
        conservative(root_a, graph_a, ('right',))
        local(root_b, graph_b, ('right',))
        assert fingerprint(root_a.node) == fingerprint(root_b.node)
        assert evaluate(root_b.node) == expected
        right_compared += 1
    costs = []
    for branches in (8, 16, 32, 64):
        graph, root = make_graph(11, branches, 2)
        graph_a, root_a = copy.deepcopy((graph, root))
        graph_b, root_b = copy.deepcopy((graph, root))
        conservative(root_a, graph_a, ('through', 'into'))
        local(root_b, graph_b, ('through', 'into'))
        assert fingerprint(root_a.node) == fingerprint(root_b.node)
        assert graph_b.output_visits < graph_a.output_visits
        assert graph_b.attempts < graph_a.attempts
        assert graph_b.candidate_visits < graph_a.candidate_visits
        costs.append({'branches': branches, 'matches': graph_b.matches,
                      'conservative_attempts': graph_a.attempts, 'worklist_attempts': graph_b.attempts,
                      'conservative_output_visits': graph_a.output_visits,
                      'worklist_output_visits': graph_b.output_visits,
                      'conservative_candidate_visits': graph_a.candidate_visits,
                      'worklist_candidate_visits': graph_b.candidate_visits})
    return {'model_pass': True, 'seed': seed, 'fixed_point_comparisons': compared,
            'semantic_graph_checks': semantics, 'unknown_effect_fallback_checks': fallbacks,
            'raw_shared_owner_fallback_checks': raw_fallbacks, 'right_join_chain_model_checks': right_compared,
            'model_costs': costs, 'native_runs': 0, 'native_cost_reduction_verified': False}
