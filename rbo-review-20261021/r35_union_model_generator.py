"""Generate union binding/multiplicity checks; retain no generated cases."""
import random
import json
from collections import Counter


def trial(rng, depth=3):
    width = rng.randrange(8)
    def make(width, depth):
        if depth == 0 or rng.randrange(3) == 0:
            return {'rows': [tuple(rng.randrange(6) for _ in range(width)) for _ in range(rng.randrange(5))]}
        count = rng.randrange(2, 5)
        child_widths = [rng.randrange(max(1, width), max(2, width + 5)) for _ in range(count)]
        children = [make(w, depth - 1) for w in child_widths]
        mapping = [[rng.randrange(w) for w in child_widths] for _ in range(width)]
        return {'children': children, 'mapping': mapping, 'ordered': bool(rng.randrange(4) == 0)}
    return make(width, depth)


def evaluate(tree):
    if 'rows' in tree:
        return tree['rows']
    return [tuple(row[binding[i]] for binding in tree['mapping'])
            for i, child in enumerate(tree['children']) for row in evaluate(child)]


def merge(tree):
    if 'rows' in tree:
        return tree
    children = [merge(c) for c in tree['children']]
    flatten = [not tree['ordered'] and 'children' in child and not child['ordered'] for child in children]
    new_children = [c for flag, child in zip(flatten, children) for c in (child['children'] if flag else [child])]
    mappings = []
    for binding in tree['mapping']:
        new_binding = []
        for i, (flag, child) in enumerate(zip(flatten, children)):
            new_binding += child['mapping'][binding[i]] if flag else [binding[i]]
        mappings.append(new_binding)
    return {'children': new_children, 'mapping': mappings, 'ordered': tree['ordered']}


def check_stage_permutation(rng):
    # Group connections by producer stage, then restore logical child positions.
    stages = [rng.randrange(8) for _ in range(rng.randrange(2, 24))]
    unique = list(dict.fromkeys(stages))
    order = [i for stage in unique for i, s in enumerate(stages) if s == stage]
    restored = [None] * len(stages)
    for physical_arg, child_index in enumerate(order):
        restored[child_index] = order[physical_arg]
    return restored == list(range(len(stages)))


def main():
    rng = random.Random(350021)
    counts = {'union_binding_cases': 0, 'stage_grouping_cases': 0, 'counterexamples': 0}
    for _ in range(40000):
        tree = trial(rng)
        before, after = evaluate(tree), evaluate(merge(tree))
        counts['union_binding_cases'] += 1
        counts['counterexamples'] += int(before != after)
        counts['stage_grouping_cases'] += 1
        counts['counterexamples'] += int(not check_stage_permutation(rng))
    print(json.dumps(counts, sort_keys=True))


if __name__ == '__main__':
    main()
