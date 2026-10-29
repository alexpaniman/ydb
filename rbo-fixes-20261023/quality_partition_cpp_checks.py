"""Build anonymous C++ contracts for the Q15 production helper and metadata.

No generated translation unit, IR fixture or native payload is persisted here.
The caller compiles against the selected branch with its normal dependencies.
"""
import hashlib
import json


def cases():
    return ['window_subset', 'window_order', 'window_duplicate', 'window_alias_subset',
            'window_extra_computed', 'window_missing', 'window_computed_replacement',
            'window_config_disabled', 'column_subset', 'row_metadata_rejected',
            'full_metadata_rejected', 'exclusion_metadata_rejected', 'mixed_join_metadata_rejected',
            'assigned_connections_authoritative', 'three_window_subset']


def _line(statement):
    return statement + ';\n'


def _check(expression):
    return _line('Check(' + expression + ')')


def _window(name, child, keys):
    return _line('auto ' + name + ' = MakeIntrusive<TOpWindow>(' + child +
                 ', f.Pos, TWindowIUs{}, TOrderedIUs<>{' + ','.join(keys) + '}, TSortIUs{}, TOpWindowFrame{})')


def source(case):
    if case not in cases():
        raise ValueError('Unknown partition contract')
    includes = ['ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
                'ydb/core/kqp/opt/rbo/kqp_rbo_utils.h', 'iostream', 'stdexcept']
    result = ''.join('#include <' + name + '>\n' for name in includes)
    result += 'using namespace NKikimr;\nusing namespace NKikimr::NKqp;\nusing namespace NYql;\n'
    result += 'static unsigned Checks=0;\nstatic void Check(bool value) { ++Checks; if(!value) throw std::runtime_error("contract"); }\n'
    result += 'static void Run() {\n'
    body = _line('NTests::TIdTestContext f') + _line('f.Config->SetDefaultEnableShuffleElimination(true)')
    for name in ('a', 'b', 'c', 'q', 'x', 'r'):
        body += _line('const auto ' + name + ' = f.Id(' + json.dumps(name) + ')')
    body += _line('auto read = f.Read({a,b,c})')
    body += _line('read->Props.Metadata.emplace()')
    body += _line('read->Props.Metadata->ShuffledByColumns = TOrderedIUs<>{a}')
    if case == 'column_subset' or case.startswith('assigned_'):
        body += _line('read->StorageType = EStorageType::ColumnStorage')
    if case.startswith('assigned_'):
        body += _line('read->Props.Metadata->ShuffledByColumns = TOrderedIUs<>{c}')
    first_keys = ('a', 'b') if case in ('window_order', 'window_missing',
                                      'assigned_connections_authoritative') else (
        ('a', 'a') if case == 'window_duplicate' else ('a',))
    body += _window('first', 'read', first_keys)
    body += _line('first->ComputeMetadata(f.RboCtx, f.Props)')
    body += _line('TIntrusivePtr<IOperator> input = first')
    keys, expected = ('a', 'b'), first_keys
    if case in ('column_subset', 'row_metadata_rejected'):
        body += _line('input = read')
        expected = ('a',) if case == 'column_subset' else ()
    elif case == 'window_order':
        keys = ('b', 'a')
    elif case == 'window_missing':
        keys, expected = ('a',), ()
    elif case in ('window_alias_subset', 'window_extra_computed', 'window_computed_replacement'):
        body += _line('TMapIUs definitions')
        body += _line('definitions.Add(q, f.Column(a))')
        body += _line('definitions.Add(x, f.Constant())')
        body += _line('input = MakeIntrusive<TOpMap>(input, f.Pos, std::move(definitions))')
        body += _line('input->ComputeMetadata(f.RboCtx, f.Props)')
        keys = ('q', 'x') if case != 'window_computed_replacement' else ('x', 'b')
        expected = ('q',) if case != 'window_computed_replacement' else ()
    elif case in ('full_metadata_rejected', 'exclusion_metadata_rejected', 'mixed_join_metadata_rejected'):
        kind = {'full_metadata_rejected': 'Full', 'exclusion_metadata_rejected': 'Exclusion',
                'mixed_join_metadata_rejected': 'Inner'}[case]
        body += _line('auto right = f.Read({r})')
        if case == 'mixed_join_metadata_rejected':
            body += _line('f.SetType(*read)')
            body += _line('const auto* rightField = f.ExprCtx.MakeType<TItemExprType>(ToString(r), f.ExprCtx.MakeType<TDataExprType>(EDataSlot::Uint64))')
            body += _line('right->Type = f.ExprCtx.MakeType<TListExprType>(f.ExprCtx.MakeType<TStructExprType>(TVector<const TItemExprType*>{rightField}))')
        body += _line('TJoinIUs joinKeys') + _line('joinKeys.Add({a,r,false})')
        body += _line('input = MakeIntrusive<TOpJoin>(read, right, f.Pos, ' + json.dumps(kind) + ', std::move(joinKeys))')
        body += _line('input->Props.Metadata.emplace()')
        body += _line('input->Props.Metadata->ShuffledByColumns = TOrderedIUs<>{a}')
        expected = ()
    elif case == 'window_config_disabled':
        body += _line('f.Config->SetDefaultEnableShuffleElimination(false)')
        expected = ()
    if case.startswith('assigned_'):
        keys, expected = ('a', 'b', 'c'), ('a', 'b')
    body += _window('window', 'input', keys)
    body += _check('GetWindowPreservedShuffling(*window, f.RboCtx).Items() == TOrderedIUs<>{' + ','.join(expected) + '}.Items()')
    body += _line('window->ComputeMetadata(f.RboCtx, f.Props)')
    actual = expected or keys
    body += _check('window->Props.Metadata->ShuffledByColumns.Items() == TOrderedIUs<>{' + ','.join(actual) + '}.Items()')
    if case == 'three_window_subset':
        body += _window('third', 'window', ('a',))
        body += _check('GetWindowPreservedShuffling(*third, f.RboCtx).Items() == TOrderedIUs<>{a}.Items()')
        body += _line('third->ComputeMetadata(f.RboCtx, f.Props)')
        body += _check('third->Props.Metadata->ShuffledByColumns.Items() == TOrderedIUs<>{a}.Items()')
    if case.startswith('assigned_'):
        body += _line('auto root = f.Root(window, TOrderedIUs<TString>{{a,"a"},{b,"b"},{c,"c"}})')
        body += _line('TAssignStagesStage stage') + _line('stage.RunStage(*root, f.RboCtx)')
        body += _line('auto firstEdge = root->PlanProps.StageGraph.TryGetConnection(*read->Props.StageId, *first->Props.StageId)')
        body += _line('auto nextEdge = root->PlanProps.StageGraph.TryGetConnection(*first->Props.StageId, *window->Props.StageId)')
        body += _check('firstEdge && IsConnection<TShuffleConnection>(firstEdge)')
        body += _check('nextEdge && IsConnection<TMapConnection>(nextEdge)')
        body += _line('read->Props.Metadata->ShuffledByColumns = TOrderedIUs<>{a}')
        body += _line('first->ComputeMetadata(f.RboCtx, root->PlanProps)')
        body += _line('window->ComputeMetadata(f.RboCtx, root->PlanProps)')
        body += _check('first->Props.Metadata->ShuffledByColumns.Items() == TOrderedIUs<>{a,b}.Items()')
        body += _check('window->Props.Metadata->ShuffledByColumns.Items() == TOrderedIUs<>{a,b}.Items()')
    result += body + '}\n'
    result += 'int main() { try { Run(); std::cout << "{\\"contract_pass\\":true,\\"checks\\":" << Checks << "}\\n"; return 0; } catch (...) { std::cout << "{\\"contract_pass\\":false,\\"checks\\":" << Checks << "}\\n"; return 1; } }\n'
    return result.encode()


