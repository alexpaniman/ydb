"""Anonymous production-rule contracts for C31 identity admission and bindings.

No generated translation unit or IR fixture is persisted or executed here.
These contracts observe the actual rewrite; they are not query-result evidence.
"""
import hashlib
import json


def cases():
    return ['c31_ir_' + shape + '__' + kind
            for shape in ('zero', 'domain_nullable', 'unique_nullable', 'unique_control',
                          'reject_float', 'reject_volatile', 'reject_subplan', 'producer_guard',
                          'equi_keys', 'left_residual', 'subplan_capture')
            for kind in ('Left', 'LeftSemi', 'LeftOnly')]


def _line(value):
    return value + ';\n'


def _check(value):
    return _line('Check(' + value + ')')


def _prelude():
    includes = ('ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
                'ydb/core/kqp/opt/rbo/copy_logical_subtree.h', 'iostream', 'stdexcept', 'string_view', 'utility')
    text = ''.join('#include <' + value + '>\n' for value in includes)
    text += 'using namespace NKikimr::NKqp;\nusing namespace NYql;\n'
    text += 'static unsigned Checks = 0;\nstatic void Check(bool value) { ++Checks; if (!value) throw std::runtime_error("contract"); }\n'
    text += r'''
static void CheckMember(const TExpression& expression, const TExprNode* member, TInfoUnitId expected) {
    const auto lambda = expression.GetLambda();
    Check(lambda->ChildrenSize() == 2 && lambda->Child(0)->ChildrenSize() == 1);
    Check(member && member->IsCallable("Member") && member->ChildrenSize() == 2);
    Check(member->Child(0) == lambda->Child(0)->Child(0));
    Check(GetMemberId(*member) == expected);
}
static std::pair<TInfoUnitId, bool> IdentitySource(IOperator& branch, TInfoUnitId key) {
    if (branch.Kind != EOperator::Map) return {key, false};
    const auto* element = CastOperator<TOpMap>(branch).GetMapElements().Find(key);
    if (!element) return {key, false};
    const auto& expression = element->GetExpression();
    const auto body = expression.GetExpressionBody();
    Check(body->IsCallable("StablePickle") && body->ChildrenSize() == 1);
    const auto& inputs = expression.GetRawInputIUs();
    Check(inputs.Size() == 1);
    const auto source = *inputs.begin();
    CheckMember(expression, body->Child(0), source);
    return {source, true};
}
'''
    return text


