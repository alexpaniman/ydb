"""Generate anonymous C++ contracts that call the real production implementation.

No generated translation unit is written or executed here. Compile its returned
bytes from a memfd, link the selected branch's objects, and retain only binary/
source fingerprints plus numeric verdicts. Q19's wrappers replace the external
statistics-request boundary; the transformer callback and cache application run
unchanged from the selected production branch.
"""
import hashlib
import json
import re


def cases():
    return [*(f'q18_fields_{count}' for count in (1, 3, 7)),
            *(f'q19_{order}' for order in ('success_error', 'error_success', 'success_warning',
                                         'all_success', 'all_error', 'future_exception'))]


def _statement(expression):
    return expression + ';\n'


def _check(expression):
    return _statement('Check(' + expression + ')')


def _shared_prelude():
    includes = [
        'ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
        'ydb/core/kqp/opt/kqp_column_statistics_utils.h',
        'ydb/core/kqp/host/kqp_transform.h',
        'ydb/core/kqp/opt/cbo/solver/kqp_opt_predicate_selectivity.h',
        'ydb/library/actors/core/actorsystem_fwd.h',
        'yql/essentials/ast/yql_expr.h', 'yql/essentials/core/yql_graph_transformer.h',
        'yql/essentials/core/histogram/eq_height_histogram_reader.h',
        'iostream', 'stdexcept', 'vector', 'exception']
    text = ''.join('#include <' + include + '>\n' for include in includes)
    # Dependency headers are included first: the access adapter changes only
    # the transformer declaration in this anonymous verification translation unit.
    text += '#define private public\n#include <ydb/core/kqp/opt/rbo/kqp_rbo_transformer.h>\n#undef private\n'
    text += 'using namespace NKikimr;\nusing namespace NKikimr::NKqp;\nusing namespace NYql;\n'
    text += 'static unsigned Checks = 0;\nstatic void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }\n'
    return text


def _transformer_setup():
    statements = [
        'NKikimr::NKqp::NTests::TIdTestContext f',
        'auto optimizeContext = MakeIntrusive<NOpt::TKqpOptimizeContext>("ut", f.Config, f.QueryCtx, f.Tables, f.UserRequestContext)',
        'TAutoPtr<IGraphTransformer> annotation(new TNullTransformer())',
        'TKqpNewRBOTransformer transformer(optimizeContext, f.TypeCtx, std::move(annotation), *f.Tables, "ut", "/Root", nullptr, *f.FuncRegistry, nullptr)',
        'using TStatsMap = NYql::TOptimizerStatistics::TColumnStatMap']
    return ''.join(map(_statement, statements))