def metadata(case):
    content = source(case)
    return {'case': case, 'generated_source_sha256': hashlib.sha256(content).hexdigest(),
            'production_objects_required': True, 'compile_or_native_run_performed': False,
            'generated_source_saved': False, 'production_edited': False}


def render_cpp_suite():
    """One anonymous fixed-only suite; baseline has no production helper."""
    first = source(cases()[0]).decode()
    prefix = first.split('static void Run() {', 1)[0]
    functions = []
    for index, case in enumerate(cases()):
        unit = source(case).decode()
        function = 'static void Run() {' + unit.split('static void Run() {', 1)[1].split('int main()', 1)[0]
        functions.append(function.replace('static void Run()', 'static void Run' + str(index) + '()', 1))
    main = ['int main() { bool allPassed = true;']
    for index in range(len(cases())):
        main.append('{ Checks = 0; bool passed = true; try { Run' + str(index) + '(); } catch (...) { passed = false; }')
        message = '{"case_index":' + str(index) + ',"checks_executed":'
        main.append('std::cout << ' + json.dumps(message) + ' << Checks << '
                    + json.dumps(',"passed":') + ' << (passed ? "true" : "false") << "}\\n";')
        main.append('allPassed = allPassed && passed; }')
    main.append('return allPassed ? 0 : 1; }')
    return prefix + ''.join(functions) + '\n'.join(main) + '\n'
