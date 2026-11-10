#include "kqp_rbo_physical_lookup_join_builder.h"
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>

#include <yql/essentials/core/yql_expr_type_annotation.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

namespace {

TCoNameValueTuple BuildMemberTuple(const TString& name, const TString& sourceName, const TExprBase& row, TExprContext& ctx,
                                   TPositionHandle pos) {
    // clang-format off
    return Build<TCoNameValueTuple>(ctx, pos)
        .Name().Build(name)
        .Value<TCoMember>()
            .Struct(row)
            .Name().Build(sourceName)
        .Build()
    .Done();
    // clang-format on
}

TExprBase BuildOptionalIf(const TExprBase& predicate, const TExprBase& value, TExprContext& ctx, TPositionHandle pos) {
    // clang-format off
    return Build<TCoOptionalIf>(ctx, pos)
        .Predicate<TCoCoalesce>()
            .Predicate(predicate)
            .Value<TCoBool>()
                .Literal().Build("false")
            .Build()
        .Build()
        .Value(value)
    .Done();
    // clang-format on
}

} // anonymous namespace

namespace NKikimr::NKqp::NLookupJoinBuilder {

TExprNode::TPtr BuildLookupPoints(const TOpRead::TPointPrefix& prefix, TExprContext& ctx, TPositionHandle pos) {
    Y_ENSURE(prefix.HasPoints());
    const auto& points = *prefix.NativePoints;
    const auto* unit = ctx.MakeType<TUnitExprType>();
    const auto atom = [&](TStringBuf text) {
        auto result = ctx.NewAtom(pos, text); result->SetTypeAnn(unit); return result;
    };
    const auto call = [&](TStringBuf name, TExprNode::TListType children, const TTypeAnnotationNode* type) {
        auto result = ctx.NewCallable(pos, name, std::move(children)); result->SetTypeAnn(type); return result;
    };
    const auto tuple = [&](TExprNode::TListType children) {
        TTypeAnnotationNode::TListType types;
        for (const auto& child : children) types.push_back(child->GetTypeAnn());
        auto result = ctx.NewList(pos, std::move(children)); result->SetTypeAnn(ctx.MakeType<TTupleExprType>(types)); return result;
    };
    TVector<const TItemExprType*> fields;
    TTypeAnnotationNode::TListType types;
    for (size_t i = 0; i < prefix.Columns.size(); ++i) {
        const auto* type = ExportNativeType(points.Types[i], ctx);
        fields.push_back(ctx.MakeType<TItemExprType>(prefix.Columns[i], type));
        types.push_back(type);
    }
    const auto* rowType = ctx.MakeType<TStructExprType>(fields);
    const auto* listType = ctx.MakeType<TListExprType>(rowType);
    const bool guarded = AnyOf(points.Rows, [](const auto& point) { return point.Condition.has_value(); });
    TExprNode::TPtr empty;
    if (guarded) empty = call("List", {LowerExecutionType(listType, ctx, pos)}, listType);
    TExprNode::TListType rows;
    for (const auto& row : points.Rows) {
        Y_ENSURE(row.Values.size() == prefix.Columns.size());
        TExprNode::TListType members;
        for (size_t i = 0; i < row.Values.size(); ++i) {
            const auto& expression = row.Values[i];
            Y_ENSURE(expression.IsNative() && expression.GetRawInputIUs().Empty(), "Lookup point depends on a row");
            auto value = LowerNativeLambda(*expression.GetNativeArena(), expression.GetNativeRoot(), ctx, pos)->TailPtr();
            if (points.Types[i].OptionalDepth && !expression.GetNativeType().OptionalDepth) value = call("Just", {value}, types[i]);
            Y_ENSURE(IsSameAnnotation(*value->GetTypeAnn(), *types[i]), "Lookup point type mismatch");
            members.push_back(tuple({atom(prefix.Columns[i]), value}));
        }
        auto value = call("AsStruct", std::move(members), rowType);
        if (guarded) value = call("AsList", {value}, listType);
        if (row.Condition) {
            const auto& expression = *row.Condition;
            Y_ENSURE(expression.IsNative() && expression.GetRawInputIUs().Empty() &&
                expression.GetNativeType() == NFrontend::TType{NFrontend::ETypeKind::Bool}, "Invalid lookup point condition");
            const auto condition = LowerNativeLambda(*expression.GetNativeArena(), expression.GetNativeRoot(), ctx, pos)->TailPtr();
            value = call("If", {condition, value, empty}, listType);
        }
        rows.push_back(std::move(value));
    }
    auto list = guarded && rows.size() == 1 ? rows.front() : call(guarded ? "Extend" : "AsList", std::move(rows), listType);
    if (points.Rows.size() == 1) return list;
    // Distinct parameter names may bind to the same value. Deduplicate complete
    // prefix tuples at runtime to avoid multiplying joined rows. ToDict and
    // DictPayloads are directly executable; no semantic lowering is needed.
    const auto lambda = [&](const TExprNode::TPtr& arg, TExprNode::TPtr body) {
        auto args = ctx.NewArguments(pos, {arg}); args->SetTypeAnn(unit);
        const TTypeAnnotationNode* type = body->GetTypeAnn();
        auto result = ctx.NewLambda(pos, std::move(args), std::move(body)); result->SetTypeAnn(type); return result;
    };
    auto keyArg = ctx.NewArgument(pos, "point_key"); keyArg->SetTypeAnn(rowType);
    TExprNode::TListType key;
    for (size_t i = 0; i < prefix.Columns.size(); ++i) key.push_back(call("Member", {keyArg, atom(prefix.Columns[i])}, types[i]));
    auto keySelector = lambda(keyArg, tuple(std::move(key)));
    auto payloadArg = ctx.NewArgument(pos, "point_payload"); payloadArg->SetTypeAnn(rowType);
    auto payloadSelector = lambda(payloadArg, payloadArg);
    auto settings = ctx.NewList(pos, {atom("Hashed"), atom("One")}); settings->SetTypeAnn(unit);
    const auto* dictType = ctx.MakeType<TDictExprType>(ctx.MakeType<TTupleExprType>(types), rowType);
    auto dict = call("ToDict", {list, keySelector, payloadSelector, settings}, dictType);
    return call("DictPayloads", {dict}, listType);
}

// Translate an upstream IU row to the storage lookup-key contract. Join mode
// additionally preserves the left payload and optional-prefix behavior.
TLookupKeysResult BuildLookupKeys(TOpTableLookup& lookup, TExprNode::TPtr inputStage, TExprContext& ctx, const TPhysicalNames& names) {
    auto& input = *lookup.GetInput();
    const auto pos = lookup.Pos;
    if (!lookup.IsJoin()) {
        TVector<std::pair<TString, TString>> columns;
        TVector<const TItemExprType*> types;
        for (const auto& [id, column] : lookup.LookupKeys.Items()) {
            columns.emplace_back(names.Get(id), column);
            types.push_back(ctx.MakeType<TItemExprType>(column, ExportIUType(input, id, ctx)));
        }
        auto stage = NPhysicalConvertionUtils::TransformStageOutput(inputStage, [&](TExprNode::TPtr body) {
            return NPhysicalConvertionUtils::BuildRenameMap(body, columns, ctx);
        }, GetReplicateOutputIndex(input), ctx);
        auto type = ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(types));
        return {std::move(stage), NYql::ExpandType(pos, *type, ctx)};
    }