def _q18_body(count):
    body = _transformer_setup()
    body += _statement('auto cached = MakeIntrusive<TStatsMap>()')
    body += _statement('TStatsMap incoming')
    for index in range(count):
        key = json.dumps('field_' + str(index))
        prefix = 'cached->Data[' + key + ']'
        added = 'incoming.Data[' + key + ']'
        for statement in (prefix + '.NumUniqueVals = 17.0', prefix + '.Type = "Int64"',
                          added + '.NumUniqueVals = 0.0', added + '.HyperLogLog = 0.0'):
            body += _statement(statement)
    # Real statistics objects: identity/replacement/preservation, without
    # interpreting histogram estimates or invoking a model of the merger.
    body += _statement('auto oldSketch = std::shared_ptr<NKikimr::TCountMinSketch>(NKikimr::TCountMinSketch::Create(16, 2))')
    body += _statement('auto newSketch = std::shared_ptr<NKikimr::TCountMinSketch>(NKikimr::TCountMinSketch::Create(16, 2))')
    body += _statement('auto oldWidth = std::make_shared<NKikimr::TEqWidthHistogramEstimator>(std::make_shared<NKikimr::TEqWidthHistogram>())')
    body += _statement('auto newWidth = std::make_shared<NKikimr::TEqWidthHistogramEstimator>(std::make_shared<NKikimr::TEqWidthHistogram>())')
    body += _statement('auto oldHeight = std::make_shared<NKikimr::TEqHeightHistogram>(NKikimr::TEqHeightHistogramResult{})')
    body += _statement('auto newHeight = std::make_shared<NKikimr::TEqHeightHistogram>(NKikimr::TEqHeightHistogramResult{})')
    for index in range(count):
        key = json.dumps('field_' + str(index))
        body += _statement('cached->Data[' + key + '].CountMinSketch = oldSketch')
        body += _statement('cached->Data[' + key + '].EqWidthHistogramEstimator = oldWidth')
        body += _statement('incoming.Data[' + key + '].CountMinSketch = newSketch')
        body += _statement('incoming.Data[' + key + '].EqWidthHistogramEstimator = newWidth')
    body += _statement('cached->MultiData["tuple"].CountMinSketch = oldSketch')
    body += _statement('cached->MultiData["tuple"].EqHeightHistogram = oldHeight')
    body += _statement('cached->Data["unrelated"].NumUniqueVals = 31.0')
    body += _statement('cached->MultiData["tuple"].Columns = {"left", "right"}')
    body += _statement('cached->MultiData["tuple"].Types = {"Int64", "Int64"}')
    body += _statement('incoming.MultiData["tuple"] = NYql::TMultiColumnStatistics{}')
    body += _statement('incoming.MultiData["tuple"].CountMinSketch = newSketch')
    body += _statement('incoming.MultiData["tuple"].EqHeightHistogram = newHeight')
    body += _statement('f.TypeCtx.ColumnStatisticsByTableName["/Root/contract"] = cached')
    body += _statement('transformer.SharedState = std::make_shared<TColumnStatisticsSharedState>()')
    body += _statement('transformer.SharedState->Response.emplace()')
    body += _statement('transformer.SharedState->Response->ColumnStatisticsByTableName["/Root/contract"] = incoming')
    body += _statement('auto ready = NThreading::NewPromise<void>()')
    body += _statement('transformer.ColumnStatisticsReadiness = ready.GetFuture()')
    body += _statement('ready.SetValue()')
    body += _statement('transformer.ApplyColumnStatistics()')
    body += _check('f.TypeCtx.ColumnStatisticsByTableName.at("/Root/contract").Get() == cached.Get()')
    for index in range(count):
        prefix = 'cached->Data.at(' + json.dumps('field_' + str(index)) + ')'
        body += _check(prefix + '.NumUniqueVals.has_value() && *' + prefix + '.NumUniqueVals == 0.0')
        body += _check(prefix + '.HyperLogLog.has_value() && *' + prefix + '.HyperLogLog == 0.0')
        body += _check(prefix + '.Type == "Int64"')
        body += _check(prefix + '.CountMinSketch == newSketch')
        body += _check(prefix + '.EqWidthHistogramEstimator == newWidth')
    body += _check('*cached->Data.at("unrelated").NumUniqueVals == 31.0')
    body += _check('cached->MultiData.at("tuple").Columns.size() == 2')
    body += _check('cached->MultiData.at("tuple").Types.size() == 2')
    body += _check('cached->MultiData.at("tuple").CountMinSketch == newSketch')
    body += _check('cached->MultiData.at("tuple").EqHeightHistogram == newHeight')
    body += _statement('TStatsMap replacement')
    for index in range(count):
        body += _statement('replacement.Data[' + json.dumps('field_' + str(index)) + '].Type = "Uint64"')
    body += _statement('replacement.MultiData["tuple"].Columns = {"changed"}')
    body += _statement('replacement.MultiData["tuple"].Types = {"Uint64"}')
    body += _statement('transformer.SharedState->Response->ColumnStatisticsByTableName["/Root/contract"] = replacement')
    body += _statement('transformer.ApplyColumnStatistics()')
    for index in range(count):
        prefix = 'cached->Data.at(' + json.dumps('field_' + str(index)) + ')'
        body += _check(prefix + '.Type == "Uint64"')
        body += _check(prefix + '.CountMinSketch == newSketch && ' + prefix + '.EqWidthHistogramEstimator == newWidth')
        body += _check(prefix + '.NumUniqueVals.has_value() && *' + prefix + '.NumUniqueVals == 0.0')
        body += _check(prefix + '.HyperLogLog.has_value() && *' + prefix + '.HyperLogLog == 0.0')
    body += _check('cached->MultiData.at("tuple").Columns == TVector<TString>{"changed"}')
    body += _check('cached->MultiData.at("tuple").Types == TVector<TString>{"Uint64"}')
    body += _check('cached->MultiData.at("tuple").CountMinSketch == newSketch && cached->MultiData.at("tuple").EqHeightHistogram == newHeight')
    body += _statement('TStatsMap absent')
    body += _statement('transformer.SharedState->Response->ColumnStatisticsByTableName["/Root/contract"] = absent')
    body += _statement('transformer.ApplyColumnStatistics()')
    body += _statement('transformer.SharedState->Response->ColumnStatisticsByTableName["/Root/contract"] = incoming')
    body += _statement('transformer.ApplyColumnStatistics()')
    body += _check('cached->Data.size() == ' + str(count + 1))
    body += _check('f.TypeCtx.ColumnStatisticsByTableName.at("/Root/contract").Get() == cached.Get()')
    for index in range(count):
        prefix = 'cached->Data.at(' + json.dumps('field_' + str(index)) + ')'
        body += _check(prefix + '.Type == "Uint64"')
        body += _check(prefix + '.CountMinSketch == newSketch && ' + prefix + '.EqWidthHistogramEstimator == newWidth')
    body += _check('cached->MultiData.at("tuple").Columns == TVector<TString>{"changed"}')
    body += _check('cached->MultiData.at("tuple").Types == TVector<TString>{"Uint64"}')
    body += _check('cached->MultiData.at("tuple").CountMinSketch == newSketch && cached->MultiData.at("tuple").EqHeightHistogram == newHeight')
    return body


