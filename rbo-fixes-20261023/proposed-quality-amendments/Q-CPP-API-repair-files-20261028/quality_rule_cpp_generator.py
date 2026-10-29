"""Generate anonymous production-rule contracts for Q14 and Q16.

No generated C++ source, diagnostics, or workloads are saved. These adapters
call real repository methods but remain uncompiled until the guarded build
series reaches quality verification. They establish local IR contracts only.
"""
import json


def cases():
    return [*(f'q14_{algo}_{left}_{right}' for algo in ('GraceJoin', 'ReverseBlockJoin', 'MapJoin')
              for left in (0, 2) for right in (0, 2)),
            *(f'q16_{storage}_{width}_{threshold}_{mode}' for storage in ('row', 'column', 'mixed', 'shared', 'unrelated')
              for width in (0, 1, 3) for threshold in (0, 2) for mode in ('Auto', 'Disable')),
            *(f'q16_window_{storage}_{enabled}_{threshold}_{mode}' for storage in ('row', 'column')
              for enabled in ('false', 'true') for threshold in (0, 2) for mode in ('Auto', 'Disable'))]


def _line(statement):
    return statement + ';\n'


def _check(expression):
    return _line('Check(' + expression + ')')


def _q14(case):
    _, algo, left, right = case.split('_')
    statements = ['const auto a = f.Id(), b = f.Id(), c = f.Id(), d = f.Id()',
                  'auto lhs = f.Read({a, b})', 'auto rhs = f.Read({c, d})',
                  'auto left = std::make_shared<NOpt::TRBORelOptimizerNode>(TVector<TString>{"left"}, NKikimr::NKqp::TOptimizerStatistics{}, lhs)',
                  'auto right = std::make_shared<NOpt::TRBORelOptimizerNode>(TVector<TString>{"right"}, NKikimr::NKqp::TOptimizerStatistics{}, rhs)',
                  'TVector<TJoinColumn> leftKeys{MakeCBOColumn("left", a), MakeCBOColumn("left", b)}',
                  'TVector<TJoinColumn> rightKeys{MakeCBOColumn("right", c), MakeCBOColumn("right", d)}',
                  'auto tree = std::make_shared<TJoinOptimizerNode>(left, right, leftKeys, rightKeys, EJoinKind::InnerJoin, NKikimr::NKqp::EJoinAlgoType::' + algo + ', false, false)']
    if left == '2':
        statements.append('tree->ShuffleLeftSideBy = {leftKeys[1], leftKeys[0]}')
    if right == '2':
        statements.append('tree->ShuffleRightSideBy = {rightKeys[1], rightKeys[0]}')
    statements += ['auto converted = ConvertOptimizedTree(tree, {}, f.Pos)',
                   'auto& result = CastOperator<TOpJoin>(*converted)']
    body = ''.join(map(_line, statements))
    body += _check('result.Props.JoinAlgo == NKikimr::NKqp::EJoinAlgoType::' + algo)
    if algo == 'MapJoin':
        body += _check('!result.Props.LeftShuffleBy.has_value() && !result.Props.RightShuffleBy.has_value()')
    else:
        for side, size, expected in (('Left', left, ('b', 'a')), ('Right', right, ('d', 'c'))):
            prop = 'result.Props.' + side + 'ShuffleBy'
            body += _check(prop + '.has_value()')
            body += _check(prop + '->Items().size() == ' + size)
            if size != '0':
                body += _check(prop + '->Items()[0] == ' + expected[0] + ' && ' + prop + '->Items()[1] == ' + expected[1])
    return body


def _q16_window(case):
    _, _, storage, enabled, threshold, mode = case.split('_')
    body = _line('f.Config->SetEnableWindowFunctionsV2(' + enabled + ')')
    body += _line('f.Config->SetDisableOlapBlocksOnColumnsLimit(' + threshold + ')')
    body += _line('f.TypeCtx.BlockEngineMode = NYql::EBlockEngineMode::' + mode)
    body += _line('const auto a = f.Id(), b = f.Id(), c = f.Id()')
    body += _line('auto read = f.Read({a, b, c})')
    if storage == 'column':
        body += _line('read->StorageType = NYql::EStorageType::ColumnStorage')
    body += _line('auto window = MakeIntrusive<TOpWindow>(read, f.Pos, TWindowIUs{}, TOrderedIUs<>{a}, TSortIUs{}, TOpWindowFrame{})')
    body += _line('TDisableBlocksOnColumnsLimitRule rule')
    body += _check('rule.QuickMatch(window)')
    body += _check('rule.SimpleMatchAndApply(window, f.RboCtx, f.Props).Get() == window.Get()')
    expected = 'Disable' if enabled == 'true' or mode == 'Disable' else 'Auto'
    body += _check('f.TypeCtx.BlockEngineMode == NYql::EBlockEngineMode::' + expected)
    return body