    const auto row = Build<TCoArgument>(ctx, pos).Name("lookup_join_left_row").Done();
    const auto points = lookup.Prefix ? BuildLookupPoints(*lookup.Prefix, ctx, pos) : nullptr;

    const auto& liveOut = GetLiveOut(&lookup);
    TVector<TExprBase> leftMembers;
    TVector<const TItemExprType*> leftItems;
    THashSet<TString> addedNames;
    auto addLeftMember = [&](TInfoUnitId iu) {
        const auto name = names.Get(iu);
        if (!addedNames.insert(name).second) {
            return;
        }

        auto type = ExportIUType(input, iu, ctx);
        Y_ENSURE(type, "Type of the lookup join input column " << names.Get(iu) << " is not available");
        leftMembers.push_back(BuildMemberTuple(name, name, row, ctx, pos));
        leftItems.push_back(ctx.MakeType<TItemExprType>(name, type));
    };

    for (const auto& iu : input.GetOutputIUs()) {
        if (liveOut.Contains(iu)) {
            addLeftMember(iu);
        }
    }

    for (const auto& [leftKey, rightKey, equalNulls] : lookup.ResidualJoinKeys.Items()) {
        Y_UNUSED(rightKey);
        addLeftMember(leftKey);
    }

    const auto point = Build<TCoArgument>(ctx, pos).Name("lookup_join_key_point").Done();
    TVector<TExprBase> keyMembers;
    TVector<const TItemExprType*> keyItems;
    TVector<TExprBase> equalities;
    if (lookup.Prefix) {
        const auto* pointType = points->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
        Y_ENSURE(pointType);
        for (const auto& column : lookup.Prefix->Columns) {
            auto type = pointType->FindItemType(column);
            Y_ENSURE(type, "Type of the lookup key prefix column " << column << " is not available");
            keyMembers.push_back(BuildMemberTuple(column, column, point, ctx, pos));
            keyItems.push_back(ctx.MakeType<TItemExprType>(column, type));
        }

        // This is copy behavior of old optimizer but it looks non optimal.
        // Does we need to filter every left side prefix column with constant point?
        // We can just lookup by this constant point instead.
        // Keeping this for now, but it looks like we can optimize it out.
        for (const auto& [key, column] : lookup.Prefix->Equalities.Items()) {
            // clang-format off
            equalities.push_back(Build<TCoCmpEqual>(ctx, pos)
                .Left<TCoMember>()
                    .Struct(point)
                    .Name().Build(column)
                .Build()
                .Right<TCoMember>()
                    .Struct(row)
                    .Name().Build(names.Get(key))
                .Build()
            .Done());
            // clang-format on
        }
    }

