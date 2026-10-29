"""Anonymous local contracts for Q09/Q20 production statistics methods.

Only this generator and sanitized profile metadata are persisted. Generated C++
is returned to the caller in memory. These contracts do not establish query
semantics or measured optimization quality.
"""
import json

VERSION = 'q09_q20_production_statistics_contracts_v1'


def specs(finding=None):
    result = []
    widths = (
        ('ordinary', '64.0', '1024.0', 2, 1, '8.0', True),
        ('multiple_outputs', '64.0', '1024.0', 2, 3, '24.0', True),
        ('empty', '0.0', '0.0', 2, 1, '8.0', True),
        ('empty_width', '0.0', '1024.0', 2, 1, '8.0', True),
        ('zero_input_columns', '64.0', '1024.0', 0, 1, '8.0', True),
        ('zero_output_columns', '64.0', '1024.0', 2, 0, '8.0', True),
        ('zero_width', '64.0', '0.0', 2, 1, '8.0', True),
        ('negative_width', '64.0', '-64.0', 2, 1, '8.0', True),
        ('nan_width', '64.0', 'std::numeric_limits<double>::quiet_NaN()', 2, 1, '8.0', True),
        ('infinite_width', '64.0', 'std::numeric_limits<double>::infinity()', 2, 1, '8.0', True),
        ('underflow_width', 'std::numeric_limits<double>::max()', 'std::numeric_limits<double>::denorm_min()', 2, 1, '8.0', True),
        ('overflow_width', 'std::numeric_limits<double>::min()', 'std::numeric_limits<double>::max()', 2, 1, '8.0', True),
        ('absent_cost', '64.0', '1024.0', 2, 1, '8.0', False),
    )
    for phase in ('Undefined', 'Final'):
        for name, rows, size, before_columns, after_columns, output_size, cost in widths:
            result.append(dict(name='q09_scalar_' + phase.lower() + '_' + name,
                bug='Q09', phase=phase, shape='scalar', rows=rows, size=size,
                before_columns=before_columns, after_columns=after_columns,
                output_size=output_size, cost=cost, control=False))
    for phase in ('Undefined', 'Intermediate', 'Final'):
        for shape in ('grouped', 'distinct', 'grouping_only'):
            result.append(dict(name='q09_' + shape + '_' + phase.lower() + '_control',
                bug='Q09', phase=phase, shape=shape, rows='64.0', size='1024.0',
                before_columns=2, after_columns=2 if shape == 'grouped' else 1,
                output_size='1024.0' if shape == 'grouped' else '512.0', cost=True, control=True))
    result.append(dict(name='q09_scalar_intermediate_control', bug='Q09', phase='Intermediate',
        shape='scalar', rows='64.0', size='1024.0', before_columns=2, after_columns=1,
        output_size='512.0', cost=True, control=True))
    for missing in ('input_statistics', 'output_metadata'):
        result.append(dict(name='q09_missing_' + missing + '_control', bug='Q09',
            shape='missing', missing=missing, control=True))
    for shape in ('ordinary', 'self', 'nested_left', 'nested_right', 'left_semi', 'right_semi', 'left_only', 'right_only'):
        for hint in ('none', 'left_rows', 'right_rows', 'union_rows', 'left_bytes', 'right_bytes', 'union_bytes', 'absent_rows', 'absent_bytes'):
            result.append(dict(name='q20_' + shape + '_' + hint, bug='Q20', shape=shape,
                hint=hint, control=hint in ('none', 'absent_rows', 'absent_bytes')))
    return [item for item in result if finding is None or item['bug'] == finding]


def cases():
    return [item['name'] for item in specs()]


def _stmt(text):
    return text + ';\n'


def _check(text):
    return _stmt('Check(' + text + ')')


def _vector(items):
    return 'TVector<TString>{' + ', '.join(json.dumps(item) for item in items) + '}'


