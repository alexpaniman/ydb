"""Anonymous contracts for Q27's actual top-level SELECT collector.

The function body is read from the selected Git revision, not reimplemented.
One counter at function entry measures traversal work. Generated C++ must remain
in anonymous memory. This verifies the local collector; full transformer/query
integration still requires the repository suites and native query controls.
"""
import hashlib
import json
from pathlib import Path
import subprocess

SOURCE = 'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp'
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
FIXED = '72e17774d130a022ed4649c26c3db7656d83d971'


def specs():
    return [dict(shape=shape, depth=depth)
            for shape in ('shared', 'empty', 'terminal', 'mixed', 'tree')
            for depth in (0, 3, 7, 12)]


def production_function(worktree, revision):
    if revision not in (BASE, FIXED):
        raise ValueError('Collector contracts require the pinned baseline or Q27 revision')
    source = subprocess.check_output(
        ['git', 'show', revision + ':' + SOURCE], cwd=worktree, text=True)
    signature = 'void CollectTopLevelSelects('
    if source.count(signature) != 1:
        raise ValueError('Collector declaration is ambiguous')
    start = source.index(signature)
    end = source.index('\n}\n', start) + 2
    function = source[start:end]
    # This narrow extraction is deliberately limited to the audited function.
    if function.count('{') != function.count('}') or 'CollectTopLevelSelects(c, topLevelSelects, visited);' not in function:
        raise ValueError('Collector shape requires manual review')
    return function


def render_cpp_suite(worktree, revision):
    function = production_function(worktree, revision)
    body_start = function.index('{') + 1
    instrumented = function[:body_start] + '\n    ++Entries;' + function[body_start:]
    text = ''.join('#include <' + header + '>\n' for header in (
        'yql/essentials/ast/yql_expr.h', 'util/generic/hash_set.h',
        'iostream', 'stdexcept', 'vector'))
    text += 'using namespace NYql;\nstatic unsigned long long Entries = 0;\n'
    text += instrumented + '\n'
    text += 'int main() { unsigned failures = 0;\n'
    for index, spec in enumerate(specs()):
        shape, depth = spec['shape'], spec['depth']
        text += '{ bool passed = true; unsigned checks = 0; unsigned long long entries = 0, unique = 0;\ntry {\n'
        text += 'auto Check = [&](bool value) { ++checks; passed = value && passed; };\n'
        text += 'TExprContext ctx; TPositionHandle pos; THashSet<TExprNode*> expected;\n'
        text += 'auto hidden = ctx.NewCallable(pos, "YqlSelect", {});\n'
        text += 'auto selected = ctx.NewCallable(pos, "YqlSelect", {hidden});\n'
        text += 'auto boundary = ctx.NewCallable(pos, "KqpOpRoot", {hidden});\n'
        text += 'auto empty = ctx.NewList(pos, {});\n'
        if shape in ('shared', 'tree'):
            text += 'TExprNode::TPtr input = selected; expected.insert(selected.Get());\n'
        elif shape == 'empty':
            text += 'TExprNode::TPtr input = empty;\n'
        elif shape == 'terminal':
            text += 'TExprNode::TPtr input = boundary;\n'
        else:
            text += 'auto second = ctx.NewCallable(pos, "YqlSelect", {boundary});\n'
            text += 'TExprNode::TPtr input = ctx.NewList(pos, {selected, boundary, second, empty});\n'
            text += 'expected.insert(selected.Get()); expected.insert(second.Get());\n'
        if shape == 'tree':
            # A linear unshared control keeps the same visible root while
            # adding distinct branches; neither implementation should regress.
            text += 'for (unsigned d = 0; d < ' + str(depth) + '; ++d) { input = ctx.NewList(pos, {input, ctx.NewList(pos, {})}); }\n'
        else:
            text += 'for (unsigned d = 0; d < ' + str(depth) + '; ++d) { input = ctx.NewList(pos, {input, input}); }\n'
        # Iterative, independent visible-graph accounting is the work bound.
        text += 'THashSet<TExprNode*> nodes; std::vector<TExprNode::TPtr> pending{input}; unsigned long long edges = 0;\n'
        text += 'while (!pending.empty()) { auto node = pending.back(); pending.pop_back(); if (!nodes.insert(node.Get()).second) continue;\n'
        text += 'if (node->IsCallable("YqlSelect") || node->IsCallable("KqpOpRoot")) continue;\n'
        text += 'edges += node->ChildrenSize(); for (auto child : node->Children()) pending.push_back(child); }\n'
        text += 'THashSet<TExprNode*> roots, visited; Entries = 0; CollectTopLevelSelects(input, roots, visited);\n'
        text += 'entries = Entries; unique = nodes.size(); Check(roots.size() == expected.size());\n'
        text += 'for (auto root : expected) Check(roots.contains(root));\n'
        text += 'Check(!roots.contains(hidden.Get()));\n'
        if revision == FIXED:
            text += 'Check(visited.size() == nodes.size()); Check(entries <= edges + 1);\n'
        text += '} catch (...) { passed = false; } if (!passed) ++failures;\n'
        prefix = '{"case_index":' + str(index) + ',"passed":'
        text += 'std::cout << ' + json.dumps(prefix) + ' << (passed ? "true" : "false")'
        text += ' << ' + json.dumps(',"checks_executed":') + ' << checks'
        text += ' << ' + json.dumps(',"entries":') + ' << entries << ' + json.dumps(',"unique_visible_nodes":') + ' << unique << ' + json.dumps('}\n') + '; }\n'
    text += 'return failures ? 1 : 0; }\n'
    return text


def manifest(worktree):
    functions = {revision: production_function(worktree, revision) for revision in (BASE, FIXED)}
    return {
        'status': 'generator_only_not_compiled_or_executed', 'cases_per_revision': len(specs()),
        'source': SOURCE,
        'source_function_sha256': {revision: hashlib.sha256(source.encode()).hexdigest()
                                   for revision, source in functions.items()},
        'generator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'instrumentation': 'One entry counter; original function body otherwise unchanged',
        'acceptance': 'Exact visible root identities, terminal boundaries, one edge-bounded traversal on fixed branch; compare shared-DAG entries against baseline and retain unshared control',
        'limitations': ['Source-extracted collector, not linked transformer object',
                       'Actual expression-node compile and execution pending',
                       'Full transformer/native integration pending'],
        'rendered_sources_persisted': False,
    }
