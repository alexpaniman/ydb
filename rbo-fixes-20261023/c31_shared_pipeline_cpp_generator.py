"""Four production IR/stage/initial-lowering contracts; generated C++ stays in RAM.

Each reported check is a complete checkpoint: every internal Require must pass.
This is construction evidence, not execution or final SQL-plan lineage evidence.
"""
import hashlib
import json

CASES = ('c31_pipeline_shared_scalar_sum_count',
         'c31_pipeline_shared_nullable_group_sum_count',
         'c31_pipeline_shared_scalar_some_count',
         'c31_pipeline_copied_scalar_some_rejected')
CHECKPOINTS = (8, 8, 8, 2)


def cases():
    return list(CASES)


def specs():
    return [{'case': name, 'assertion_count': count}
            for name, count in zip(CASES, CHECKPOINTS)]


def _prelude():
    names = ('ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
             'ydb/core/kqp/common/kqp_yql.h',
             'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_aggregation_builder.h',
             'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_convertion_utils.h',
             'yql/essentials/core/type_ann/type_ann_expr.h',
             'yql/essentials/core/type_ann/type_ann_core.h',
             'iostream', 'stdexcept', 'utility')
    text = ''.join('#include <' + name + '>\n' for name in names)
    return text + r'''
using namespace NKikimr::NKqp;
using namespace NYql;
using namespace NYql::NNodes;
static unsigned Checks = 0;
static void Require(bool ok) { if (!ok) throw std::runtime_error("contract"); }
template<class F> static void Checkpoint(F&& f) { ++Checks; f(); }
template<class F> static void RequireRejected(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
}
static const TExprNode* Call(const TExprNode* node, TStringBuf name, size_t size) {
    Require(node && node->IsCallable(name) && node->ChildrenSize() == size);
    return node;
}
static const TExprNode* Unary(const TExprNode* node, TStringBuf name) {
    return Call(node, name, 1)->Child(0);
}
static const TExprNode* Lambda(const TExprNode* node, size_t args, size_t results) {
    Require(node && node->IsLambda() && node->ChildrenSize() == results + 1);
    Require(node->Child(0)->IsArguments() && node->Child(0)->ChildrenSize() == args);
    return node;
}
struct TFixture {
    NTests::TIdTestContext F;
    TAutoPtr<IGraphTransformer> Callable;
    TAutoPtr<IGraphTransformer> Annotator;
    TRBOContext Ctx;
    TInfoUnitId Key, Group, Value, Result, Count, Right;
    TIntrusivePtr<TOpRead> Read, RightRead;
    TIntrusivePtr<TOpAggregate> OriginalProducer;
    TIntrusivePtr<TOpJoin> OriginalJoin;
    TIntrusivePtr<TOpRoot> Root;
    bool Grouped, Some, Shared;
    TFixture(bool grouped, bool some, bool shared)
        : Callable(CreateExtCallableTypeAnnotationTransformer(F.TypeCtx))
        , Annotator(CreateTypeAnnotationTransformer(Callable, F.TypeCtx))
        , Ctx(F.KqpCtx, F.ExprCtx, F.TypeCtx, *Annotator, *F.FuncRegistry)
        , Grouped(grouped), Some(some), Shared(shared)
    {
        F.Config->SetUseBlockHashJoin(false);
        F.Config->OptShuffleElimination = false;
        F.Config->SetEnableQueryServiceSpilling(shared);
        F.Config->_KqpEnableSpilling = true;
        F.QueryCtx->Type = EKikimrQueryType::Query;
        auto metadata = MakeIntrusive<TKikimrTableMetadata>();
        metadata->DoesExist = true;
        metadata->Cluster = "ut";
        metadata->Name = "/Root/contract";
        metadata->Kind = EKikimrTableKind::Datashard;
        metadata->Columns.emplace("pk", TKikimrColumnMetadata("pk", 1, "Int64", true));
        metadata->Columns.emplace("group", TKikimrColumnMetadata("group", 2, "Int64", false));
        metadata->Columns.emplace("value", TKikimrColumnMetadata("value", 3, "Int64", false));
        metadata->KeyColumnNames = {"pk"};
        auto& table = F.Tables->GetOrAddTable("ut", "/Root", metadata->Name);
        table.Metadata = metadata;
        Require(table.Load(F.ExprCtx));
        auto& registry = F.Props.InfoUnitRegistry;
        Count = registry.AddGenerated("count");
        Group = registry.Add(TInfoUnit("source", "group"));
        Result = registry.AddGenerated("value");
        Key = registry.Add(TInfoUnit("source", "pk"));
        Value = registry.Add(TInfoUnit("source", "value"));
        Right = registry.Add(TInfoUnit("right", "pk"));
        const auto tableNode = Build<TKqpTable>(F.ExprCtx, F.Pos).Path().Build(metadata->Name)
            .PathId().Build("1:1").SysView().Build("").Version().Build("1").Done().Ptr();
        Read = MakeIntrusive<TOpRead>("source", TUnorderedIUs{Key, Group, Value},
            NYql::EStorageType::RowStorage, tableNode, nullptr, nullptr, std::nullopt,
            std::nullopt, ESortDir::None, TPhysicalOpProps{}, F.Pos);
        RightRead = MakeIntrusive<TOpRead>("right", TUnorderedIUs{Right},
            NYql::EStorageType::RowStorage, tableNode, nullptr, nullptr, std::nullopt,
            std::nullopt, ESortDir::None, TPhysicalOpProps{}, F.Pos);
        TAggregationIUs traits;
        traits.Add(Result, TOpAggregationTraits{Value, Some ? "some" : "sum"});
        traits.Add(Count, TOpAggregationTraits{Value, "count"});
        OriginalProducer = MakeIntrusive<TOpAggregate>(Read, std::move(traits),
            Grouped ? TOrderedIUs<>{Group} : TOrderedIUs<>{}, EOpPhase::Undefined, false, F.Pos);
        OriginalJoin = MakeIntrusive<TOpJoin>(OriginalProducer, RightRead, F.Pos, "Left", TJoinIUs{});
        OriginalJoin->JoinFilters.push_back(MakeBinaryPredicate(">", F.Column(Result), F.Column(Right)));
        TOrderedIUs<TString> outputs{{Right, "right"}, {Result, "result"}, {Count, "count"}};
        if (Grouped) outputs.Append(Group, "group");
        Root = F.Root(OriginalJoin, std::move(outputs));
    }
    void Props(ui32 required) { ComputeRequiredProps(*Root, required, Ctx, "contract"); }
    void Typed() {
        Props(ERuleProperties::RequireParents | ERuleProperties::RequireTypes | ERuleProperties::RequireMetadata);
    }
};
struct TShape {
    TOpJoin* Reconstruct;
    TOpJoin* Inner;
    TOpFilter* Residual;
    TOpReplicate* Primary;
    TOpReplicate* Secondary;
    TOpAggregate* Domain;
};
static IOperator* StripCodec(IOperator* op) {
    if (op->Kind == EOperator::Map) return CastOperator<TOpMap>(*op).GetInput().Get();
    return op;
}
static TInfoUnitId IdentityInput(IOperator& op, TInfoUnitId key, bool& encoded) {
    encoded = false;
    if (op.Kind != EOperator::Map) return key;
    auto* element = CastOperator<TOpMap>(op).GetMapElements().Find(key);
    if (!element) return key;
    const auto& expression = element->GetExpression();
    const auto body = expression.GetExpressionBody();
    const auto* member = Call(body.Get(), "StablePickle", 1)->Child(0);
    const auto lambda = expression.GetLambda();
    Lambda(lambda.Get(), 1, 1);
    Call(member, "Member", 2);
    Require(member->Child(0) == lambda->Child(0)->Child(0));
    const auto id = GetMemberId(*member);
    Require(expression.GetRawInputIUs() == TUnorderedIUs{id});
    encoded = true;
    return id;
}
static TShape Inspect(TFixture& f, bool split) {
    auto& reconstruction = CastOperator<TOpJoin>(*f.Root->GetInput());
    auto* primary = StripCodec(reconstruction.GetLeftInput().Get());
    Require(primary->Kind == EOperator::Replicate);
    auto* filtered = StripCodec(reconstruction.GetRightInput().Get());
    Require(filtered->Kind == EOperator::Filter);
    auto& filter = CastOperator<TOpFilter>(*filtered);
    auto& inner = CastOperator<TOpJoin>(*filter.GetInput());
    auto* domainInput = inner.GetLeftInput().Get();
    TOpAggregate* domain = nullptr;
    if (!f.Grouped) {
        Require(domainInput->Kind == EOperator::Aggregate);
        domain = &CastOperator<TOpAggregate>(*domainInput);
        Require(domain->GetAggregationTraits().Keys().Empty() && !domain->IsDistinctAll());
        domainInput = domain->GetInput().Get();
        if (split) {
            Require(domain->GetAggregationPhase() == EOpPhase::Final && domainInput->Kind == EOperator::Aggregate);
            auto& partial = CastOperator<TOpAggregate>(*domainInput);
            Require(partial.GetAggregationPhase() == EOpPhase::Intermediate);
            Require(partial.GetAggregationTraits().Keys().Empty() && !partial.IsDistinctAll());
            Require(partial.GetKeyColumns() == domain->GetKeyColumns());
            domainInput = partial.GetInput().Get();
        }
    }
    Require(domainInput->Kind == EOperator::Replicate);
    auto& a = CastOperator<TOpReplicate>(*primary);
    auto& b = CastOperator<TOpReplicate>(*domainInput);
    Require(a.IsPrimary() && !b.IsPrimary() && &a.GetReplicate() == &b.GetReplicate());
    Require(a.GetInput() == b.GetInput() && !a.GetOutputIUs().HasAny(b.GetOutputIUs()));
    Require(inner.GetRightInput() == f.RightRead && inner.JoinKind == "Cross");
    Require(reconstruction.JoinKind == "Left" && reconstruction.JoinFilters.empty() && inner.JoinFilters.empty());
    const auto& renamed = b.GetRebindings();
    const auto expected = f.Grouped ? TUnorderedIUs{f.Group} : TUnorderedIUs{f.Result, f.Count};
    Require(a.GetInput()->GetOutputIUs().Contains(f.Result) && a.GetInput()->GetOutputIUs().Contains(f.Count));
    TUnorderedIUs found;
    for (const auto& key : reconstruction.JoinKeys.Items()) {
        bool leftEncoded, rightEncoded;
        const auto source = IdentityInput(*reconstruction.GetLeftInput(), key.first, leftEncoded);
        const auto target = IdentityInput(*reconstruction.GetRightInput(), key.second, rightEncoded);
        Require(expected.Contains(source) && !found.Contains(source));
        found.Add(source);
        Require(target == renamed.At(source) && !key.EqualNulls);
        Require(leftEncoded == rightEncoded && leftEncoded == (source != f.Count));
    }
    Require(found == expected);
    if (domain) {
        Require(domain->GetKeyColumns().Unordered() == b.GetOutputIUs());
        Require(domain->GetKeyColumns().Unordered() == TUnorderedIUs{renamed.At(f.Result), renamed.At(f.Count)});
    }
    const auto& residual = filter.GetFilterExpression();
    Require(residual.GetRawInputIUs() == TUnorderedIUs{renamed.At(f.Result), f.Right});
    const auto lambda = residual.GetLambda();
    const auto body = residual.GetExpressionBody();
    Lambda(lambda.Get(), 1, 1);
    Call(body.Get(), ">", 2);
    for (size_t i = 0; i != 2; ++i) {
        auto member = Call(body->Child(i), "Member", 2);
        Require(member->Child(0) == lambda->Child(0)->Child(0));
        Require(GetMemberId(*member) == (i ? f.Right : renamed.At(f.Result)));
    }
    return {&reconstruction, &inner, &filter, &a, &b, domain};
}
static void ApplyAggregateStage(TFixture& f) {
    TVector<std::unique_ptr<IRule>> rules;
    rules.emplace_back(std::make_unique<TPropagateAggregateThroughStageRule>());
    TRuleBasedStage stage("contract aggregate propagation", std::move(rules));
    stage.RunStage(*f.Root, f.Ctx);
}
static void VerifyProducer(TFixture& f, const TShape& s) {
    auto& final = CastOperator<TOpAggregate>(*s.Primary->GetInput());
    auto& partial = CastOperator<TOpAggregate>(*final.GetInput());
    Require(final.GetAggregationPhase() == EOpPhase::Final && partial.GetAggregationPhase() == EOpPhase::Intermediate);
    Require(partial.GetInput() == f.Read);
    Require(final.GetAggregationTraits().Keys() == TUnorderedIUs{f.Result, f.Count});
    Require(partial.GetAggregationTraits().Keys().Size() == 2);
    Require(final.GetKeyColumns().Unordered() == (f.Grouped ? TUnorderedIUs{f.Group} : TUnorderedIUs{}));
    Require(final.GetKeyColumns() == partial.GetKeyColumns());
    TUnorderedIUs intermediate;
    for (auto output : TUnorderedIUs{f.Result, f.Count}) {
        const auto& finish = final.GetAggregationTraits().At(output);
        Require(!intermediate.Contains(finish.Input));
        intermediate.Add(finish.Input);
        const auto& init = partial.GetAggregationTraits().At(finish.Input);
        Require(init.Input == f.Value && !init.Distinct && !init.Unwrap);
        Require(init.AggFunction == (output == f.Count ? "count" : f.Some ? "some" : "sum"));
        Require(finish.AggFunction == (output == f.Count ? "sum" : f.Some ? "some" : "sum"));
    }
    Require(intermediate == partial.GetAggregationTraits().Keys());
    THashSet<const IOperator*> visited;
    unsigned owners = 0;
    for (const auto& item : *f.Root) {
        auto* op = item.Current;
        if (!visited.insert(op).second || op->Kind != EOperator::Aggregate) continue;
        if (CastOperator<TOpAggregate>(*op).GetAggregationTraits().Keys().HasAny(TUnorderedIUs{f.Result, f.Count})) {
            Require(op == &final);
            ++owners;
        }
    }
    Require(owners == 1);
    Require(final.Props.StageId == s.Primary->Props.StageId && final.Props.StageId == s.Secondary->Props.StageId);
    Require(partial.Props.StageId == final.Props.StageId);
    const auto& edge = f.Root->PlanProps.StageGraph.GetConnections(*f.Read->Props.StageId, *final.Props.StageId);
    Require(edge.size() == 1);
}
static void VerifyEdges(TFixture& f, const TShape& s) {
    const auto& graph = f.Root->PlanProps.StageGraph;
    const auto producer = *s.Primary->Props.StageId;
    const auto outer = *s.Reconstruct->GetLeftInput()->Props.StageId;
    const auto inner = *s.Inner->Props.StageId;
    const auto reconstruction = *s.Reconstruct->Props.StageId;
    Require(s.Reconstruct->GetLeftInput()->Kind == EOperator::Map);
    const auto& primary = graph.GetConnections(producer, outer);
    Require(primary.size() == 1 && primary.front()->GetOutputIndex() == GetReplicateOutputIndex(*s.Primary));
    Require(graph.StageInputs.at(outer).size() == 1 && graph.StageInputs.at(outer).front() == producer);
    const auto secondTarget = s.Domain ? *s.Domain->Props.StageId : inner;
    const auto& secondary = graph.GetConnections(producer, secondTarget);
    Require(secondary.size() == 1 && secondary.front()->GetOutputIndex() == GetReplicateOutputIndex(*s.Secondary));
    Require(graph.StageInputs.at(inner).size() == 2 && graph.StageInputs.at(inner).front() == *s.Inner->GetLeftInput()->Props.StageId);
    Require(graph.StageInputs.at(reconstruction).size() == 2);
    Require(graph.StageInputs.at(reconstruction).front() == outer);
    Require(graph.StageInputs.at(reconstruction).back() == *s.Reconstruct->GetRightInput()->Props.StageId);
    const auto& leftKeys = graph.GetConnections(outer, reconstruction);
    const auto& rightKeys = graph.GetConnections(*s.Reconstruct->GetRightInput()->Props.StageId, reconstruction);
    Require(leftKeys.size() == 1 && rightKeys.size() == 1);
    Require(leftKeys.front()->GetUsedIUs() == s.Reconstruct->JoinKeys.Left());
    Require(rightKeys.front()->GetUsedIUs() == s.Reconstruct->JoinKeys.Right());
}
static void CheckPortBranch(const TExprNode* branch, TOpReplicate& port, const TPhysicalNames& names) {
    Lambda(branch, 1, 1);
    auto map = Call(branch->Child(1), "OrderedMap", 2);
    Require(map->Child(0) == branch->Child(0)->Child(0));
    auto row = Lambda(map->Child(1), 1, 1);
    const auto* fields = row->Child(1);
    Require(fields->IsCallable("AsStruct"));
    THashSet<TString> observed;
    for (const auto& tuple : fields->Children()) {
        Require(tuple->IsList() && tuple->ChildrenSize() == 2 && tuple->Child(0)->IsAtom());
        const auto* member = Call(tuple->Child(1), "Member", 2);
        Require(member->Child(0) == row->Child(0)->Child(0));
        const TString output(tuple->Child(0)->Content());
        Require(observed.insert(output).second);
        bool matched = false;
        for (auto source : port.GetInput()->GetOutputIUs()) {
            const auto target = port.IsPrimary() ? source : port.GetRebindings().At(source);
            if (names.Get(target) == output) {
                Require(GetLiveOut(&port).Contains(target) && member->Child(1)->Content() == names.Get(source));
                matched = true;
            }
        }
        Require(matched);
    }
    Require(observed.size() == GetLiveOut(&port).Size());
}
static void CheckDomainBuilder(const TExprNode* output, const TExprNode* expectedInput,
    TOpAggregate& domain, const TPhysicalNames& names, bool blocks) {
    Require(domain.GetAggregationTraits().Keys().Empty());
    const auto* narrow = Call(Unary(output, "FromFlow"), "NarrowMap", 2);
    const auto* combine = narrow->Child(0);
    if (blocks) combine = Unary(Unary(Unary(combine, "ToFlow"), "WideFromBlocks"), "FromFlow");
    Call(combine, "DqPhyHashCombine", 6);
    const auto* expand = combine->Child(0);
    if (blocks) expand = Unary(Unary(Unary(expand, "ToFlow"), "WideToBlocks"), "FromFlow");
    Call(expand, "ExpandMap", 2);
    Require(Unary(expand->Child(0), "ToFlow") == expectedInput);
    const auto count = domain.GetKeyColumns().Items().size();
    auto expandLambda = Lambda(expand->Child(1), 1, count);
    auto keys = Lambda(combine->Child(2), count, count);
    auto init = Lambda(combine->Child(3), count + count, 0);
    auto update = Lambda(combine->Child(4), count + count, 0);
    auto finish = Lambda(combine->Child(5), count, count);
    Y_UNUSED(init); Y_UNUSED(update);
    THashMap<TString, size_t> inputPositions;
    for (size_t i = 0; i != count; ++i) {
        auto member = Call(expandLambda->Child(i + 1), "Member", 2);
        Require(member->Child(0) == expandLambda->Child(0)->Child(0));
        Require(inputPositions.emplace(TString(member->Child(1)->Content()), i).second);
    }
    auto resultMap = Lambda(narrow->Child(1), count, 1);
    auto fields = Call(resultMap->Child(1), "AsStruct", count);
    THashMap<TString, const TExprNode*> results;
    for (const auto& tuple : fields->Children()) {
        Require(tuple->IsList() && tuple->ChildrenSize() == 2 && tuple->Child(0)->IsAtom());
        Require(results.emplace(TString(tuple->Child(0)->Content()), tuple->Child(1)).second);
    }
    size_t index = 0;
    for (const auto id : domain.GetKeyColumns().Items()) {
        const auto& name = names.Get(id);
        Require(inputPositions.contains(name) && results.contains(name));
        Require(keys->Child(index + 1) == keys->Child(0)->Child(inputPositions.at(name)));
        Require(finish->Child(index + 1) == finish->Child(0)->Child(index));
        Require(results.at(name) == resultMap->Child(0)->Child(index));
        ++index;
    }
}
static void Lower(TFixture& f, const TShape& s, bool blocks) {
    const TPhysicalNames names(f.Root->PlanProps.InfoUnitRegistry);
    auto& producer = CastOperator<TOpAggregate>(*s.Primary->GetInput());
    ui32 inputCounter = 0;
    auto [arg, input] = f.Root->PlanProps.StageGraph.GenerateStageInput(inputCounter, f.F.Pos, f.F.ExprCtx);
    Require(arg && input && inputCounter == 1);
    auto& producerPartial = CastOperator<TOpAggregate>(*producer.GetInput());
    auto partialBody = TPhysicalAggregationBuilder(producerPartial, f.F.ExprCtx, f.F.Pos, names, false, blocks)
        .BuildPhysicalOp(input, std::nullopt);
    auto producerBody = TPhysicalAggregationBuilder(producer, f.F.ExprCtx, f.F.Pos, names, false, blocks)
        .BuildPhysicalOp(partialBody, std::nullopt);
    auto before = NPhysicalConvertionUtils::BuildSwitch(producerBody, s.Primary->GetReplicate(), names, f.F.ExprCtx);
    Require(before->IsCallable("Switch") && before->ChildrenSize() == 6 && before->Child(0) == producerBody.Get());
    const auto primaryIndex = GetReplicateOutputIndex(*s.Primary).value();
    const auto secondaryIndex = GetReplicateOutputIndex(*s.Secondary).value();
    Require(primaryIndex != secondaryIndex && primaryIndex < 2 && secondaryIndex < 2);
    CheckPortBranch(before->Child(3 + 2 * primaryIndex), *s.Primary, names);
    CheckPortBranch(before->Child(3 + 2 * secondaryIndex), *s.Secondary, names);
    auto foreign = f.F.ExprCtx.NewArgument(f.F.Pos, "foreign");
    const auto branch = before->ChildPtr(3 + 2 * secondaryIndex);
    auto wrongMapInput = f.F.ExprCtx.ChangeChild(*branch->Child(1), 0, TExprNode::TPtr(foreign));
    auto wrongBranch = f.F.ExprCtx.ChangeChild(*branch, 1, std::move(wrongMapInput));
    RequireRejected([&] { CheckPortBranch(wrongBranch.Get(), *s.Secondary, names); });
    if (!s.Domain) return;
    auto& partial = CastOperator<TOpAggregate>(*s.Domain->GetInput());
    Require(partial.GetInput().Get() == s.Secondary);
    Require(partial.Props.StageId == producer.Props.StageId);
    Require(GetReplicateOutputIndex(partial) == secondaryIndex);
    const auto& edge = f.Root->PlanProps.StageGraph.GetConnections(*partial.Props.StageId, *s.Domain->Props.StageId);
    Require(edge.size() == 1 && edge.front()->GetOutputIndex() == secondaryIndex);
    Require(edge.front()->GetUsedIUs() == partial.GetKeyColumns().Unordered());
    const auto* oldBranch = before->Child(3 + 2 * secondaryIndex);
    auto after = NPhysicalConvertionUtils::TransformStageOutput(before, [&](TExprNode::TPtr body) {
        Require(body.Get() == oldBranch->Child(1));
        return TPhysicalAggregationBuilder(partial, f.F.ExprCtx, f.F.Pos, names, false, blocks)
            .BuildPhysicalOp(body, std::nullopt);
    }, secondaryIndex, f.F.ExprCtx);
    Require(after->Child(0) == producerBody.Get());
    Require(after->Child(3 + 2 * primaryIndex) == before->Child(3 + 2 * primaryIndex));
    auto* adapted = after->Child(3 + 2 * secondaryIndex);
    Require(adapted->Child(0) == oldBranch->Child(0));
    CheckDomainBuilder(adapted->Child(1), oldBranch->Child(1), partial, names, blocks);
    RequireRejected([&] { CheckDomainBuilder(adapted->Child(1), foreign.Get(), partial, names, blocks); });
    const auto output = adapted->ChildPtr(1);
    const auto narrow = output->ChildPtr(0);
    const auto lambda = narrow->ChildPtr(1);
    const auto fields = lambda->ChildPtr(1);
    Require(fields->ChildrenSize() == 2);
    auto duplicateField = f.F.ExprCtx.ChangeChild(*fields->Child(0), 0, fields->Child(1)->ChildPtr(0));
    auto wrongFields = f.F.ExprCtx.ChangeChild(*fields, 0, std::move(duplicateField));
    auto wrongLambda = f.F.ExprCtx.ChangeChild(*lambda, 1, std::move(wrongFields));
    auto wrongNarrow = f.F.ExprCtx.ChangeChild(*narrow, 1, std::move(wrongLambda));
    auto wrongOutput = f.F.ExprCtx.ChangeChild(*output, 0, std::move(wrongNarrow));
    RequireRejected([&] { CheckDomainBuilder(wrongOutput.Get(), oldBranch->Child(1), partial, names, blocks); });
}
static void Run(bool grouped, bool some, bool shared) {
    TFixture f(grouped, some, shared);
    Checkpoint([&] {
        f.Typed();
        Require(f.F.KqpCtx.IsGenericQuery() && f.F.Config->SpillingEnabled());
        Require(f.F.Config->GetEnableQueryServiceSpilling() == shared);
        Require(f.OriginalProducer->Props.Metadata->KeyColumns.Unordered() == (grouped ? TUnorderedIUs{f.Group} : TUnorderedIUs{}));
        Require(f.OriginalProducer->GetIUType(f.Result, f.F.ExprCtx)->IsOptionalOrNull());
        Require(!f.OriginalProducer->GetIUType(f.Count, f.F.ExprCtx)->IsOptionalOrNull());
    });
    if (!shared) {
        Checkpoint([&] {
            const auto count = f.Root->PlanProps.InfoUnitRegistry.Size();
            const auto output = f.Root->GetOutputIUs();
            const auto predicate = f.OriginalJoin->JoinFilters.front().Node;
            const auto producerType = f.OriginalProducer->Type;
            bool rejected = false;
            try {
                TInlineJoinFiltersRule rule;
                rule.SimpleMatchAndApply(f.Root->GetInput(), f.Ctx, f.Root->PlanProps);
            } catch (const yexception& error) {
                rejected = TString(error.what()).Contains("Join filter reconstruction requires stable copied values");
            }
            Require(rejected && f.Root->GetInput() == f.OriginalJoin);
            Require(f.Root->PlanProps.InfoUnitRegistry.Size() == count && f.Root->GetOutputIUs() == output);
            Require(f.OriginalJoin->GetLeftInput() == f.OriginalProducer && f.OriginalProducer->GetInput() == f.Read);
            Require(f.OriginalJoin->JoinFilters.size() == 1 && f.OriginalJoin->JoinFilters.front().Node == predicate);
            Require(f.OriginalProducer->Props.Metadata->KeyColumns.Items().empty());
            Require(f.OriginalProducer->GetAggregationTraits().Keys() == TUnorderedIUs{f.Result, f.Count});
            Require(f.OriginalProducer->Type == producerType);
            const auto& first = f.OriginalProducer->GetAggregationTraits().At(f.Result);
            const auto& second = f.OriginalProducer->GetAggregationTraits().At(f.Count);
            Require(first.Input == f.Value && first.AggFunction == "some" && !first.Distinct && !first.Unwrap);
            Require(second.Input == f.Value && second.AggFunction == "count" && !second.Distinct && !second.Unwrap);
            for (const auto& item : *f.Root) Require(item.Current->Kind != EOperator::Replicate);
        });
        return;
    }
    TShape s{};
    Checkpoint([&] {
        TVector<std::unique_ptr<IRule>> rules;
        rules.emplace_back(std::make_unique<TInlineJoinFiltersRule>());
        TRuleBasedStage stage("contract inline", std::move(rules));
        stage.RunStage(*f.Root, f.Ctx);
        f.Typed();
        s = Inspect(f, false);
        Require(s.Primary->GetInput() == f.OriginalProducer);
        NTests::AssertIdInvariants(*f.Root, f.Root->PlanProps);
    });
    Checkpoint([&] {
        TGlobalPruningStage prune("contract prune");
        f.Props(prune.Props);
        prune.RunStage(*f.Root, f.Ctx);
        Require(!f.Root->Type && !f.OriginalProducer->Type);
        TGlobalInliningStage inlineCopies("contract inline copies");
        f.Props(inlineCopies.Props);
        inlineCopies.RunStage(*f.Root, f.Ctx);
        f.Typed();
        s = Inspect(f, false);
        Require(s.Primary->GetInput() == f.OriginalProducer);
        NTests::AssertIdInvariants(*f.Root, f.Root->PlanProps);
    });
    Checkpoint([&] {
        TAssignStagesStage stage;
        f.Props(stage.Props);
        stage.RunStage(*f.Root, f.Ctx);
        s = Inspect(f, false);
        Require(s.Primary->Props.StageId == s.Secondary->Props.StageId);
        Require(s.Primary->Props.StageId == s.Primary->GetInput()->Props.StageId);
        if (s.Domain) {
            const auto& edge = f.Root->PlanProps.StageGraph.GetConnections(*s.Secondary->Props.StageId, *s.Domain->Props.StageId);
            Require(edge.size() == 1 && edge.front()->GetOutputIndex() == GetReplicateOutputIndex(*s.Secondary));
            Require(edge.front()->GetUsedIUs() == s.Domain->GetKeyColumns().Unordered());
        }
    });
    Checkpoint([&] {
        ApplyAggregateStage(f);
        f.Typed();
        s = Inspect(f, true);
        VerifyProducer(f, s);
        VerifyEdges(f, s);
    });
    Checkpoint([&] {
        ComputePlanLiveness(*f.Root, ELivenessMode::Global);
        NTests::AssertIdInvariants(*f.Root, f.Root->PlanProps);
        ComputePlanLiveness(*f.Root);
        s = Inspect(f, true);
        Require(GetLiveOut(s.Primary).Contains(f.Result) && GetLiveOut(s.Primary).Contains(f.Count));
        Require(GetLiveOut(s.Secondary).Contains(s.Secondary->GetRebindings().At(f.Result)));
        if (s.Domain) Require(GetLiveOut(s.Secondary) == s.Domain->GetKeyColumns().Unordered());
        VerifyProducer(f, s);
        VerifyEdges(f, s);
    });
    Checkpoint([&] { Lower(f, s, false); });
    Checkpoint([&] { Lower(f, s, true); });
}
'''


