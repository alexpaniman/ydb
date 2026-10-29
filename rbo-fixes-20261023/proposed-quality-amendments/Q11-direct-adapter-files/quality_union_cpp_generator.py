"""Anonymous local contracts for the actual Q11 UNION filter rewrite.

Only compositional generators are stored. Translation units remain in memory.
These structural contracts grant neither query semantics nor plan improvement.
"""
import json


def cases():
    return ['plain', 'ordered', 'three', 'three_ordered', 'shared', 'shared_ordered',
            'nullable', 'constant', 'partial', 'subplan', 'external', 'optional_late',
            'primitive_late', 'volatile', 'fallible', 'unknown', 'arithmetic',
            'nested_volatile', 'nested_fallible']


class _Body:
    def __init__(self):
        self.parts = []
        self.checks = 0

    def line(self, value):
        self.parts.append(value + ';\n')

    def check(self, value):
        self.line('Check(' + value + ')')
        self.checks += 1

    def member(self, expression, node, expected):
        self.line('CheckMember(' + expression + ', ' + node + ', ' + expected + ')')
        self.checks += 4


def _body(case):
    if case not in cases():
        raise ValueError('unknown UNION contract')
    b = _Body()
    positive = case in cases()[:8]
    shared = case.startswith('shared')
    count = 3 if case.startswith('three') else 2
    ordered = case.endswith('ordered') or not positive
    b.line('NKikimr::NKqp::NTests::TIdTestContext f')
    b.line('const auto outA = f.Id("outA"), outB = f.Id("outB")')
    b.line('TVector<TIntrusivePtr<IOperator>> inputs')
    source_a, source_b = [], []
    for index in range(count):
        if shared and index == 1:
            b.line('inputs.push_back(inputs[0])')
            source_a.append('a0')
            source_b.append('b0')
        else:
            a, c = 'a' + str(index), 'b' + str(index)
            b.line('const auto ' + a + ' = f.Id("a"), ' + c + ' = f.Id("b")')
            b.line('inputs.push_back(f.Read({' + a + ', ' + c + '}))')
            b.line('f.SetType(*inputs.back()' + (', {' + a + ', ' + c + '}' if case == 'nullable' else '') + ')')
            source_a.append(c if index == 1 else a)
            source_b.append(a if index == 1 else c)
    b.line('TUnionAllIUs columns(TUnionInputPolicy{' + str(count) + '})')
    b.line('columns.Add(outA, TUnionInputRow{{' + ', '.join(source_a) + '}})')
    b.line('columns.Add(outB, TUnionInputRow{{' + ', '.join(source_b) + '}})')
    b.line('auto merge = MakeIntrusive<TOpUnionAll>(inputs, f.Pos, columns, ' + str(ordered).lower() + ')')
    b.line('f.SetType(*merge' + (', {outA, outB}' if case in ('nullable', 'optional_late') else '') + ')')
    if case == 'optional_late':
        b.line('f.SetType(*inputs[0], {a0, b0})')
    if case == 'primitive_late':
        b.line('SetPrimitiveType(f, *inputs[0], EDataSlot::Int64)')
        b.line('SetPrimitiveType(f, *merge, EDataSlot::Int64)')
    b.line('auto predicate = MakeBinaryPredicate(">", f.Column(outA), f.Column(outB))')
    if case == 'constant':
        b.line('predicate = MakeConstant("Bool", "true", f.Pos, &f.ExprCtx)')
        b.line('predicate.BindPlanProps(&f.Props)')
    elif case in ('volatile', 'nested_volatile'):
        b.line('TExpression random(f.ExprCtx.NewCallable(f.Pos, "RandomNumber", {}), &f.ExprCtx, &f.Props)')
        b.line('auto risky = MakeBinaryPredicate(">", random, MakeConstant("Double", "0", f.Pos, &f.ExprCtx))')
        b.line('predicate = ' + ('MakeConjunction({predicate, risky})' if case.startswith('nested_') else 'risky'))
    elif case in ('fallible', 'nested_fallible'):
        b.line('auto risky = MakeEnsure(predicate, MakeConstant("Bool", "false", f.Pos, &f.ExprCtx), "contract")')
        b.line('predicate = ' + ('MakeConjunction({predicate, risky})' if case.startswith('nested_') else 'risky'))
    elif case == 'unknown':
        b.line('predicate = MakeBinaryPredicate(">", MakeUnaryCallable("Abs", f.Column(outA)), f.Column(outB))')
    elif case == 'arithmetic':
        b.line('predicate = MakeBinaryPredicate(">", MakeBinaryPredicate("/", f.Column(outA), f.Column(outB)), f.Column(outB))')
    elif case == 'external':
        b.line('const auto external = f.Id("external")')
        b.line('predicate = MakeBinaryPredicate(">", f.Column(external), f.Column(outA))')
    elif case == 'subplan':
        b.line('const auto binding = f.Id("binding"), result = f.Id("result")')
        b.line('auto subplan = f.Read({result})')
        b.line('f.SetType(*subplan)')
        b.line('f.Props.Subplans.Add(binding, subplan, ESubplanType::EXPR, TOrderedIUs<>{}, result)')
        b.line('predicate = MakeBinaryPredicate(">", f.Column(binding), f.Column(outA))')
    b.line('auto filter = MakeIntrusive<TOpFilter>(merge, f.Pos, predicate)')
    if case == 'partial':
        b.line('filter->PartiallyPushedDown = true')
    b.line('auto observer = MakeIntrusive<TOpFilter>(inputs[0], f.Pos, MakeBinaryPredicate(">", f.Column(a0), f.Column(b0)))')
    b.line('const auto observerNode = observer->GetFilterExpression().Node')
    b.line('const auto originalNode = filter->GetFilterExpression().Node')
    b.line('const auto originalUsed = filter->GetFilterExpression().GetRawInputIUs()')
    b.line('const auto originalType = merge->Type')
    for index in range(count):
        b.line('const auto outputs' + str(index) + ' = inputs[' + str(index) + ']->GetOutputIUs()')
        b.line('const auto type' + str(index) + ' = inputs[' + str(index) + ']->Type')
    b.line('TPushFilterThroughUnionAllRule rule')
    b.check('rule.QuickMatch(filter)')
    b.check('!rule.QuickMatch(merge)')
    b.check('!rule.QuickMatch(observer)')
    b.line('auto rewritten = rule.SimpleMatchAndApply(filter, f.RboCtx, f.Props)')
    b.check('filter->GetInput().Get() == merge.Get()')
    b.check('filter->GetFilterExpression().Node == originalNode')
    b.check('filter->GetFilterExpression().GetRawInputIUs() == originalUsed')
    b.check('filter->PartiallyPushedDown == ' + str(case == 'partial').lower())
    b.check('merge->Ordered == ' + str(ordered).lower())
    b.check('merge->GetChildCount() == ' + str(count))
    b.check('merge->GetColumns().Keys() == columns.Keys()')
    b.check('merge->GetColumns().At(outA).Inputs == columns.At(outA).Inputs')
    b.check('merge->GetColumns().At(outB).Inputs == columns.At(outB).Inputs')
    b.check('merge->Type == originalType')
    b.check('observer->GetInput().Get() == inputs[0].Get()')
    b.check('observer->GetFilterExpression().Node == observerNode')
    for index in range(count):
        i = str(index)
        b.check('merge->GetInput(' + i + ').Get() == inputs[' + i + '].Get()')
        b.check('inputs[' + i + ']->GetOutputIUs() == outputs' + i)
        b.check('inputs[' + i + ']->Type == type' + i)
    if not positive:
        b.check('rewritten.Get() == filter.Get()')
        if case == 'subplan':
            b.check('f.Props.Subplans.At(binding).Plan.Get() == subplan.Get()')
        return ''.join(b.parts), b.checks
    b.check('rewritten.Get() != filter.Get() && rewritten.Get() != merge.Get()')
    b.check('rewritten->Kind == EOperator::UnionAll')
    b.line('const auto pushed = CastOperator<TOpUnionAll>(rewritten)')
    b.check('pushed->Ordered == merge->Ordered')
    b.check('pushed->GetChildCount() == ' + str(count))
    b.check('pushed->GetOutputIUs() == merge->GetOutputIUs()')
    b.check('pushed->GetColumns().Policy().ChildCount == columns.Policy().ChildCount')
    b.check('pushed->GetColumns().At(outA).Inputs == columns.At(outA).Inputs')
    b.check('pushed->GetColumns().At(outB).Inputs == columns.At(outB).Inputs')
    for index, (a, c) in enumerate(zip(source_a, source_b)):
        i = str(index)
        b.check('pushed->GetInput(' + i + ')->Kind == EOperator::Filter')
        b.line('const auto branch' + i + ' = CastOperator<TOpFilter>(pushed->GetInput(' + i + '))')
        b.check('branch' + i + '.Get() != filter.Get() && branch' + i + '.Get() != observer.Get()')
        for previous in range(index):
            b.check('branch' + i + '.Get() != branch' + str(previous) + '.Get()')
        b.check('branch' + i + '->GetInput().Get() == inputs[' + i + '].Get()')
        b.check('branch' + i + '->GetOutputIUs() == outputs' + i)
        b.check('!branch' + i + '->PartiallyPushedDown')
        b.line('const auto& expression' + i + ' = branch' + i + '->GetFilterExpression()')
        b.line('const auto body' + i + ' = expression' + i + '.GetExpressionBody()')
        if case == 'constant':
            b.check('expression' + i + '.GetRawInputIUs().Empty()')
            b.check('body' + i + '->IsCallable("Bool") && body' + i + '->ChildrenSize() == 1')
            b.check('body' + i + '->Child(0)->Content() == "true"')
        else:
            b.check('expression' + i + '.GetRawInputIUs() == TUnorderedIUs{' + a + ', ' + c + '}')
            b.check('expression' + i + '.Node != originalNode')
            b.check('body' + i + '->IsCallable(">") && body' + i + '->ChildrenSize() == 2')
            b.member('expression' + i, 'body' + i + '->Child(0)', a)
            b.member('expression' + i, 'body' + i + '->Child(1)', c)
    return ''.join(b.parts), b.checks


