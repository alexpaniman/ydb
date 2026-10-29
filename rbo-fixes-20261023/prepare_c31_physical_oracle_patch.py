"""Prepare dormant oracle corrections; never execute or save a workload."""
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent

JOIN_HELPER = '''def _join_kind_counts(resolve, nodes):
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
        kind = kind.strip("\\\"'")
        if kind not in kinds:
            raise ValueError('Unrecognized physical join kind')
        kinds[kind] += 1
    return kinds
'''


def replace_once(text, old, new):
    assert text.count(old) == 1, 'Oracle source anchor changed'
    return text.replace(old, new, 1)


def render_files(frontend_files=None):
    files = {}
    for name in ('sharing_lifecycle_checks.py', 'c31_identity_checks.py'):
        before = (ROOT / name).read_text()
        source = frontend_files['rbo-fixes-20261023/' + name][1] if frontend_files else before
        after = source.replace('DqBlockHashJoinCore', 'BlockHashJoinCore')
        after = after.replace("'GraceSelfJoinCore',", "'GraceSelfJoinCore', 'MapJoinCore',")
        after = replace_once(after, 'def check_physical_ast(case, text, baseline=None):',
                             JOIN_HELPER + '\n\ndef check_physical_ast(case, text, baseline=None):')
        after = replace_once(after, '    _, nodes = windows.parse_physical_graph(text)',
                             '    resolve, nodes = windows.parse_physical_graph(text)\n'
                             '    kinds = _join_kind_counts(resolve, nodes)')
        if name == 'sharing_lifecycle_checks.py':
            after = replace_once(after,
                "        assertions['row_reconstruction_uses_row_join'] = counts['GraceJoinCore'] + counts['GraceSelfJoinCore'] >= 1",
                "        assertions['row_reconstruction_uses_row_join'] = sum(counts[name] for name in\n"
                "            ('GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore')) >= 2")
            after = replace_once(after,
                "            'BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore', 'KqpIndexLookupJoin')) >= 1",
                "            'BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore', 'KqpIndexLookupJoin')) >= 2")
            anchor = "    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),"
            extra = """    if case.startswith(('c31_row_', 'c31_forced_')):
        assertions['reconstruction_has_inner_and_left_kinds'] = kinds['Inner'] >= 1 and kinds['Left'] >= 1
    elif case.startswith('c31_block_'):
        assertions['retained_residual_has_left_kind'] = kinds['Left'] >= 1
"""
            after = replace_once(after, anchor, extra + anchor)
            after = replace_once(after, "            'physical_ast_parsed': True, 'callables': counts,",
                                 "            'physical_ast_parsed': True, 'callables': counts, 'join_kind_counts': kinds,")
        else:
            # Add MapJoinCore to the two reconstruction count tuples as well.
            after = after.replace("'BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore'))",
                                  "'BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore'))")
            after = replace_once(after,
                "        assertions['reconstruction_has_join'] = sum(counts[name] for name in\n"
                "            ('BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore')) > 0",
                "        assertions['reconstruction_has_both_joins'] = sum(counts[name] for name in\n"
                "            ('BlockHashJoinCore', 'GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore')) >= 2")
            after = replace_once(after,
                "    assertions = {'has_physical_stage': counts['DqPhyStage'] > 0}",
                "    assertions = {'has_physical_stage': counts['DqPhyStage'] > 0}\n"
                "    if mode == 'block_control':\n"
                "        assertions['retained_residual_has_left_kind'] = kinds['Left'] >= 1\n"
                "    else:\n"
                "        assertions['reconstruction_has_inner_and_left_kinds'] = kinds['Inner'] >= 1 and kinds['Left'] >= 1")
            after = after.replace("'callables': counts, 'structural_assertions': assertions,",
                                  "'callables': counts, 'join_kind_counts': kinds, 'structural_assertions': assertions,")
        ast.parse(after)
        files['rbo-fixes-20261023/' + name] = before, after
    return files


def write_patch(path, files):
    patch = ''.join('diff --git a/' + name + ' b/' + name + '\n' +
                    ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + name, tofile='b/' + name))
                    for name, (before, after) in files.items())
    path.write_text(patch)
    checked = subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(path.resolve())],
                             cwd=ROOT.parent, capture_output=True)
    assert checked.returncode == 0, 'Artifact apply check failed; raw diagnostic suppressed'
    return {'path': str(path.relative_to(ROOT)), 'sha256': hashlib.sha256(patch.encode()).hexdigest(),
            'apply_check_passed': True,
            'preimages': {name: hashlib.sha256(before.encode()).hexdigest() for name, (before, _) in files.items()}}


def main():
    patches = [write_patch(ROOT / 'proposed-correctness-amendments/C31-physical-oracle-artifacts.patch', render_files())]
    from prepare_c31_frontend_coverage_patch import render_files as frontend_files
    patches.append(write_patch(ROOT / 'proposed-correctness-amendments/C31-coverage-and-physical-artifacts.patch',
                               render_files(frontend_files())))
    name = 'rbo-fixes-20261023/quality_lookup_checks.py'
    before = (ROOT / 'quality_lookup_checks.py').read_text()
    after = replace_once(before,
        "_JOIN_CALLS = ('GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore', 'DqBlockHashJoinCore', 'BlockMapJoinCore')",
        "_JOIN_CALLS = ('GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore', 'BlockHashJoinCore', 'BlockMapJoinCore')")
    # References use the same closed callable dictionary key.
    after = after.replace("counts['DqBlockHashJoinCore']", "counts['BlockHashJoinCore']")
    assert 'DqBlockHashJoinCore' not in after
    ast.parse(after)
    patches.append(write_patch(ROOT / 'proposed-quality-amendments/lookup-block-callable-artifacts.patch', {name: (before, after)}))
    report = {'purpose': 'Correct physical join names and retain strict reconstruction topology acceptance',
              'artifact_only': True, 'live_files_modified': False, 'patches': patches,
              'source_contracts': [
                  'dq_expr_nodes.json: TDqBlockHashJoinCore matches BlockHashJoinCore; JoinKind child2.',
                  'physical_join_builder.cpp: BuildPhysicalJoin selects MapJoinCore or GraceJoinCore for row joins.',
                  'yql_expr_nodes.json: MapJoinCore/GraceJoinCore JoinKind child2; GraceSelfJoinCore child1.',
                  'kqp_expr_nodes.json: KqpIndexLookupJoin inherits JoinType child1.',
                  'inline_join_filters.cpp: keyed residual LEFT reconstruction creates Inner followed by Left.',
                  'All admitted reconstruction profiles still require two physical joins and both Inner/Left kinds.',
                  'Block controls require actual BlockHashJoinCore and Left kind; lookup exclusion remains.',
                  'BlockMapJoinCore is not added to C31: the reviewed RBO builder does not emit it.'
              ],
              'native_name_evidence': 'verification/C31-frontend-and-physical-callable-diagnostic.json',
              'historical_records_reclassified': False,
              'next_evidence': 'Root reruns actual retained LEFT SQL profiles with corrected summaries; prior records remain unchanged.',
              'native_executed': False, 'compiler_executed': False, 'mocks_executed': False}
    (ROOT / 'verification/C31-physical-oracle-source-contracts.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(patches))


if __name__ == '__main__':
    main()