def _q16(case):
    if case.startswith('q16_window_'):
        return _q16_window(case)
    _, storage, width_text, threshold, mode = case.split('_')
    width = int(width_text)
    body = _line('f.Config->SetDisableOlapBlocksOnColumnsLimit(' + threshold + ')')
    body += _line('f.TypeCtx.BlockEngineMode = NYql::EBlockEngineMode::' + mode)
    body += _line('TUnorderedIUs columns')
    body += ''.join(_line('columns.Add(f.Id())') for _ in range(width))
    body += _line('auto row = f.Read(columns)')
    body += _line('auto column = MakeIntrusive<TOpRead>("t", columns, NYql::EStorageType::ColumnStorage, row->TableCallable, nullptr, nullptr, std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, f.Pos)')
    if storage in ('mixed', 'shared'):
        body += _line('TUnionAllIUs bindings(TUnionInputPolicy{2})')
        if storage == 'mixed':
            body += _line('TUnorderedIUs rightColumns')
            body += 'for (const auto id : columns) { const auto right = f.Id(); rightColumns.Add(right); bindings.Add(f.Id(), TUnionInputRow{{id, right}}); }\n'
            body += _line('column->GetColumns() = rightColumns')
            children = 'row, column'
        else:
            body += 'for (const auto id : columns) { bindings.Add(f.Id(), TUnionInputRow{{id, id}}); }\n'
            children = 'row, row'
        body += _line('TIntrusivePtr<IOperator> input = MakeIntrusive<TOpUnionAll>(' + children + ', f.Pos, std::move(bindings), false)')
    else:
        body += _line('TIntrusivePtr<IOperator> input = ' + ('column' if storage == 'column' else 'row'))
    body += _line('auto limit = MakeIntrusive<TOpLimit>(input, f.Pos, f.Constant(), EOpPhase::Final)')
    body += _line('TDisableBlocksOnColumnsLimitRule rule')
    body += _check('rule.QuickMatch(limit)')
    body += _check('rule.SimpleMatchAndApply(limit, f.RboCtx, f.Props).Get() == limit.Get()')
    disable = mode == 'Disable' or (storage in ('column', 'mixed') and int(threshold) > 0 and width >= int(threshold))
    body += _check('f.TypeCtx.BlockEngineMode == NYql::EBlockEngineMode::' + ('Disable' if disable else 'Auto'))
    return body


def render_cpp(case):
    if case not in cases():
        raise ValueError('Unknown rule contract case')
    text = ''.join('#include <' + path + '>\n' for path in (
        'ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_cbo.h',
        'ydb/core/kqp/opt/rbo/rules/kqp_cbo_trees.h', 'iostream', 'stdexcept'))
    text += 'using namespace NKikimr::NKqp;\nusing namespace NYql;\n'
    text += 'static unsigned Checks = 0;\nstatic void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }\n'
    text += 'int main() { try { NKikimr::NKqp::NTests::TIdTestContext f;\n'
    text += {'q14': _q14, 'q16': _q16}[case[:3]](case)
    text += 'std::cout << ' + json.dumps('{"checks_passed":') + ' << Checks << ' + json.dumps(',"status":"passed"}\n') + '; return 0;\n'
    text += '} catch (...) { std::cout << ' + json.dumps('{"status":"failed"}\n') + '; return 1; } }\n'
    return text


def render_cpp_suite(finding):
    """One anonymous translation unit per finding/branch, with serial cases."""
    if finding not in ('Q14', 'Q16'):
        raise ValueError('A suite must select one finding/branch')
    selected = [case for case in cases() if case.startswith(finding.lower() + '_')]
    text = render_cpp(selected[0]).split('int main()', 1)[0]
    for index, case in enumerate(selected):
        text += 'static void Case' + str(index) + '() { NKikimr::NKqp::NTests::TIdTestContext f;\n'
        text += {'Q14': _q14, 'Q16': _q16}[finding](case) + '}\n'
    text += 'int main() { unsigned failures = 0;\n'
    for index in range(len(selected)):
        prefix = '{"case_index":' + str(index) + ',"checks_executed":'
        text += '{ const auto before = Checks; bool passed = true; try { Case' + str(index) + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps(prefix) + ' << (Checks-before) << ' + json.dumps(',"passed":')
        text += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    text += 'return failures ? 1 : 0; }\n'
    return text


def execution_manifest():
    return {'status': 'generator_only_not_compiled_or_executed', 'cases': len(cases()),
            'Q14': 'Calls actual ConvertOptimizedTree with Grace/ReverseBlock/Map and empty/ordered nonempty shuffle vectors; tests engaged-empty versus absent output directives.',
            'Q16': 'Calls actual width-limit rule across row/column/mixed/shared/unrelated sources, thresholds, widths and block modes; also preserves the WindowFunctionsV2 guard independently of storage/threshold; observes the real TypeCtx mutation.',
            'source_transport': 'Anonymous memfd compiler input, exactly as quality_cpp_contract_generator execution manifest.',
            'batch_interface': 'render_cpp_suite(Q14 or Q16); one translation unit per selected branch with numeric JSONL case verdicts',
            'remaining': ['Compile adapters against selected-branch dependencies', 'Guarded baseline/fixed native execution',
                          'Q14 downstream physical stage edges and multinode semantics'],
            'production_sources_modified': False, 'rendered_sources_persisted': False}