def expected_checks():
    return [_body(case)[1] for case in cases()]


def render_cpp_suite():
    includes = ('ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h', 'iostream', 'stdexcept')
    text = ''.join('#include <' + path + '>\n' for path in includes)
    text += 'using namespace NKikimr::NKqp;\nusing namespace NYql;\n'
    text += 'static unsigned Checks = 0;\nstatic void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }\n'
    text += 'static void CheckMember(const TExpression& expression, const TExprNode* member, TInfoUnitId expected) {\n'
    text += 'const auto lambda = expression.GetLambda();\n'
    text += 'Check(lambda->ChildrenSize() == 2 && lambda->Child(0)->ChildrenSize() == 1);\n'
    text += 'Check(member && member->IsCallable("Member") && member->ChildrenSize() == 2);\n'
    text += 'Check(member->Child(0) == lambda->Child(0)->Child(0)); Check(GetMemberId(*member) == expected); }\n'
    text += 'static void SetPrimitiveType(NKikimr::NKqp::NTests::TIdTestContext& f, IOperator& op, EDataSlot slot) {\n'
    text += 'TVector<const TItemExprType*> fields; for (const auto id : op.GetOutputIUs()) fields.push_back(f.ExprCtx.MakeType<TItemExprType>(f.ExprCtx.GetIndexAsString(id), f.ExprCtx.MakeType<TDataExprType>(slot)));\n'
    text += 'op.Type = f.ExprCtx.MakeType<TListExprType>(f.ExprCtx.MakeType<TStructExprType>(fields)); }\n'
    for index, case in enumerate(cases()):
        text += 'static void Run' + str(index) + '() {\n' + _body(case)[0] + '}\n'
    text += 'int main() { unsigned failures = 0;\n'
    for index in range(len(cases())):
        prefix = '{"case_index":' + str(index) + ',"checks_executed":'
        text += '{ const auto before = Checks; bool passed = true; try { Run' + str(index) + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps(prefix) + ' << (Checks - before) << ' + json.dumps(',"passed":')
        text += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    text += 'return failures ? 1 : 0; }\n'
    return text