def _binding_contract(shape, kind):
    """Inspect actual rewritten bindings; no SQL or physical execution claim."""
    zero = shape == 'zero'
    unique = shape.startswith('unique_')
    optional = shape in ('domain_nullable', 'unique_nullable')
    body = _line('const auto& renames = secondaryPort->GetRebindings()')
    body += _line('auto outerStream = reconstruction.GetLeftInput()')
    body += _line('auto filteredStream = reconstruction.GetRightInput()')
    if optional:
        body += _check('outerStream->Kind == EOperator::Map && filteredStream->Kind == EOperator::Map')
        body += _line('outerStream = CastOperator<TOpMap>(*outerStream).GetInput()')
        body += _line('filteredStream = CastOperator<TOpMap>(*filteredStream).GetInput()')
    body += _check('outerStream.Get() == primaryPort')
    body += _check('filteredStream->Kind == EOperator::Filter')
    body += _line('auto& residual = CastOperator<TOpFilter>(*filteredStream)')
    body += _check('residual.GetInput()->Kind == EOperator::Join')
    body += _line('auto& inner = CastOperator<TOpJoin>(*residual.GetInput())')
    body += _check('inner.GetRightInput().Get() == right.Get()')
    body += _check('inner.JoinKind == ' + json.dumps('Inner' if shape == 'equi_keys' else 'Cross'))
    body += _check('inner.JoinFilters.empty() && reconstruction.JoinFilters.empty()')
    body += _check('inner.JoinKeys.Items().size() == originalKeys.Items().size()')
    body += 'for (const auto& before : originalKeys.Items()) Check(inner.JoinKeys.Contains({renames.At(before.first), before.second, before.EqualNulls}));\n'
    if unique:
        body += _check('inner.GetLeftInput().Get() == secondaryPort')
    else:
        body += _check('inner.GetLeftInput()->Kind == EOperator::Aggregate')
        body += _line('auto& domain = CastOperator<TOpAggregate>(*inner.GetLeftInput())')
        body += _check('domain.GetInput().Get() == secondaryPort')
        body += _check('domain.GetKeyColumns().Unordered() == secondaryPort->GetOutputIUs()')
    body += _line('TUnorderedIUs seenSources')
    body += _line('const auto expectedSources = ' + ('originalIdentityKeys' if unique else 'primaryPort->GetInput()->GetOutputIUs()'))
    body += 'for (const auto& key : reconstruction.JoinKeys.Items()) {\n'
    body += 'const auto [source, outerEncoded] = IdentitySource(*reconstruction.GetLeftInput(), key.first);\n'
    body += 'const auto [innerSource, innerEncoded] = IdentitySource(*reconstruction.GetRightInput(), key.second);\n'
    body += 'Check(expectedSources.Contains(source)); Check(!seenSources.Contains(source)); seenSources.Add(source);\n'
    body += 'Check(innerSource == renames.At(source)); Check(!key.EqualNulls);\n'
    body += 'Check(outerEncoded == ' + ('(source == a)' if optional else 'false') + '); Check(innerEncoded == outerEncoded);\n}\n'
    body += _check('seenSources == expectedSources')
    body += _line('const auto& rewrittenPredicate = residual.GetFilterExpression()')
    body += _line('const auto predicateBody = rewrittenPredicate.GetExpressionBody()')
    body += _check('predicateBody->IsCallable(">") && predicateBody->ChildrenSize() == 2')
    if shape == 'left_residual':
        body += _check('rewrittenPredicate.GetRawInputIUs() == TUnorderedIUs{renames.At(b), r}')
        body += _line('CheckMember(rewrittenPredicate, predicateBody->Child(0), renames.At(b))')
        body += _line('CheckMember(rewrittenPredicate, predicateBody->Child(1), r)')
    elif shape == 'subplan_capture':
        body += _check('rewrittenPredicate.GetRawInputIUs() == TUnorderedIUs{binding, r}')
        body += _line('CheckMember(rewrittenPredicate, predicateBody->Child(0), binding)')
        body += _line('CheckMember(rewrittenPredicate, predicateBody->Child(1), r)')
        body += _line('const auto& call = f.Props.Subplans.At(binding)')
        body += _check('call.Plan.Get() == capturedPlan.Get() && call.Type == ESubplanType::EXPR')
        body += _check('call.ResultIU && *call.ResultIU == capturedResult && call.Tuple.Items().empty()')
        body += _check('call.DependentIUs == TUnorderedIUs{renames.At(a), r}')
        body += _check('captures->GetDependencies().Keys() == TUnorderedIUs{localLeft, localRight}')
        body += _check('captures->GetDependencies().At(localLeft).Outer == renames.At(a)')
        body += _check('captures->GetDependencies().At(localRight).Outer == r')
        body += _check('capturedPlan->GetMapElements().At(capturedResult).GetExpression().Node == originalLocalExpression')
        body += _check('capturedPlan->GetMapElements().At(capturedResult).GetExpression().GetRawInputIUs() == TUnorderedIUs{localLeft, localRight}')
        body += _check('rewrittenPredicate.GetInputIUs(false, true) == TUnorderedIUs{renames.At(a), r}')
    else:
        body += _check('rewrittenPredicate.GetRawInputIUs() == TUnorderedIUs{r}')
        body += _line('CheckMember(rewrittenPredicate, predicateBody->Child(0), r)')
        body += _check('predicateBody->Child(1)->IsCallable("Int32") && predicateBody->Child(1)->ChildrenSize() == 1')
        body += _check('predicateBody->Child(1)->Child(0)->Content() == "0"')
    body += _line('projection->SetInput(rewritten)')
    body += _check('projection->GetColumns() == originalProjection')
    body += _check('projection->GetOutputIUs() == originalOutputs')
    body += _check('originalOutputs.IsSubsetOf(rewritten->GetOutputIUs())')
    body += _check(('originalRightIUs.IsSubsetOf(rewritten->GetOutputIUs())' if kind == 'Left'
                    else '!originalRightIUs.HasAny(rewritten->GetOutputIUs())'))
    if zero:
        body += _check('projection->GetColumns().Items().size() == ' + ('1' if kind == 'Left' else '0'))
    return body