def _q19_wrappers(symbols):
    if set(symbols) != {'single', 'tuple'} or not all(re.fullmatch(r'_Z[A-Za-z0-9_]+', symbol) for symbol in symbols.values()):
        raise ValueError('Supply the two actual defined AddStatRequest mangled symbols from the selected production object')
    text = 'static TVector<NThreading::TPromise<TColumnStatisticsResponse>> Scripted;\nstatic size_t NextRequest = 0;\n'
    common = 'NActors::TActorSystem*, TVector<NThreading::TFuture<TColumnStatisticsResponse>>& futures, TKikimrTablesData&, const TString&, const TString&, TTypeAnnotationContext&, NStat::EStatType, '
    for key, columns, value in (
            ('single', 'THashMap<TString, THashSet<TString>>', 'NYql::TColumnStatistics'),
            ('tuple', 'THashMap<TString, THashMap<TString, TVector<TString>>>', 'NYql::TMultiColumnStatistics')):
        signature = 'void Wrap_' + key + '(' + common + 'const ' + columns + '&, std::function<bool(const ' + value + '&)>)'
        text += signature + ' asm(' + json.dumps('__wrap_' + symbols[key]) + ');\n'
        text += signature + ' { Check(NextRequest < Scripted.size()); futures.push_back(Scripted[NextRequest++].GetFuture()); }\n'
    return text


def _q19_body(order):
    variants = {'success_error': ('success', 'error', 'empty'), 'error_success': ('error', 'success', 'empty'),
                'success_warning': ('success', 'warning', 'empty'), 'all_success': ('success', 'success', 'empty'),
                'all_error': ('error', 'error', 'error'), 'future_exception': ('success', 'exception', 'empty')}
    states = variants[order]
    body = _transformer_setup()
    body += ''.join(_statement('Scripted.push_back(NThreading::NewPromise<TColumnStatisticsResponse>())') for _ in states)
    body += _statement('transformer.RequestColumnStatistics(f.ExprCtx)')
    body += _check('NextRequest == Scripted.size()')
    body += _check('!transformer.ColumnStatisticsReadiness.IsReady()')
    for index, state in enumerate(states):
        if state == 'exception':
            body += _statement('Scripted[' + str(index) + '].SetException(std::make_exception_ptr(std::runtime_error("scripted")))')
            continue
        var = 'response_' + str(index)
        body += _statement('TColumnStatisticsResponse ' + var)
        if state in ('success', 'warning'):
            body += _statement(var + '.ColumnStatisticsByTableName["/Root/contract"].Data[' + json.dumps('field_' + str(index)) + '].NumUniqueVals = ' + str(index + 11) + '.0')
        if state in ('error', 'warning'):
            issue = 'issue_' + str(index)
            body += _statement('TIssue ' + issue + '(' + json.dumps(issue) + ')')
            if state == 'warning':
                body += _statement(issue + '.SetCode(0, TSeverityIds::S_WARNING)')
            body += _statement(var + '.AddIssue(' + issue + ')')
        body += _statement('Scripted[' + str(index) + '].SetValue(std::move(' + var + '))')
    if order == 'future_exception':
        body += 'bool propagated = false;\ntry { transformer.ColumnStatisticsReadiness.GetValueSync(); } catch (...) { propagated = true; }\n'
        body += _check('propagated')
    else:
        body += _statement('transformer.ColumnStatisticsReadiness.GetValueSync()')
        body += _check('transformer.SharedState && transformer.SharedState->Response.has_value()')
        body += _check('transformer.SharedState->Response->Issues().Size() == ' + str(sum(state in ('error', 'warning') for state in states)))
        body += _statement('transformer.ApplyColumnStatistics()')
        populated = sum(state in ('success', 'warning') for state in states)
        if populated:
            body += _check('f.TypeCtx.ColumnStatisticsByTableName.at("/Root/contract")->Data.size() == ' + str(populated))
            for index, state in enumerate(states):
                if state in ('success', 'warning'):
                    body += _check('*f.TypeCtx.ColumnStatisticsByTableName.at("/Root/contract")->Data.at(' + json.dumps('field_' + str(index)) + ').NumUniqueVals == ' + str(index + 11) + '.0')
        else:
            body += _check('f.TypeCtx.ColumnStatisticsByTableName.empty()')
    return body