def _q09(spec):
    missing = spec['shape'] == 'missing'
    s = dict(phase='Undefined', shape='scalar', rows='64.0', size='1024.0', before_columns=2,
             after_columns=1, cost=True) if missing else spec
    body = _stmt('const auto a = f.Id(), b = f.Id()')
    body += _stmt('auto input = f.Read({a, b})')
    body += _stmt('input->Props.Metadata = TRBOMetadata{}')
    body += _stmt('input->Props.Metadata->ColumnsCount = ' + str(s['before_columns']))
    body += _stmt('input->Props.Statistics = TRBOStatistics{}')
    for member, value in (('ERows', s['rows']), ('EBytes', s['size']), ('Selectivity', '0.25')):
        body += _stmt('input->Props.Statistics->' + member + ' = ' + value)
    if s['cost']:
        body += _stmt('input->Props.Cost = 37.0')
    body += _stmt('TAggregationIUs aggregations')
    if s['shape'] != 'grouping_only':
        for _ in range(max(1, s['after_columns'] - (s['shape'] == 'grouped'))):
            body += _stmt('aggregations.Add(f.Id(), TOpAggregationTraits{.Input = a, .AggFunction = "count"})')
    body += _stmt('TOrderedIUs<> keys')
    if s['shape'] in ('grouped', 'grouping_only'):
        body += _stmt('keys.Append(b)')
    body += _stmt('auto aggregate = MakeIntrusive<TOpAggregate>(input, std::move(aggregations), std::move(keys), EOpPhase::' + s['phase'] + ', ' + ('true' if s['shape'] == 'distinct' else 'false') + ', f.Pos)')
    body += _stmt('aggregate->Props.Metadata = TRBOMetadata{}')
    body += _stmt('aggregate->Props.Metadata->ColumnsCount = ' + str(s['after_columns']))
    if missing:
        body += _stmt(('input->Props.Statistics' if spec['missing'] == 'input_statistics' else 'aggregate->Props.Metadata') + '.reset()')
        body += _stmt('aggregate->ComputeStatistics(f.RboCtx, f.Props)')
        body += _check('!aggregate->Props.Statistics.has_value()')
        body += _check('!aggregate->Props.Cost.has_value()')
        return body
    body += _stmt('const auto before = *input->Props.Statistics')
    body += _stmt('aggregate->ComputeStatistics(f.RboCtx, f.Props)')
    body += _check('aggregate->Props.Statistics.has_value()')
    expected_rows = s['rows'] if spec['control'] else '1.0'
    expected_selectivity = '0.25' if spec['control'] else '1.0'
    body += _check('aggregate->Props.Statistics->ERows == ' + expected_rows)
    body += _check('std::isfinite(aggregate->Props.Statistics->EBytes) && aggregate->Props.Statistics->EBytes == ' + s['output_size'])
    body += _check('aggregate->Props.Statistics->Selectivity == ' + expected_selectivity)
    body += _check('aggregate->Props.Cost == input->Props.Cost')
    body += _check('Same(input->Props.Statistics->ERows, before.ERows) && Same(input->Props.Statistics->EBytes, before.EBytes) && Same(input->Props.Statistics->Selectivity, before.Selectivity)')
    body += _stmt('const auto consumer = BuildOptimizerStatistics(*aggregate, f.Props.ColumnLineage, true, f.TypeCtx)')
    body += _check('consumer.Nrows == ' + expected_rows + ' && consumer.ByteSize == ' + s['output_size'])
    body += _check('consumer.Selectivity == ' + expected_selectivity + ' && consumer.Cost == ' + ('37.0' if s['cost'] else '0.0'))
    return body