def _body(case):
    if case not in cases():
        raise ValueError('Unknown C31 IR contract')
    shape, kind = case.removeprefix('c31_ir_').split('__')
    zero = shape == 'zero'
    unique = shape.startswith('unique_')
    optional = shape in ('domain_nullable', 'unique_nullable')
    rejected = shape.startswith('reject_')
    body = _line('NKikimr::NKqp::NTests::TIdTestContext f')
    body += _line('f.Config->SetUseBlockHashJoin(false)')
    body += _line('const auto a = f.Id("a"), b = f.Id("b"), r = f.Id("r")')
    if shape == 'equi_keys':
        body += _line('const auto s = f.Id("s")')
    body += _line('TIntrusivePtr<IOperator> left = f.Read(TUnorderedIUs{' + ('' if zero else 'a,b') + '})')
    if shape == 'producer_guard':
        body += _line('const auto computed = f.Id("computed")')
        body += _line('TMapIUs elements')
        body += _line('elements.Add(computed, MakeUnaryCallable("Udf", f.Column(a)))')
        body += _line('left = MakeIntrusive<TOpMap>(left, f.Pos, std::move(elements))')
    body += _line('f.SetType(*left' + (', TUnorderedIUs{a}' if optional else '') + ')')
    if shape == 'reject_float':
        body += _line('TVector<const TItemExprType*> fields')
        body += 'for (const auto id : left->GetOutputIUs()) fields.push_back(f.ExprCtx.MakeType<TItemExprType>(ToString(id), f.ExprCtx.MakeType<TDataExprType>(EDataSlot::Double)));\n'
        body += _line('left->Type = f.ExprCtx.MakeType<TListExprType>(f.ExprCtx.MakeType<TStructExprType>(fields))')
    body += _line('left->Props.Metadata.emplace()')
    if unique:
        body += _line('left->Props.Metadata->KeyColumns = TOrderedIUs<>{a,b}')
    body += _line('auto right = f.Read({' + ('r,s' if shape == 'equi_keys' else 'r') + '})')
    body += _line('f.SetType(*right)')
    body += _line('auto predicate = MakeBinaryPredicate(">", f.Column(r), MakeConstant("Int32", "0", f.Pos, &f.ExprCtx))')
    if shape == 'reject_volatile':
        body += _line('predicate = MakeUnaryCallable("Udf", f.Column(r))')
    elif shape == 'reject_subplan':
        body += _line('const auto binding = f.Id("binding"), result = f.Id("result"), source = f.Id("source")')
        body += _line('TMapIUs elements')
        body += _line('elements.Add(result, MakeUnaryCallable("Udf", f.Column(source)))')
        body += _line('auto plan = MakeIntrusive<TOpMap>(f.Read({source}), f.Pos, std::move(elements))')
        body += _line('f.Props.Subplans.Add(binding, plan, ESubplanType::EXPR, TOrderedIUs<>{}, result)')
        body += _line('predicate = MakeBinaryPredicate(">", f.Column(binding), MakeConstant("Int32", "0", f.Pos, &f.ExprCtx))')
    elif shape == 'left_residual':
        body += _line('predicate = MakeBinaryPredicate(">", f.Column(b), f.Column(r))')
    elif shape == 'subplan_capture':
        body += _line('const auto binding = f.Id("binding"), localLeft = f.Id("localLeft"), localRight = f.Id("localRight"), capturedResult = f.Id("capturedResult")')
        body += _line('TDependencyIUs dependencies')
        body += _line('dependencies.Add(localLeft, TCapturedIU{a, f.ExprCtx.MakeType<TDataExprType>(EDataSlot::Int32)})')
        body += _line('dependencies.Add(localRight, TCapturedIU{r, f.ExprCtx.MakeType<TDataExprType>(EDataSlot::Int32)})')
        body += _line('auto captures = MakeIntrusive<TOpAddDependencies>(MakeIntrusive<TOpEmptySource>(f.Pos), f.Pos, std::move(dependencies))')
        body += _line('TMapIUs capturedElements')
        body += _line('capturedElements.Add(capturedResult, MakeBinaryPredicate("+", f.Column(localLeft), f.Column(localRight)))')
        body += _line('auto capturedPlan = MakeIntrusive<TOpMap>(captures, f.Pos, std::move(capturedElements))')
        body += _line('const auto originalLocalExpression = capturedPlan->GetMapElements().At(capturedResult).GetExpression().Node')
        body += _line('f.Props.Subplans.Add(binding, capturedPlan, ESubplanType::EXPR, TOrderedIUs<>{}, capturedResult)')
        body += _line('f.Props.Subplans.RefreshDependencies(binding)')
        body += _check('f.Props.Subplans.At(binding).DependentIUs == TUnorderedIUs{a,r}')
        body += _check('CanDuplicateSubtree(*capturedPlan, f.Props.Subplans)')
        body += _line('predicate = MakeBinaryPredicate(">", f.Column(binding), f.Column(r))')
    body += _line('TJoinIUs originalKeys')
    if shape == 'equi_keys':
        body += _line('originalKeys.Add({a, r, false})')
        body += _line('originalKeys.Add({b, s, true})')
    body += _line('auto join = MakeIntrusive<TOpJoin>(left, right, f.Pos, ' + json.dumps(kind) + ', originalKeys, TVector<TExpression>{predicate})')
    body += _line('TInlineJoinFiltersRule rule')
    body += _check('rule.QuickMatch(join)')
    if rejected:
        if shape == 'reject_volatile':
            body += _check('!CanDuplicateOperator(*join)')
            expected_reason = 'Join filter domain requires repeatable predicates'
        elif shape == 'reject_subplan':
            body += _check('CanDuplicateOperator(*join)')
            body += _check('!CanDuplicateSubtree(*f.Props.Subplans.At(binding).Plan, f.Props.Subplans)')
            expected_reason = 'Join filter domain requires repeatable subplans'
        else:
            body += _check('CanDuplicateOperator(*join)')
            body += _check('left->GetIUType(a, f.ExprCtx)->Cast<TDataExprType>()->GetSlot() == EDataSlot::Double')
            expected_reason = 'Unsupported join filter identity type'
        body += _line('bool refused = false')
        body += 'try { auto ignored = rule.SimpleMatchAndApply(join, f.RboCtx, f.Props); Y_UNUSED(ignored); } '
        body += 'catch (const std::exception& error) { refused = std::string_view(error.what()).find('
        body += json.dumps(expected_reason) + ') != std::string_view::npos; } catch (...) {}\n'
        return body + _check('refused')
    body += _line('const auto originalLeftIUs = left->GetOutputIUs(), originalRightIUs = right->GetOutputIUs()')
    if unique:
        body += _line('const auto originalIdentityKeys = left->Props.Metadata->KeyColumns.Unordered()')
    body += _line('TOrderedIUs<TString> originalProjection')
    body += 'for (const auto id : originalLeftIUs) originalProjection.Append(id, ToString(id));\n'
    if kind == 'Left':
        body += 'for (const auto id : originalRightIUs) originalProjection.Append(id, ToString(id));\n'
    body += _line('const auto originalOutputs = originalProjection.Unordered()')
    body += _check('join->GetOutputIUs() == originalOutputs')
    body += _line('auto projection = MakeIntrusive<TOpRoot>(join, f.Pos, originalProjection)')
    body += _line('auto rewritten = rule.SimpleMatchAndApply(join, f.RboCtx, f.Props)')
    body += _check('rewritten.Get() != join.Get() && rewritten->Kind == EOperator::Join')
    body += _line('auto& reconstruction = CastOperator<TOpJoin>(*rewritten)')
    body += _check('reconstruction.JoinKind == ' + json.dumps(kind))
    body += _check('!HasEqualNullsKey(reconstruction.JoinKeys)')
    body += _check('reconstruction.JoinKeys.Items().size() == ' + ('1' if zero else ('3' if shape == 'producer_guard' else '2')))
    body += _check('reconstruction.JoinKeys.Left().IsSubsetOf(reconstruction.GetLeftInput()->GetOutputIUs())')
    body += _check('reconstruction.JoinKeys.Right().IsSubsetOf(reconstruction.GetRightInput()->GetOutputIUs())')
    body += _line('unsigned domains = 0, encoders = 0, primary = 0, secondary = 0')
    body += _line('TOpReplicate* primaryPort = nullptr')
    body += _line('TOpReplicate* secondaryPort = nullptr')
    body += 'for (const auto& item : IterateSubtree(rewritten.Get())) {\n'
    body += 'auto& op = *item.Current;\n'
    body += 'if (op.Kind == EOperator::Aggregate) { ++domains; auto& domain = CastOperator<TOpAggregate>(op); Check(domain.GetAggregationTraits().Keys().Empty()); Check(!domain.GetKeyColumns().Items().empty()); Check(domain.GetKeyColumns().Unordered() == domain.GetInput()->GetOutputIUs()); }\n'
    body += 'if (op.Kind == EOperator::Replicate) { auto& port = CastOperator<TOpReplicate>(op); if (port.IsPrimary()) { ++primary; primaryPort = &port; } else { ++secondary; secondaryPort = &port; } }\n'
    body += 'if (op.Kind == EOperator::Map) for (const auto& [output, element] : CastOperator<TOpMap>(op).GetMapElements().Items()) { Y_UNUSED(output); if (element.GetExpression().GetExpressionBody()->IsCallable("StablePickle")) ++encoders; }\n'
    body += '}\n'
    body += _check('domains == ' + ('0' if unique else '1'))
    body += _check('encoders == ' + ('2' if optional else '0'))
    body += _check('primary == 1 && secondary == 1 && primaryPort && secondaryPort')
    body += _check('&primaryPort->GetReplicate() == &secondaryPort->GetReplicate()')
    body += _check('!primaryPort->GetOutputIUs().HasAny(secondaryPort->GetOutputIUs())')
    body += _check('CanDuplicateSubtree(*primaryPort->GetInput(), f.Props.Subplans) == ' + ('false' if shape == 'producer_guard' else 'true'))
    if zero:
        body += _check('primaryPort->GetInput()->Kind == EOperator::Map')
        body += _line('auto& unit = CastOperator<TOpMap>(*primaryPort->GetInput())')
        body += _check('unit.GetInput().Get() == left.Get() && left->GetOutputIUs().Empty()')
        body += _check('unit.GetMapElements().Items().size() == 1')
        body += _check('unit.GetMapElements().Items().begin()->second.GetExpression().GetExpressionBody()->IsCallable("Uint64")')
    else:
        body += _check('primaryPort->GetInput().Get() == left.Get()')
    return body + _binding_contract(shape, kind)