def render_cpp_suite(selected=None):
    selected = cases() if selected is None else list(selected)
    if not selected or len(set(selected)) != len(selected) or any(x not in CASES for x in selected):
        raise ValueError('Unknown or duplicate C31 pipeline contract')
    text = _prelude()
    for i, case in enumerate(selected):
        idx = CASES.index(case)
        args = [('false', 'false', 'true'), ('true', 'false', 'true'),
                ('false', 'true', 'true'), ('false', 'true', 'false')][idx]
        text += 'static void Case' + str(i) + '() { Run(' + ','.join(args) + '); }\n'
    text += 'int main() { unsigned failures = 0;\n'
    for i in range(len(selected)):
        text += '{ const auto before = Checks; bool passed = true; try { Case' + str(i) + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps('{"case_index":' + str(i) + ',"checks_executed":')
        text += ' << (Checks-before) << ' + json.dumps(',"passed":') + ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    return text + 'return failures ? 1 : 0; }\n'


def manifest():
    source = render_cpp_suite().encode()
    return {'case_vector': cases(), 'checkpoint_counts': list(CHECKPOINTS),
            'generated_source_sha256': hashlib.sha256(source).hexdigest(),
            'generated_source_bytes': len(source), 'native_performed': False,
            'full_query_lineage_proven': False, 'generated_source_saved': False}
