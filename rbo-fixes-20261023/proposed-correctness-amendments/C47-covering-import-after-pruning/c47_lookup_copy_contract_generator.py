"""Generate local C47 production-IR copy contracts into anonymous memory.

This adapter requires the C47 ReadSettings interface. It compares the original
C47 commit with its amendment, or verifies the amendment alone; it is not a
pinned-main query verifier. No C++ source or diagnostics are persisted here.
"""
import json
import hashlib


def cases():
    return [*(f'plain_{nullable}_{width}_{requested}'
              for nullable in (0, 1) for width in (1, 3) for requested in ('fresh', 'renamed')),
            'plain_settings', 'plain_ranges', 'guard_join', 'guard_fetched_filter',
            'guard_prefix', 'guard_source_read', 'guard_residual', 'guard_chained',
            'guard_named_kind', 'input_limit', 'input_unrepeatable_settings']


def _line(statement):
    return statement + ';\n'


def _check(expression):
    return _line('Check(' + expression + ')')


def _body(case):
    if case not in cases():
        raise ValueError('Unknown C47 copy contract')
    width = int(case.split('_')[2]) if case.startswith('plain_') and case.count('_') == 3 else 3
    nullable = case.startswith('plain_1_') or case in ('plain_settings', 'plain_ranges')
    names = ('z', 'a', 'm')[:width]
    body = 'NKikimr::NKqp::NTests::TIdTestContext f;\n'
    body += _line('TVector<TInfoUnitId> inputs, outputs')
    body += _line('TUnorderedIUs readColumns, fetchedColumns')
    body += _line('TOpTableLookup::TLookupKeys keys')
    for name in names:
        body += _line('inputs.push_back(f.Id(' + json.dumps(name) + '))')
        body += _line('outputs.push_back(f.Id(' + json.dumps(name) + '))')
        body += _line('readColumns.Add(inputs.back())')
        body += _line('fetchedColumns.Add(outputs.back())')
        body += _line('keys.Append(inputs.back(), ' + json.dumps(name) + ')')
    body += _line('auto input = f.Read(readColumns)')
    if case == 'guard_join':
        body += _line('auto lookup = MakeIntrusive<TOpTableLookup>(input, f.Pos, input->TableCallable, fetchedColumns, keys, "Left", std::nullopt)')
    else:
        body += _line('auto lookup = MakeIntrusive<TOpTableLookup>(input, f.Pos, input->TableCallable, fetchedColumns, keys)')
    body += _line('lookup->AllowNullKeys = ' + ('true' if nullable else 'false'))
    if case == 'plain_settings':
        body += _line('input->ReadSettings = f.ExprCtx.NewList(f.Pos, {f.ExprCtx.NewList(f.Pos, {f.ExprCtx.NewAtom(f.Pos, "Sorted")})})')
    elif case == 'plain_ranges':
        body += _line('TOpRead::TRangeInfo ranges')
        body += _line('ranges.ComputeNode = f.ExprCtx.NewList(f.Pos, {})')
        body += _line('ranges.KeyColumns = {"z", "a", "m"}')
        body += _line('input->RangeInfo = std::move(ranges)')
    elif case == 'guard_fetched_filter':
        body += _line('lookup->FetchedRowFilter = f.Column(outputs.front())')
    elif case == 'guard_prefix':
        body += _line('lookup->Prefix.emplace()')
    elif case == 'guard_source_read':
        body += _line('lookup->SourceRead = TOpTableLookup::TSourceRead{f.Column(outputs.front()), {}}')
    elif case == 'guard_residual':
        body += _line('lookup->ResidualJoinKeys.Add(inputs.front(), outputs.front())')
    elif case == 'guard_chained':
        body += _line('lookup->KeysFromInputLookup = true')
    elif case == 'guard_named_kind':
        body += _line('lookup->JoinKind = "Left"')
    elif case == 'input_limit':
        body += _line('input->Limit = f.Constant().GetExpressionBody()')
    elif case == 'input_unrepeatable_settings':
        body += _line('input->ReadSettings = f.ExprCtx.NewList(f.Pos, {f.ExprCtx.NewCallable(f.Pos, "Udf", {})})')
    rejected = case.startswith('guard_')
    body += _check(('!' if rejected else '') + 'CanDuplicateOperator(*lookup)')
    body += _check(('!' if rejected or case.startswith('input_') else '') + 'CanDuplicateSubtree(*lookup, f.Props.Subplans)')
    if case.startswith('input_'):
        return body
    body += _line('TSubstitutions renames')
    if case.endswith('_renamed'):
        body += _line('const auto requested = f.Id("different_storage_name")')
        body += _line('renames.Add(outputs.front(), requested)')
    body += _line('auto result = lookup->Copy(f.Props, renames)')
    if rejected:
        return body + _check('!result')
    body += _check('!!result && result->GetKind() == EOperator::TableLookup')
    body += _line('auto& copied = CastOperator<TOpTableLookup>(*result)')
    body += _check('copied.Table.Get() == lookup->Table.Get()')
    body += _check('copied.AllowNullKeys == lookup->AllowNullKeys && !copied.IsJoin()')
    body += _check('copied.GetInput().Get() != input.Get()')
    body += _check('copied.LookupKeys.Items().size() == keys.Items().size()')
    body += _check('copied.GetColumns().Size() == fetchedColumns.Size()')
    body += _check('!copied.GetColumns().HasAny(fetchedColumns)')
    body += _check('!copied.GetColumns().HasAny(copied.GetInput()->GetOutputIUs())')
    body += _check('copied.LookupKeys.Unordered().IsSubsetOf(copied.GetInput()->GetOutputIUs())')
    body += 'for (size_t index = 0; index < inputs.size(); ++index) {\n'
    body += _check('copied.LookupKeys.Items()[index].first == renames.At(inputs[index])')
    body += _check('copied.LookupKeys.Items()[index].second == keys.Items()[index].second')
    body += _check('copied.GetColumns().Contains(renames.At(outputs[index]))')
    body += _check('f.Props.InfoUnitRegistry.Get(renames.At(outputs[index])).GetColumnName() == f.Props.InfoUnitRegistry.Get(outputs[index]).GetColumnName()')
    body += '}\n'
    body += _check('lookup->LookupKeys == keys && lookup->GetColumns() == fetchedColumns')
    body += _line('auto& copiedRead = CastOperator<TOpRead>(*copied.GetInput())')
    body += _check('copiedRead.ReadSettings.Get() == input->ReadSettings.Get()')
    if case == 'plain_ranges':
        body += _check('copiedRead.RangeInfo && copiedRead.RangeInfo->ComputeNode.Get() == input->RangeInfo->ComputeNode.Get()')
        body += _check('copiedRead.RangeInfo->KeyColumns == input->RangeInfo->KeyColumns')
    if case.endswith('_renamed'):
        body += _check('renames.At(outputs.front()) != requested')
    return body