    for (const auto& [key, column] : lookup.LookupKeys.Items()) {
        auto type = ExportIUType(input, key, ctx);
        Y_ENSURE(type, "Type of the lookup join key " << names.Get(key) << " is not available");
        keyMembers.push_back(BuildMemberTuple(column, names.Get(key), row, ctx, pos));
        keyItems.push_back(ctx.MakeType<TItemExprType>(column, type));
    }

    const auto leftStruct = Build<TCoAsStruct>(ctx, pos).Add(leftMembers).Done();
    const auto keyStruct = Build<TCoAsStruct>(ctx, pos).Add(keyMembers).Done();
    auto keyType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TStructExprType>(keyItems));

    TExprNode::TPtr lambdaBody;
    if (!lookup.Prefix) {
        // clang-format off
        lambdaBody = Build<TExprList>(ctx, pos)
            .Add(leftStruct)
            .Add<TCoJust>()
                .Input(keyStruct)
            .Build()
        .Done().Ptr();
        // clang-format on
    } else {
        TExprBase maybeKey = Build<TCoJust>(ctx, pos).Input(keyStruct).Done();
        if (!equalities.empty()) {
            const auto predicate = equalities.size() == 1
                ? equalities.front()
                : TExprBase(Build<TCoAnd>(ctx, pos).Add(equalities).Done());
            maybeKey = BuildOptionalIf(predicate, keyStruct, ctx, pos);
        }

        // We have to check that ranges are not null. For example `where a is null`, where a is a pk, is also valid point predicate for us.
        // clang-format off
        lambdaBody = Build<TCoIf>(ctx, pos)
            .Predicate<TCoHasItems>()
                .List(points)
            .Build()
            .ThenValue<TCoMap>()
                .Input(points)
                .Lambda()
                    .Args({point})
                    .Body<TExprList>()
                        .Add(leftStruct)
                        .Add(maybeKey)
                    .Build()
                .Build()
            .Build()
            .ElseValue<TCoAsList>()
                .Add<TExprList>()
                    .Add(leftStruct)
                    .Add<TCoNothing>()
                        .OptionalType(NYql::ExpandType(pos, *keyType, ctx))
                    .Build()
                .Build()
            .Build()
        .Done().Ptr();
        // clang-format on
    }

    // Here is a tuple for the left side.
    // clang-format off
    const auto lambda = Build<TCoLambda>(ctx, pos)
        .Args({row})
        .Body(lambdaBody)
    .Done();
    // clang-format on

    auto buildKeys = [&](TExprNode::TPtr body) {
        if (lookup.Prefix) {
            // clang-format off
            return Build<TCoFlatMap>(ctx, pos)
                .Input(body)
                .Lambda(lambda)
            .Done().Ptr();
            // clang-format on
        }
        // clang-format off
        return Build<TCoMap>(ctx, pos)
            .Input(body)
            .Lambda(lambda)
        .Done().Ptr();
        // clang-format on
    };

    const auto newInputStage = NPhysicalConvertionUtils::TransformStageOutput(inputStage, buildKeys, GetReplicateOutputIndex(input), ctx);

    // Tuple: (left row, lookup key).
    const TTypeAnnotationNode::TListType tupleItems{
        ctx.MakeType<TStructExprType>(leftItems),
        keyType,
    };
    const auto* keysType = ctx.MakeType<TListExprType>(ctx.MakeType<TTupleExprType>(tupleItems));

    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical lookup join keys] " << KqpExprToPrettyString(TExprBase(newInputStage), ctx);

    return {newInputStage, NYql::ExpandType(pos, *keysType, ctx), ctx.MakeType<TStructExprType>(leftItems)};
}