def render_cpp(case):
    return render_cpp_suite([case])


def render_cpp_suite(selected=None):
    selected = cases() if selected is None else list(selected)
    if not selected or len(set(selected)) != len(selected) or any(case not in cases() for case in selected):
        raise ValueError('Expected distinct known C31 IR cases')
    text = _prelude()
    for index, case in enumerate(selected):
        text += 'static void Run' + str(index) + '() {\n' + _body(case) + '}\n'
    text += 'int main() { unsigned failures=0;\n'
    for index in range(len(selected)):
        prefix = '{"case_index":' + str(index) + ',"checks_executed":'
        text += '{ const auto before = Checks; bool passed = true; try { Run' + str(index) + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps(prefix) + ' << (Checks-before) << ' + json.dumps(',"passed":')
        text += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    text += 'return failures ? 1 : 0; }\n'
    return text


def manifest(case):
    content = render_cpp(case).encode()
    return {'case': case, 'generated_source_sha256': hashlib.sha256(content).hexdigest(),
            'generated_bytes': len(content), 'production_rule_called': True,
            'native_or_compile_run_performed': False, 'generated_source_saved': False,
            'full_query_semantics_verified': False,
            'baseline_compatible': 'Same existing public rule/operator APIs on pinned main and amended C31'}