def _prelude():
    text = ''.join('#include <' + path + '>\n' for path in (
        'ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
        'ydb/core/kqp/opt/rbo/copy_logical_subtree.h',
        'ydb/core/kqp/host/kqp_transform.h', 'iostream', 'stdexcept'))
    return text + ('using namespace NKikimr::NKqp;\nusing namespace NYql;\n'
                   'static unsigned Checks = 0;\n'
                   'static void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }\n')


def render_cpp_suite(selected=None):
    selected = cases() if selected is None else list(selected)
    if not selected or len(set(selected)) != len(selected) or any(case not in cases() for case in selected):
        raise ValueError('Expected distinct C47 copy contracts')
    text = _prelude()
    for index, case in enumerate(selected):
        text += 'static void Case' + str(index) + '() {\n' + _body(case) + '}\n'
    text += 'int main() { unsigned failures = 0;\n'
    for index in range(len(selected)):
        text += '{ const auto before = Checks; bool passed = true; try { Case' + str(index) + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps('{"case_index":' + str(index) + ',"checks_executed":')
        text += ' << (Checks-before) << ' + json.dumps(',"passed":')
        text += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    return text + 'return failures ? 1 : 0; }\n'


def execution_manifest():
    source = render_cpp_suite()
    return {'status': 'generator_only_not_compiled_or_executed', 'case_count': len(cases()),
            'rendered_suite_sha256': hashlib.sha256(source.encode()).hexdigest(),
            'rendered_suite_bytes': len(source.encode()),
            'required_interface': 'C47 ReadSettings; pre-amendment C47 or amended C47',
            'scope': 'Copy/repeatability and IU/storage-key contracts; native query coverage is separate',
            'production_calls': ['CanDuplicateOperator', 'CanDuplicateSubtree', 'IOperator::Copy']}