TExprNode::TPtr BuildKeysFromInputLookupType(const TOpTableLookup& inputLookup, const TStructExprType* leftRowType,
                                             const TPhysicalNames& names, const TInfoUnitRegistry& registry, TExprContext& ctx) {
    Y_ENSURE(inputLookup.IsJoin(), "Keys are taken from a table lookup in join mode only");
    Y_ENSURE(leftRowType, "Type of the left rows of the input lookup is not available");
    Y_ENSURE(inputLookup.HasSchema(), "Type of the input lookup is not available");

    // The logical type of the input lookup names fetched columns by display name, the stream lookup
    // returns them by storage name.
    const auto* tupleType = ExportOperatorType(inputLookup, ctx)->Cast<TListExprType>()->GetItemType()->Cast<TTupleExprType>();
    const auto* fetchedRowType = tupleType->GetItems()[1]->Cast<TOptionalExprType>()->GetItemType()->Cast<TStructExprType>();
    TVector<const TItemExprType*> keyItems;
    for (const auto id : inputLookup.GetColumns()) {
        // The logical type of a lookup names its fetched columns by information-unit ID.
        const auto* type = fetchedRowType->FindItemType(ToString(id));
        Y_ENSURE(type, "Type of the fetched column " << names.Get(id) << " is not available");
        keyItems.push_back(ctx.MakeType<TItemExprType>(registry.Get(id).GetColumnName(), type));
    }

    // Tuple: (left row, lookup key, cookie).
    const TTypeAnnotationNode::TListType tupleItems{
        leftRowType,
        ctx.MakeType<TOptionalExprType>(ctx.MakeType<TStructExprType>(keyItems)),
        ctx.MakeType<TDataExprType>(EDataSlot::Uint64),
    };
    const auto* keysType = ctx.MakeType<TListExprType>(ctx.MakeType<TTupleExprType>(tupleItems));
    return NYql::ExpandType(inputLookup.Pos, *keysType, ctx);
}

} // namespace NKikimr::NKqp::NLookupJoinBuilder