def render_cpp(case, wrapper_symbols=None):
    if case not in cases():
        raise ValueError('Unknown generated C++ contract')
    text = _shared_prelude()
    if case.startswith('q18_'):
        body = _q18_body(int(case.rsplit('_', 1)[1]))
    else:
        text += _q19_wrappers(wrapper_symbols or {})
        body = _q19_body(case.removeprefix('q19_'))
    text += 'int main() { try {\n' + body
    text += 'std::cout << ' + json.dumps('{"checks_passed":') + ' << Checks << ' + json.dumps(',"status":"passed"}\n') + '; return 0;\n'
    text += '} catch (...) { std::cout << ' + json.dumps('{"status":"failed"}\n') + '; return 1; } }\n'
    return text


def render_cpp_suite(finding, wrapper_symbols=None):
    """One anonymous translation unit per branch, amortizing fixture linkage."""
    if finding not in ('Q18', 'Q19'):
        raise ValueError('A suite must select one finding/branch')
    selected = [case for case in cases() if case.startswith(finding.lower() + '_')]
    text = _shared_prelude()
    if finding == 'Q19':
        text += _q19_wrappers(wrapper_symbols or {})
    for index, case in enumerate(selected):
        body = _q18_body(int(case.rsplit('_', 1)[1])) if finding == 'Q18' else (
            'Scripted.clear(); NextRequest = 0;\n' + _q19_body(case.removeprefix('q19_')))
        text += 'static void Case' + str(index) + '() {\n' + body + '}\n'
    text += 'int main() { unsigned failures = 0;\n'
    for index in range(len(selected)):
        prefix = '{"case_index":' + str(index) + ',"checks_executed":'
        text += '{ const auto before = Checks; bool passed = true; try { Case' + str(index) + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps(prefix) + ' << (Checks-before) << ' + json.dumps(',"passed":')
        text += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    text += 'return failures ? 1 : 0; }\n'
    return text


def execution_manifest():
    return {
        'status': 'generator_only_not_compiled_or_executed', 'cases': cases(),
        'production_sources_modified': False,
        'includes_existing_fixture': 'ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
        'source_transport': 'os.memfd_create; compiler reads inherited /proc/self/fd descriptor; no rendered source file',
        'batch_interface': 'render_cpp_suite(Q18 or Q19, wrapper_symbols); one translation unit per selected branch with numeric JSONL case verdicts',
        'build_integration': 'Reuse the selected branch existing RBO unit-test compile/link dependency closure. One compile/link at a time under the same memory guard. Resolve resource/compiler flags from build metadata; no guessed ABI or mixed-revision objects.',
        'q19_link_boundary': 'AddStatRequest has two externally linked non-inline definitions in kqp_column_statistics_utils.cpp; transformer callsites are in a separate translation unit. Discover exact defined mangled symbols with nm and pass --wrap for both. Confirm relocations before execution; LTO/inlining can invalidate wrapping.',
        'q19_wrap_scope': 'Only external request dispatch is substituted. Production RequestColumnStatistics callback, issues accumulation, cache application and future exception propagation execute from the selected branch.',
        'pre_execution_checks': ['same branch binary/library provenance', 'both wrapped symbols actually referenced', 'NextRequest exactly three', 'future remains pending before scripted responses', 'sanitized JSON parsed and positive check count'],
        'existing_smoke_coverage': ['KqpRbo ColumnStatistics row/column twins use successful real statistics service; they do not establish partial-response behavior', 'KqpRboGlobalIUs existing Map movement controls cover surrounding invariants, not every per-definition flag edge'],
        'remaining': ['Compile generated adapters against actual production headers before accepting signatures', 'Resolve linker object closure and symbols safely', 'Execute Q18 cache application and Q19 async matrix', 'Q18 sketch/histogram identity, optional zero, type/vector replacement and absent-field preservation extension is prepared; compilation and native execution remain pending', 'Q14/Q16 adapters are separate in quality_rule_cpp_generator; Q25 adapter is in quality_map_contract_generator; none are compiled proof'],
        'artifact_policy': 'Keep generator source, binary/build hashes, numeric verdicts and sanitized counters only; never persist rendered C++ workloads or raw compiler diagnostics that quote generated lines.'}
