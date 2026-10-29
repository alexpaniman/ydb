"""Read-only source and existing-log assessment; emit dormant C41 amendment."""
import collections
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ARTIFACTS = HERE.parents[1]
PROJECT = ARTIFACTS.parent
REVISION = 'dd1d8cabb86258c4bcb998bd6b317b296bfdd251'
TARGET = 'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp'
LOG_ROOT = Path('/home/alexpaniman/.ya/build/symres/10c206bc798c4240ea0f509b3596a429/test-results/unittest')


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    paths = [TARGET,
        'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.h',
        'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_query_builder.cpp',
        'ydb/core/kqp/host/kqp_runner.cpp', 'ydb/core/kqp/host/kqp_host.cpp',
        'yql/essentials/core/services/yql_transform_pipeline.cpp',
        'yql/essentials/core/services/yql_transform_pipeline.h',
        'yql/essentials/core/yql_graph_transformer.cpp', 'yql/essentials/core/yql_graph_transformer.h',
        'yql/essentials/core/yql_expr_constraint.cpp', 'yql/essentials/core/yql_expr_csee.cpp',
        'yql/essentials/ast/yql_expr.h',
        'ydb/core/kqp/opt/kqp_opt_hash_func_propagate_transformer.cpp',
        'ydb/core/kqp/opt/peephole/kqp_opt_peephole.cpp',
        'ydb/core/kqp/provider/yql_kikimr_constraints.cpp',
        'yql/essentials/providers/common/transform/yql_visit.h',
        'ydb/library/yql/providers/dq/provider/yql_dq_datasource_constraints.cpp',
        'ydb/library/yql/providers/dq/provider/yql_dq_datasink_constraints.cpp',
        'ydb/core/kqp/opt/rbo/rules/constant_folding_stage.cpp',
        'ydb/core/kqp/opt/rbo/ya.make']
    raw = {path: subprocess.check_output(['git', 'show', REVISION + ':' + path], cwd=PROJECT) for path in paths}
    source = {path: data.decode() for path, data in raw.items()}
    before = source[TARGET]
    old = '''    // Final stage rebuilding can replace lambdas while reusing their bodies.
    // Restore completion and canonicalization on the final closed query.
    const auto status = UpdateCompletness(output, output, ctx);
    if (status != TStatus::Ok) {
        return status;
    }
    return EliminateCommonSubExpressions(output, output, ctx, false, *TypeCtx.ColumnOrderStorage);
'''
    new = '''    // Final stage rebuilding can replace lambdas while reusing their bodies.
    // Complete constraints before restoring scopes and canonicalizing the closed query.
    // Keep annotation restarts local: this output is no longer a query-block list.
    auto finalTypeAnnotation = TTransformationPipeline(&TypeCtx)
        .AddTypeAnnotationTransformer()
        .AddPostTypeAnnotation()
        .Build(false);
    return InstantTransform(*finalTypeAnnotation, output, ctx);
'''
    include_before = '#include <yql/essentials/core/yql_expr_csee.h>'
    include_after = '#include <yql/essentials/core/services/yql_transform_pipeline.h>'
    check(before.count(old) == 1 and before.count(include_before) == 1, 'exact_previous_incomplete_boundary')
    after = before.replace(include_before, include_after, 1).replace(old, new, 1)
    check(after.replace(include_after, include_before, 1).replace(new, old, 1) == before,
          'only_boundary_and_required_include_changed')
    check('yql/essentials/' not in TARGET, 'no_essentials_source_edits')
    check('output = RBO.Optimize(Roots, rboCtx);' in after, 'complete_physical_query_export_preserved')
    check('AddPlans(rboCtx.ExecutionJson, rboCtx.ExplainJson);' in after, 'single_plan_publication_preserved')
    check(after[after.index('IGraphTransformer::TStatus TKqpNewRBOTransformer::DoTransform'):after.index('NThreading::TFuture<void> TKqpNewRBOTransformer::DoGetAsyncFuture')] ==
          before[before.index('IGraphTransformer::TStatus TKqpNewRBOTransformer::DoTransform'):before.index('NThreading::TFuture<void> TKqpNewRBOTransformer::DoGetAsyncFuture')],
          'query_block_result_effect_ownership_import_unchanged')
    check(after[after.index('IGraphTransformer::TStatus TKqpRBOCleanupTransformer::DoTransform'):] ==
          before[before.index('IGraphTransformer::TStatus TKqpRBOCleanupTransformer::DoTransform'):],
          'cleanup_and_stage_registration_unchanged')
    pipeline = source['yql/essentials/core/services/yql_transform_pipeline.cpp']
    start = pipeline.index('TTransformationPipeline& TTransformationPipeline::AddPostTypeAnnotation(')
    end = pipeline.index('TTransformationPipeline& TTransformationPipeline::AddCommonOptimization(', start)
    post = pipeline[start:end]
    check(post.index('CreateConstraintTransformer(') < post.index('UpdateCompletness(') < post.index('EliminateCommonSubExpressions('),
          'standard_constraint_completion_CSEE_order')
    check('CreateConstraintTransformer(*TypeAnnotationContext_, /*instantOnly=*/false, forSubGraph, disableConstraintCheck)' in post,
          'standard_constraint_checks_enabled')
    check('coStore = TypeAnnotationContext_->ColumnOrderStorage' in post, 'same_type_context_column_order_storage')
    header = source['yql/essentials/core/services/yql_transform_pipeline.h']
    check('AddPostTypeAnnotation(bool forSubGraph = false, bool disableConstraintCheck = false' in header,
          'closed_query_default_and_constraint_validation_preserved')
    check(new.index('.AddTypeAnnotationTransformer()') < new.index('.AddPostTypeAnnotation()'),
          'type_annotation_available_after_constraint_restart')
    graph = source['yql/essentials/core/yql_graph_transformer.cpp']
    graph_header = source['yql/essentials/core/yql_graph_transformer.h']
    check('bool breakOnRestart = false' in graph_header, 'local_instant_driver_consumes_restart_default')
    instant = graph[graph.index('IGraphTransformer::TStatus InstantTransform('):graph.index('IGraphTransformer::TStatus AsyncTransformStep(')]
    check('AsyncTransformStepImpl(' in instant and '/*applyAsyncChanges=*/false' in instant,
          'existing_instant_driver_not_custom_status_loop')
    check('status.Level == IGraphTransformer::TStatus::Async' in instant and 'return IGraphTransformer::TStatus::Error;' in instant,
          'unexpected_async_rejected_not_falsely_completed')
    check('.Wait(' not in instant and 'GetAsyncFuture' not in instant, 'no_blocking_future_wait')
    check(instant.count('ctx.ResetCycleDetector();') == 2, 'existing_instant_cycle_detector_reset_explicitly_accounted')
    step = graph[graph.index('IGraphTransformer::TStatus AsyncTransformStepImpl('):graph.index('IGraphTransformer::TStatus InstantTransform(')]
    check('ctx.RepeatTransformCounter < ctx.RepeatTransformLimit' in step and 'AddTooManyTransformationsError' in step,
          'shared_repeat_budget_and_limit_retained')
    check('if (breakOnRestart && status.HasRestart)' in step, 'restart_stays_in_local_composite')
    check('Index_ = 0;' in graph and 'if (status.HasRestart)' in graph, 'composite_restarts_its_own_type_stage')
    csee = source['yql/essentials/core/yql_expr_csee.cpp']
    expr = source['yql/essentials/ast/yql_expr.h']
    check('node.GetAllConstraints()' in csee, 'CSEE_reads_constraint_metadata')
    check('const TConstraintNode::TListType& GetAllConstraints() const' in expr and
          'Y_ENSURE(static_cast<EState>(State_) >= EState::ConstrComplete);' in expr,
          'constraint_state_precondition_is_real_API')
    constraint = source['yql/essentials/core/yql_expr_constraint.cpp']
    check('class TDefaultCallableConstraintTransformer: public TSyncTransformerBase' in constraint,
          'default_constraint_callable_is_synchronous')
    check('class TVisitorTransformerBase: public TSyncTransformerBase' in source['yql/essentials/providers/common/transform/yql_visit.h'],
          'provider_visitor_constraint_base_is_synchronous')
    for path, klass in (
        ('ydb/core/kqp/provider/yql_kikimr_constraints.cpp', 'class TKiSourceConstraintsTransformer final : public TVisitorTransformerBase'),
        ('ydb/library/yql/providers/dq/provider/yql_dq_datasource_constraints.cpp', 'class TDqDataSourceConstraintTransformer : public TVisitorTransformerBase'),
        ('ydb/library/yql/providers/dq/provider/yql_dq_datasink_constraints.cpp', 'class TDqDataSinkConstraintTransformer : public TVisitorTransformerBase')):
        check(klass in source[path], 'reachable_sync_provider:' + path)
    physical = source['ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_query_builder.cpp']
    check('TypeAnnotate(physicalQuery);' in physical, 'final_builder_previously_types_without_post_type_stages')
    check('if (status != IGraphTransformer::TStatus::Ok)' in physical and 'Type inference failed for stage in NEW RBO' in physical,
          'existing_physical_builder_requires_synchronous_type_completion')
    for path in ('ydb/core/kqp/opt/kqp_opt_hash_func_propagate_transformer.cpp', 'ydb/core/kqp/opt/peephole/kqp_opt_peephole.cpp'):
        check('.AddTypeAnnotationTransformer()' in source[path] and '.AddPostTypeAnnotation(' in source[path]
              and 'InstantTransform(*TxTransformer, expr, ctx)' in source[path], 'existing_local_KQP_pipeline_precedent:' + path)
    check('TTransformationPipeline(&ctx.TypeCtx)' in source['ydb/core/kqp/opt/rbo/rules/constant_folding_stage.cpp'],
          'existing_RBO_pipeline_constructor_API_and_link_dependency')
    patch = ARTIFACTS / 'proposed-correctness-amendments/C41-final-query-standard-post-type-pipeline-20261028.patch'
    patch.write_text('diff --git a/' + TARGET + ' b/' + TARGET + '\n' + ''.join(difflib.unified_diff(
        before.splitlines(True), after.splitlines(True), fromfile='a/' + TARGET, tofile='b/' + TARGET)))
    with tempfile.TemporaryDirectory(prefix='c41-final-post-type-') as td:
        target = Path(td) / TARGET
        target.parent.mkdir(parents=True)
        target.write_text(before)
        subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(patch)], cwd=td,
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(['git', 'apply', str(patch)], cwd=td, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(target.read_text() == after, 'exact_disposable_patch_application')
    outcomes_path = ARTIFACTS / 'verification/C41-fullsuite-sanitized-outcomes-dd1d8cab-20261028.json'
    outcomes_raw = outcomes_path.read_bytes()
    outcomes = json.loads(outcomes_raw)
    check(outcomes['revision'] == REVISION and outcomes['counts'] == {'good': 139, 'fail': 209}, 'exact_current_suite_outcomes')
    logs = {path.name: path for path in LOG_ROOT.glob('chunk*/testing_out_stuff/*.err')}
    categories = collections.Counter()
    log_identities, unmatched = [], []
    for index, record in enumerate(outcomes['failures']):
        name = record['class'] + '.' + record['subtest'].replace('+', '.') + '.err'
        data = logs[name].read_bytes()
        identity = {'failure_index': index, 'sha256': sha(data), 'bytes': len(data)}
        log_identities.append(identity)
        if b'ConstrComplete' in data and b'2204' in data:
            categories['constraint_state_assertion'] += 1
        else:
            categories['separate_unclassified_explain_status_failure'] += 1
            unmatched.append(dict(identity, test_class=record['class'], subtest=record['subtest']))
    check(categories == {'constraint_state_assertion': 208, 'separate_unclassified_explain_status_failure': 1},
          'all209_logs_classified_without_copying_raw_diagnostics')
    report = {
        'schema': 'c41_final_standard_post_type_source_preparation_v1', 'revision_before_amendment': REVISION,
        'status': 'dormant_source_verified_native_pending',
        'patch': str(patch.relative_to(ARTIFACTS)), 'patch_sha256': sha(patch.read_bytes()),
        'production_target': TARGET, 'before_sha256': sha(raw[TARGET]), 'after_sha256': sha(after.encode()),
        'source_bindings': {path: sha(data) for path, data in raw.items()},
        'failure_evidence': {'outcomes': str(outcomes_path.relative_to(ARTIFACTS)), 'outcomes_sha256': sha(outcomes_raw),
            'counts': outcomes['counts'], 'crashes': 0, 'classified_failure_counts': dict(categories),
            'log_identity_vector_sha256': sha(json.dumps(log_identities, sort_keys=True).encode()),
            'log_count': len(log_identities), 'remaining_unclassified': unmatched,
            'raw_logs_copied_or_printed': False},
        'human_readable_cause': 'The previous boundary repair called canonicalization after scope completion but omitted constraint annotation. Newly rebuilt final nodes therefore reached CSEE before ConstrComplete, causing the observed assertion failures. The standard post-type pipeline supplies the missing prerequisite.',
        'repair': 'Replace the handwritten completion/CSEE tail by a local TypeAnnotation + AddPostTypeAnnotation pipeline and drive it with existing InstantTransform. The standard post-type stage runs Constraints, UpdateCompletness and CSEE in order with normal constraint validation.',
        'lifecycle': {
            'final_query_ownership_and_result_effect_mapping_changed': False,
            'old_scope_and_canonicalization_repairs_preserved_via_standard_pipeline': True,
            'closed_query_forSubGraph': False, 'constraint_checks_disabled': False,
            'local_repeat_restart_consumed': True, 'RBO_reentered_on_physical_query': False,
            'blocking_wait_added': False, 'unexpected_async': 'Error, as required by the existing synchronous final physical-builder contract',
            'cycle_detector': 'InstantTransform resets the shared expression cycle detector on entry and exit, as existing KQP per-transaction annotation/peephole wrappers do. This is an explicit API side effect, not a claim that the detector is isolated.',
            'repeat_budget': 'Uses the existing shared RepeatTransformCounter/Limit and fails on exhaustion; no reset or relaxed limit is added.',
            'service_GC_stage_added': False},
        'validation_required': [
            'Compile and rerun the complete original RBO unit-test suite on the exact amended C41 revision under the existing guard.',
            'Confirm the 208 constraint assertions are eliminated without restoring the original 87 dependency-scope crashes.',
            'Retain the one separately unclassified TPCDS_14 explain-status failure until the exact rerun or additional evidence resolves it.',
            'Rerun the existing 12 targeted C41 semantic/control cases with original oracles and independent main-based revision provenance.'],
        'tests_changed': False, 'production_live_helpers_or_Git_objects_modified': False,
        'generated_repro_inputs_or_outputs_persisted': False, 'native_build_toolchain_runs': 0,
        'source_checks_passed': len(checks), 'source_checks': checks,
    }
    output = ARTIFACTS / 'verification/C41-final-query-standard-post-type-pipeline-source-preparation-20261028.json'
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'patch_sha256': sha(patch.read_bytes()), 'report_sha256': sha(output.read_bytes()),
                      'checks_passed': len(checks), 'after_sha256': sha(after.encode())}))


if __name__ == '__main__':
    main()