def _q20(spec):
    shape, hint = spec['shape'], spec['hint']
    self_join = shape == 'self'
    body = _stmt('auto a = Leaf(f, ' + json.dumps('t' if self_join else 'a') + ', 10.0, 160.0, 3.0)')
    body += _stmt('auto b = Leaf(f, ' + json.dumps('t' if self_join else 'b') + ', 20.0, 320.0, 5.0)')
    body += _stmt('TIntrusivePtr<IOperator> left = a.Op, right = b.Op')
    aliases_left, aliases_right, aliases_union = ['a'], ['b'], ['a', 'b']
    if self_join:
        aliases_left, aliases_right, aliases_union = ['t'], ['t'], ['t', 't_#1']
    if shape.startswith('nested_'):
        body += _stmt('auto c = Leaf(f, "c", 30.0, 480.0, 7.0)')
        body += _stmt('auto nested = MakeIntrusive<TOpJoin>(' + ('a.Op, b.Op' if shape == 'nested_left' else 'b.Op, c.Op') + ', f.Pos, "Cross", TJoinIUs{})')
        body += _stmt('nested->Props.JoinAlgo = NKikimr::NKqp::EJoinAlgoType::GraceJoin')
        body += _stmt('nested->ComputeMetadata(f.RboCtx, f.Props)')
        body += _stmt('nested->ComputeStatistics(f.RboCtx, f.Props)')
        body += _stmt('left = ' + ('nested' if shape == 'nested_left' else 'a.Op'))
        body += _stmt('right = ' + ('c.Op' if shape == 'nested_left' else 'nested'))
        aliases_left, aliases_right, aliases_union = (['a', 'b'], ['c'], ['a', 'b', 'c']) if shape == 'nested_left' else (['a'], ['b', 'c'], ['a', 'b', 'c'])
    kind = {'left_semi': 'LeftSemi', 'right_semi': 'RightSemi', 'left_only': 'LeftOnly', 'right_only': 'RightOnly'}.get(shape, 'Cross')
    body += _stmt('TJoinIUs keys' + ('{{a.Key, b.Key}}' if kind != 'Cross' else '{}'))
    body += _stmt('auto join = MakeIntrusive<TOpJoin>(left, right, f.Pos, ' + json.dumps(kind) + ', std::move(keys))')
    body += _stmt('join->Props.JoinAlgo = NKikimr::NKqp::EJoinAlgoType::GraceJoin')
    body += _check('f.Props.ColumnLineage.GetAliases(left->Props.Metadata->HintRelations) == ' + _vector(aliases_left))
    body += _check('f.Props.ColumnLineage.GetAliases(right->Props.Metadata->HintRelations) == ' + _vector(aliases_right))
    body += _stmt('const auto beforeLeft = *left->Props.Statistics, beforeRight = *right->Props.Statistics')
    body += _stmt('auto expectedLeft = BuildOptimizerStatistics(*left, f.Props.ColumnLineage, true, f.TypeCtx)')
    body += _stmt('auto expectedRight = BuildOptimizerStatistics(*right, f.Props.ColumnLineage, true, f.TypeCtx)')
    body += _stmt('TOptimizerHints hints')
    body += _stmt('std::optional<TCardinalityHints::TCardinalityHint> expectedRowsHint, expectedBytesHint')
    if hint != 'none':
        target, dimension = hint.split('_')
        labels = {'left': aliases_left, 'right': aliases_right, 'union': aliases_union, 'absent': ['unrelated']}[target]
        factor = {'left_rows': 2, 'right_rows': 3, 'union_rows': 5, 'left_bytes': 7, 'right_bytes': 11, 'union_bytes': 887, 'absent_rows': 13, 'absent_bytes': 17}[hint]
        operation = 'Replace' if hint == 'union_bytes' else 'Multiply'
        vector = 'CardinalityHints' if dimension == 'rows' else 'BytesHints'
        body += _stmt('hints.' + vector + '->PushBack(' + _vector(labels) + ', TCardinalityHints::' + operation + ', ' + str(factor) + '.0, "numeric-contract")')
        if target == 'union':
            body += _stmt(('expectedRowsHint' if dimension == 'rows' else 'expectedBytesHint') + ' = hints.' + vector + '->Hints.front()')
        else:
            for side, aliases in (('Left', aliases_left), ('Right', aliases_right)):
                if len(aliases) == 1 and labels == aliases:
                    body += _stmt('expected' + side + ('.Nrows' if dimension == 'rows' else '.ByteSize') + ' *= ' + str(factor) + '.0')
    body += _stmt('f.Config->OptimizerHints = hints')
    body += _stmt('f.KqpCtx.Hints.reset()')
    body += _stmt('join->ComputeMetadata(f.RboCtx, f.Props)')
    expected_outputs = aliases_left if shape.startswith('left_') else aliases_right if shape.startswith('right_') else aliases_union
    body += _check('join->Props.Metadata.has_value() && f.Props.ColumnLineage.GetAliases(join->Props.Metadata->HintRelations) == ' + _vector(expected_outputs))
    if hint != 'none':
        body += _check('f.KqpCtx.GetOptimizerHints().' + vector + '->Hints.front().Applied == ' + ('true' if target == 'union' else 'false'))
    body += _stmt('TVector<TJoinColumn> leftKeys, rightKeys')
    if kind != 'Cross':
        body += _stmt('leftKeys.push_back(MakeCBOColumn("a", a.Key))')
        body += _stmt('rightKeys.push_back(MakeCBOColumn("b", b.Key))')
    body += _stmt('const auto expected = f.RboCtx.CBOCtx.ComputeJoinStatsV2(expectedLeft, expectedRight, leftKeys, rightKeys, NKikimr::NKqp::EJoinAlgoType::GraceJoin, ConvertToJoinKind(' + json.dumps(kind) + '), expectedRowsHint ? &*expectedRowsHint : nullptr, false, false, expectedBytesHint ? &*expectedBytesHint : nullptr)')
    body += _stmt('join->ComputeStatistics(f.RboCtx, f.Props)')
    body += _check('join->Props.Statistics.has_value() && join->Props.Cost.has_value()')
    for actual, expected in (('ERows', 'Nrows'), ('EBytes', 'ByteSize'), ('Selectivity', 'Selectivity')):
        body += _check('Near(join->Props.Statistics->' + actual + ', expected.' + expected + ')')
    body += _check('Near(*join->Props.Cost, expected.Cost)')
    body += _check('SameStats(*left->Props.Statistics, beforeLeft) && SameStats(*right->Props.Statistics, beforeRight)')
    body += _stmt('const auto first = *join->Props.Statistics')
    body += _stmt('const auto firstCost = join->Props.Cost')
    body += _stmt('join->ComputeStatistics(f.RboCtx, f.Props)')
    body += _check('SameStats(*join->Props.Statistics, first) && join->Props.Cost == firstCost')
    if hint != 'none':
        body += _check('f.KqpCtx.GetOptimizerHints().' + vector + '->Hints.front().Applied == ' + ('true' if target == 'union' else 'false'))
    return body