TExprNode::TPtr TPhysicalIndexLookupJoinBuilder::BuildRenamedRow(const TExprBase& fetchedRow, const TOpTableLookup& lookup,
                                                                bool& needsRename) const {
    const auto row = Build<TCoArgument>(Ctx, Pos).Name("lookup_join_right_row").Done();
    TVector<TExprBase> members;
    needsRename = false;
    for (const auto id : lookup.GetColumns()) {
        const auto column = Registry.Get(id).GetColumnName();
        const auto& name = Names.Get(id);
        needsRename = needsRename || name != column;
        members.push_back(BuildMemberTuple(name, column, row, Ctx, Pos));
    }

    if (!needsRename) {
        return fetchedRow.Ptr();
    }

    // clang-format off
    return Build<TCoMap>(Ctx, Pos)
        .Input(fetchedRow)
        .Lambda()
            .Args({row})
            .Body<TCoAsStruct>()
                .Add(members)
            .Build()
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalIndexLookupJoinBuilder::ProcessFetchedRows(TExprNode::TPtr input, const TOpTableLookup& lookup) const {
    const auto pair = Build<TCoArgument>(Ctx, Pos).Name("lookup_join_pair").Done();
    // clang-format off
    const auto fetchedRow = Build<TCoNth>(Ctx, Pos)
        .Tuple(pair)
        .Index().Value("1").Build()
    .Done();
    // clang-format on

    bool needsRename = false;
    auto processedRow = TExprBase(BuildRenamedRow(fetchedRow, lookup, needsRename));

    if (lookup.FetchedRowFilter) {
        const auto row = Build<TCoArgument>(Ctx, Pos).Name("fetched_row").Done();
        TMappedIUs<TExprNode::TPtr> fields;
        for (const auto id : lookup.FetchedRowFilter->GetRawInputIUs()) {
            fields.Add(id, Build<TCoMember>(Ctx, Pos).Struct(row).Name().Build(Names.Get(id)).Done().Ptr());
        }
        const auto predicate = TExprBase(NPhysicalConvertionUtils::LowerRowLambdaBody(LowerExpressionLambda(*lookup.FetchedRowFilter, Ctx), fields, Ctx));
        // clang-format off
        processedRow = Build<TCoFlatMap>(Ctx, Pos)
            .Input(processedRow)
            .Lambda()
                .Args({row})
                .Body(BuildOptionalIf(predicate, row, Ctx, Pos))
            .Build()
        .Done();
        // clang-format on
    }

    if (!lookup.ResidualJoinKeys.Items().empty()) {
        // clang-format off
        const auto leftRow = Build<TCoNth>(Ctx, Pos)
            .Tuple(pair)
            .Index().Value("0").Build()
        .Done();
        // clang-format on

        const auto rightArg = Build<TCoArgument>(Ctx, Pos).Name("lookup_join_residual_right").Done();
        TVector<TExprBase> equalities;

        // The join keys which are not present in the right side index.
        // We have to evaluate them before apply index lookup join.
        for (const auto& [leftKey, rightKey, equalNulls] : lookup.ResidualJoinKeys.Items()) {
            // clang-format off
            equalities.push_back(Build<TCoCmpEqual>(Ctx, Pos)
                .Left<TCoMember>()
                    .Struct(leftRow)
                    .Name<TCoAtom>().Build(Names.Get(leftKey))
                    .Build()
                .Right<TCoMember>()
                    .Struct(rightArg)
                    .Name<TCoAtom>().Build(Names.Get(rightKey))
                    .Build()
            .Done());
            // clang-format on
        }

        // clang-format off
        const TExprBase pred = equalities.size() == 1
            ? equalities.front()
            : TExprBase(Build<TCoAnd>(Ctx, Pos).Add(equalities).Done());
        // clang-format on

        // clang-format off
        processedRow = Build<TCoFlatMap>(Ctx, Pos)
            .Input(processedRow)
            .Lambda()
                .Args({rightArg})
                .Body(BuildOptionalIf(pred, rightArg, Ctx, Pos))
            .Build()
        .Done();
        // clang-format on
    }

    if (!lookup.FetchedRowFilter && lookup.ResidualJoinKeys.Items().empty() && !needsRename) {
        return input;
    }

    // This is a tuple which represents input for index lookup join.
    // clang-format off
    return Build<TCoMap>(Ctx, Pos)
        .Input(input)
        .Lambda()
            .Args({pair})
            .Body<TExprList>()
                .Add<TCoNth>()
                    .Tuple(pair)
                    .Index().Value("0").Build()
                .Build()
                .Add(processedRow)
                .Add<TCoNth>()
                    .Tuple(pair)
                    .Index().Value("2").Build()
                .Build()
            .Build()
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalIndexLookupJoinBuilder::BuildPhysicalOp(TExprNode::TPtr input) {
    const auto& lookup = LookupJoin.GetTableLookup();

    input = Build<TCoToStream>(Ctx, Pos).Input(input).Done().Ptr();
    input = ProcessFetchedRows(input, lookup);

    // clang-format off
    input = Build<TKqpIndexLookupJoin>(Ctx, Pos)
        .Input(input)
        .JoinType().Build(LookupJoin.JoinKind)
        // TODO: If needed we can also propagate labels.
        .LeftLabel().Build("")
        .RightLabel().Build("")
    .Done().Ptr();
    // clang-format on

    const auto liveOutputs = NPhysicalConvertionUtils::GetLiveOutputIUs(LookupJoin);
    if (liveOutputs.size() != LookupJoin.GetOutputIUs().Size()) {
        input = NPhysicalConvertionUtils::ExtractMembers(input, Ctx, liveOutputs, Names);
    }

    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical index lookup join] " << KqpExprToPrettyString(TExprBase(input), Ctx);

    return input;
}
