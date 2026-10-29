"""Classify existing test failures privately; persist only sanitized metadata."""
from pathlib import Path
import ast
import hashlib
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT.parent
REVISION = 'b71ac02fc35f8a058faf2eb996ca8cf0d0266a18'
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
TRACE = ROOT / 'worktrees/verify/ydb/core/kqp/ut/rbo/test-results/unittest/ytest.report.trace'
sys.path[:0] = [str(ROOT), str(PROJECT / 'rbo-review-20261021')]
from quality_window_checks import _parse_ast


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def source(path, revision=REVISION):
    return subprocess.check_output(['git', 'show', revision + ':' + path], cwd=PROJECT)


def physical_ast(comment):
    start = comment.index('(\n')
    quoted = escaped = False
    depth = 0
    for index in range(start, len(comment)):
        char = comment[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth == 0:
                return comment[start:index + 1]
    raise ValueError('unbalanced_existing_diagnostic_AST')


def compact(text):
    return re.sub(r'\s+', '', text)


def numeric_quotes(text):
    return re.sub(r'"([0-9]+)"', r'\1', text)


def stream_lookups_into(text, table):
    """Equivalent to the existing test helper; its values stay in RAM."""
    values, lookups, table_var = {}, [], None
    for line in text.splitlines():
        if line.startswith('(let $') and line.endswith(')'):
            name_end = line.index(' ', 5)
            name = line[5:name_end]
            values[name] = line[name_end + 1:-1]
            if '(KqpTable \'"' + table + '"' in line:
                table_var = ' ' + name + ' '
        if table_var and '(KqpCnStreamLookup ' in line and table_var in line:
            lookups.append(line)
    for _ in range(3):
        lookups = [re.sub(r'\$[0-9]*', lambda match: values.get(match.group(0), match.group(0)), line) for line in lookups]
    return lookups


lookup_path = 'ydb/core/kqp/ut/rbo/kqp_rbo_lookup_join_ut.cpp'
range_path = 'ydb/core/kqp/ut/rbo/kqp_rbo_yql_ut.cpp'
lookup = source(lookup_path).decode().splitlines()
ranges = source(range_path).decode().splitlines()
assert source(lookup_path) == source(lookup_path, BASE)
assert source(range_path) == source(range_path, BASE)
patterns = {line: re.search(r'R"\((.*?)\)"', lookup[line - 1]).group(1) for line in (393, 563, 779)}
expected_range = ast.literal_eval(re.search(r'"(?:\\.|[^"\\])*"', ranges[3679]).group(0))
failures = []
for raw in TRACE.read_bytes().splitlines():
    item = json.loads(raw)
    value = item.get('value', {})
    if item.get('name') != 'subtest-finished' or value.get('status') != 'fail':
        continue
    comment = value['comment']
    text = physical_ast(comment)
    record = dict(test=[value['class'], value['subtest']], comment_sha256=sha(comment.encode()),
                  classification='printed_physical_AST_canonicalization_assertion',
                  expected_test_source_identical_to_pinned_main=True)
    if value['subtest'].startswith('LookupJoin'):
        assertion = 779 if 'NullablePk' in value['subtest'] else 563 if 'NonCovering' in value['subtest'] else 393
        expected = patterns[assertion]
        if assertion != 393:
            table_line = lookup[(777 if assertion == 779 else 552) - 1]
            table = ast.literal_eval(re.search(r'"(?:\\.|[^"\\])*"', table_line).group(0))
            nodes = stream_lookups_into(text, table)
            assert len(nodes) == 1
            compared = nodes[0]
        else:
            compared = text
        assert expected not in compared
        assert compact(expected) not in compact(compared)
        assert compact(expected) in compact(numeric_quotes(compared))
        record.update(assertion_location=lookup_path + ':' + str(assertion),
            observed_explain_success=True, observed_prior_result_oracle_pass=True,
            prior_result_oracle_location=lookup_path + ':' + str({393:377, 563:543, 779:770}[assertion]),
            exact_index_lookup_target_used=assertion != 393,
            unchanged_settings_confirmed=True, only_numeric_atom_quote_normalization_needed=True,
            symbols=['KqpCnStreamLookup', 'AllowNullKeysPrefixSize', 'LookupJoinRows'],
            explanation='The lookup strategy and null-key-prefix metadata are correct, and the test passed its result-set assertion. The printed numeric atom retains double quotes, causing an exact-text AST assertion to fail.')
    else:
        assert value['subtest'] == 'LiteralRangeWithParameters'
        parsed = _parse_ast(text)
        expected = _parse_ast(expected_range)
        bindings = {node[1]: node[2] for node in parsed if isinstance(node, list) and len(node) == 3 and node[0] == 'let'}
        declared = {node[1] for node in parsed if isinstance(node, list) and node and node[0] == 'declare'}

        def resolve(node, active=frozenset()):
            if isinstance(node, str) and node in bindings:
                if node in active:
                    raise ValueError('cyclic_existing_alias')
                return resolve(bindings[node], active | {node})
            return [resolve(child, active) for child in node] if isinstance(node, list) else node

        bounds, todo = [], [parsed]
        while todo:
            node = todo.pop()
            if isinstance(node, list):
                if node and node[0] == 'KqlKeyInc':
                    bounds.append(node)
                todo.extend(node)
        resolved = [resolve(node) for node in bounds]
        assert len(bounds) == 2 and all(node == expected for node in resolved)
        assert all(node != resolve(node) for node in bounds)
        assert all(isinstance(arg, str) and arg in declared for node in resolved for arg in node[1:])
        record.update(assertion_location=range_path + ':3663', observed_explain_success=True,
            observed_prior_result_oracle_pass=False, later_result_oracle_not_reached=True,
            later_result_oracle_locations=[range_path + ':3687', range_path + ':3703'],
            inclusive_bound_count=2, bounds_structurally_equal_after_alias_resolution=True,
            resolved_bound_matches_test_expected=True, resolved_arguments_are_declared_parameters=True,
            symbols=['KqlKeyInc', 'Parameter', 'ConvertToAst'],
            explanation='The two inclusive range bounds use generated aliases. Resolving those aliases yields exactly the expected declared parameters. The exact-text assertion fails before the runtime result checks execute.')
    failures.append(record)
assert len(failures) == 6
source_paths = [
    'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp',
    'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_query_builder.cpp',
    'ydb/core/kqp/host/kqp_host.cpp', 'ydb/core/kqp/common/kqp_yql.cpp',
    'yql/essentials/core/services/yql_transform_pipeline.cpp',
    'yql/essentials/core/yql_expr_optimize.cpp', 'yql/essentials/core/yql_expr_csee.cpp',
    'yql/essentials/core/yql_expr_csee.h', 'yql/essentials/ast/yql_expr.h', 'yql/essentials/ast/yql_expr.cpp',
    lookup_path, range_path]
bindings = {path: dict(fixed_sha256=sha(source(path)), base_sha256=sha(source(path, BASE))) for path in source_paths}
changed = subprocess.check_output(['git', 'diff', '--name-only', BASE, REVISION], cwd=PROJECT, text=True).splitlines()
assert len(changed) == 8
result = dict(schema_version='c41_six_assertion_source_assessment_v1', revision=REVISION, base_revision=BASE,
    status='source_causality_established_native_amendment_pending', trace_sha256=sha(TRACE.read_bytes()),
    assertion_failure_count=6, findings=failures, changed_production_files=changed, source_bindings=bindings,
    common_cause=dict(symbol='TKqpNewRBOTransformer::ContinueOptimizations',
        location='ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp:450',
        explanation='C41 replaces the complete query and returns Ok. Previously RemapExpr returned Repeat with restart after a changed graph, re-entering the host post-type-annotation stages. That restart supplied both dependency-scope repair and common-subexpression elimination.',
        old_restart_location='yql/essentials/core/yql_expr_optimize.cpp:554',
        old_host_pipeline_location='ydb/core/kqp/host/kqp_host.cpp:2363',
        old_post_annotation_locations=['yql/essentials/core/services/yql_transform_pipeline.cpp:161',
                                       'yql/essentials/core/services/yql_transform_pipeline.cpp:172']),
    canonicalization_proof=[
        dict(symbol='EliminateCommonSubExpressions / VisitNode', location='yql/essentials/core/yql_expr_csee.cpp:619',
            fact='Equal atom nodes are merged and their formatting flags normalized.'),
        dict(symbol='TExprNode::NormalizeAtomFlags', location='yql/essentials/ast/yql_expr.h:2173',
            fact='For equal nonbinary atoms, the minimum flags are retained; merging with default numeric atoms removes arbitrary-content double quoting.'),
        dict(symbol='ConvertToAst / VisitNode / InlineNode', locations=['yql/essentials/ast/yql_expr.cpp:1838','yql/essentials/ast/yql_expr.cpp:2080'],
            fact='Reference counts determine generated let aliases. Duplicate identical bounds retain extra parameter references until CSEE merges the common expression.'),
        dict(symbol='BuildValueNode', location='yql/essentials/ast/yql_expr.cpp:1883',
            fact='Printed atom quoting depends on expression flags; parameters are represented through declared names and any selected let bindings.')],
    recommendation='At the final whole-query RBO export, run UpdateCompletness and then the existing EliminateCommonSubExpressions with forSubGraph=false and the current TypeCtx column-order store. Keep all six assertions unchanged; do not add renderer-specific normalization to tests.',
    classification_limits=['The five lookup result-set checks passed before their failed AST assertions.',
        'The parameterized-range runtime checks were not reached, so this report does not claim their semantic execution passed.',
        'The logged metadata/bounds are semantically consistent with the expectations; no wrong result bag is established by these six failures.',
        'A pinned-main native full-suite comparison was not run in this source-only assessment; causality follows the removed restart and observed canonicalization differences.',
        'A scope-only UpdateCompletness amendment addresses the separate crash mechanism but does not itself restore CSEE canonicalization.',
        'The combined amendment and all original assertions still require native verification; this is not a complete host-pipeline equivalence proof.'],
    native_build_runs=0, live_source_edits=0, raw_diagnostics_AST_SQL_results_copied=False)
out = ROOT / 'verification/C41-six-assertion-canonicalization-source-assessment-20261028.json'
out.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
print(json.dumps(dict(path=str(out.relative_to(ROOT)), sha256=sha(out.read_bytes()), failures=6,
                      lookup_semantic_checks_preceded_assertion=5, range_runtime_check_pending=True)))