def expected_checks(finding):
    return [(_q09(spec) if finding == 'Q09' else _q20(spec)).count('Check(') for spec in specs(finding)]


def render_cpp_suite(finding):
    selected = specs(finding)
    if finding not in ('Q09', 'Q20') or not selected:
        raise ValueError('Select Q09 or Q20')
    text = ''.join('#include <' + path + '>\n' for path in (
        'ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_cbo.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_utils.h', 'cmath', 'limits', 'iostream', 'stdexcept'))
    text += 'using namespace NKikimr::NKqp;\n'
    text += 'static unsigned Checks = 0; static void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }\n'
    text += 'static bool Same(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }\n'
    text += 'static bool Near(double a, double b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a-b) <= 1e-10 * std::max(1.0, std::abs(b)); }\n'
    text += 'static bool SameStats(const TRBOStatistics& a, const TRBOStatistics& b) { return Same(a.ERows,b.ERows) && Same(a.EBytes,b.EBytes) && Same(a.Selectivity,b.Selectivity); }\n'
    if finding == 'Q20':
        text += 'struct TLeaf { TIntrusivePtr<TOpRead> Op; TInfoUnitId Key; };\n'
        text += 'static TLeaf Leaf(NKikimr::NKqp::NTests::TIdTestContext& f, TString alias, double rows, double bytes, double cost) {\n'
        text += 'const auto a = f.Id(), b = f.Id(); auto op = f.Read({a,b}); op->Props.Metadata = TRBOMetadata{}; op->Props.Metadata->ColumnsCount = 2; op->Props.Statistics = TRBOStatistics{}; op->Props.Statistics->ERows = rows; op->Props.Statistics->EBytes = bytes; op->Props.Cost = cost;\n'
        text += 'const auto relation = f.Props.ColumnLineage.AddRelation(alias); for (const auto id : op->GetOutputIUs()) { op->Props.Metadata->HintRelations.Add(id, relation); f.Props.ColumnLineage.Add(id, TColumnLineageEntry{.SourceAlias=alias, .ColumnName=::ToString(id), .Relation=relation}); } return {op,a}; }\n'
    for index, spec in enumerate(selected):
        text += 'static void Case' + str(index) + '() { NKikimr::NKqp::NTests::TIdTestContext f;\n'
        text += (_q09(spec) if finding == 'Q09' else _q20(spec)) + '}\n'
    text += 'int main() { unsigned failures = 0;\n'
    for index in range(len(selected)):
        text += '{ const auto before = Checks; bool passed = true; try { Case' + str(index) + '(); } catch (...) { passed=false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps('{"case_index":' + str(index) + ',"checks_executed":') + ' << (Checks-before) << ' + json.dumps(',"passed":') + ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    return text + 'return failures ? 1 : 0; }\n'
