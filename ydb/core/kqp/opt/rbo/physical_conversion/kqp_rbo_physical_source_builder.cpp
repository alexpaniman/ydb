#include "kqp_rbo_physical_source_builder.h"

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/opt/rbo/kqp_olap_expr_inspection.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/library/yql/dq/type_ann/dq_type_ann.h>

#include <yql/essentials/core/yql_expr_optimize.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

namespace {

// RangeFinalize is an executable MiniKQL operation. Its input is a list of
// (lower, upper) pairs. Each bound carries (infinity marker, optional key) for
// every physical key column, followed by its inclusion marker. The outer
// optional distinguishes an unbounded suffix from a finite SQL NULL key.
// RangeUnion sorts and merges singleton intervals before finalization, so
// overlapping OR branches never cause duplicate reads. Both operations are
// executable directly, without AsRange/RangeFor macros or annotation passes.
TExprNode::TPtr LowerNativeRanges(const TOpRead::TRangeInfo& info, const TStructExprType& schema,
    TExprContext& ctx, TPositionHandle pos) {
    Y_ENSURE(info.NativeRanges && !info.ComputeNode && !info.LiteralRange);
    NFrontend::TExpressionArena constants;
    const auto scalar = [&](NFrontend::TType type, NFrontend::TScalar value) {
        return LowerNativeLambda(constants, constants.Literal(type, std::move(value)), ctx, pos)->TailPtr();
    };
    const auto negative = scalar({NFrontend::ETypeKind::Int32}, std::int64_t{-1});
    const auto zero = scalar({NFrontend::ETypeKind::Int32}, std::int64_t{0});
    const auto positive = scalar({NFrontend::ETypeKind::Int32}, std::int64_t{1});
    const auto call = [&](TStringBuf name, TExprNode::TListType children, const TTypeAnnotationNode* type) {
        auto result = ctx.NewCallable(pos, name, std::move(children));
        result->SetTypeAnn(type);
        return result;
    };
    const auto tuple = [&](TExprNode::TListType values) {
        TTypeAnnotationNode::TListType types;
        for (const auto& value : values) types.push_back(value->GetTypeAnn());
        auto result = ctx.NewList(pos, std::move(values));
        result->SetTypeAnn(ctx.MakeType<TTupleExprType>(types));
        return result;
    };
    TTypeAnnotationNode::TListType resultTypes;
    for (const auto& key : info.KeyColumns) {
        const auto* type = schema.FindItemType(key);
        Y_ENSURE(type, "Missing native read key type");
        resultTypes.push_back(ctx.MakeType<TOptionalExprType>(type));
    }
    const auto lower = [&](const TOpRead::TKeyBound& bound, bool left) {
        Y_ENSURE(bound.Values.size() <= info.KeyColumns.size());
        TExprNode::TListType values;
        const bool negativeInfinity = bound.Values.empty() || bound.Inclusive ? left : !left;
        for (size_t i = 0; i < info.KeyColumns.size(); ++i) {
            const auto* keyType = schema.FindItemType(info.KeyColumns[i]);
            Y_ENSURE(keyType, "Missing native read key type");
            const auto* optionalType = ctx.MakeType<TOptionalExprType>(keyType);
            if (i < bound.Values.size()) {
                const auto& expression = bound.Values[i];
                Y_ENSURE(expression.IsNative() && expression.GetRawInputIUs().Empty(), "Read bound depends on a row");
                auto value = LowerNativeLambda(*expression.GetNativeArena(), expression.GetNativeRoot(), ctx, pos)->TailPtr();
                if (keyType->GetKind() == ETypeAnnotationKind::Optional && value->GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional)
                    value = call("Just", {value}, keyType);
                Y_ENSURE(IsSameAnnotation(*value->GetTypeAnn(), *keyType), "Native range bound does not match the storage key type");
                values.push_back(zero);
                values.push_back(call("Just", {value}, optionalType));
            } else {
                // Lowering a native NULL supplies fully typed scalar type
                // syntax; add one backend-only optional layer for nullable keys.
                auto empty = scalar(ImportNativeType(*keyType).Optional(), {});
                if (keyType->GetKind() == ETypeAnnotationKind::Optional) {
                    const auto type = call("OptionalType", {empty->HeadPtr()}, ctx.MakeType<TTypeExprType>(optionalType));
                    empty = call("Nothing", {type}, optionalType);
                }
                values.push_back(negativeInfinity ? negative : positive);
                values.push_back(std::move(empty));
            }
        }
        // The storage decoder reserves an inclusive empty upper endpoint for
        // point lookup encoding. Positive infinity must therefore be exclusive,
        // even though inclusion has no meaning for an infinite native bound.
        const bool inclusive = bound.Inclusive && (left || !bound.Values.empty());
        values.push_back(inclusive ? (left ? negative : positive) : zero);
        return tuple(std::move(values));
    };
    TExprNode::TListType lists;
    for (const auto& range : *info.NativeRanges) {
        auto pair = tuple({lower(range.Lower, true), lower(range.Upper, false)});
        lists.push_back(call("AsList", {pair}, ctx.MakeType<TListExprType>(pair->GetTypeAnn())));
    }
    Y_ENSURE(!lists.empty(), "Native range extraction must produce an interval");
    const TTypeAnnotationNode* listType = lists.front()->GetTypeAnn();
    auto list = lists.size() == 1 ? lists.front() : call("RangeUnion", std::move(lists), listType);
    resultTypes.push_back(zero->GetTypeAnn());
    const auto* boundaryType = ctx.MakeType<TTupleExprType>(resultTypes);
    const auto* rangeType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{boundaryType, boundaryType});
    return call("RangeFinalize", {list}, ctx.MakeType<TListExprType>(rangeType));
}

} // namespace

TExprNode::TPtr TPhysicalSourceBuilder::BuildPhysicalOp() {
    TExprNode::TPtr source;
    TVector<TString> storageColumns;
    TVector<std::pair<TString, TString>> renames;
    THashMap<TString, TString> olapNames;
    for (const auto id : Read.GetColumns()) {
        const auto column = Registry.Get(id).GetColumnName();
        storageColumns.push_back(column);
        renames.emplace_back(column, Names.Get(id));
        olapNames.emplace(Ctx.GetIndexAsString(id), column);
    }
    // OLAP execution needs a nonempty storage projection to retain row counts.
    // The carrier has no logical ID and is dropped by the NarrowMap below.
    if (storageColumns.empty() && Read.GetTableStorageType() == NYql::EStorageType::ColumnStorage) {
        Y_ENSURE(!CarrierColumn.empty(), "An empty OLAP payload needs a storage carrier column");
        storageColumns.push_back(CarrierColumn);
    }
    // Block reads expose TStructExprType's lexical field order, not ID order.
    // Fetch a storage field once even when multiple logical IDs refer to it.
    std::sort(storageColumns.begin(), storageColumns.end());
    storageColumns.erase(std::unique(storageColumns.begin(), storageColumns.end()), storageColumns.end());
    TVector<TExprNode::TPtr> columns;
    THashMap<TString, ui32> positions;
    for (const auto& column : storageColumns) {
        positions.emplace(column, columns.size());
        columns.push_back(Ctx.NewAtom(Pos, column));
    }
    // Extract ranges.
    TExprNode::TPtr ranges = Read.GetRanges() ? Read.GetRanges() : Build<TCoVoid>(Ctx, Pos).Done().Ptr();

    switch (Read.GetTableStorageType()) {
        case NYql::EStorageType::RowStorage: {
            // A literal range is passed to the source as is, so it needs no materialization.
            if (const auto literalRange = Read.GetLiteralRange()) {
                ranges = literalRange;
            }
            if (Read.RangeInfo && Read.RangeInfo->NativeRanges && Read.RangeInfo->NativeRanges->size() == 1) {
                Y_ENSURE(!Read.GetRanges() && !Read.GetLiteralRange(), "Mixed native and legacy read ranges");
                const auto lower = [&](const TOpRead::TKeyBound& bound) {
                    TExprNode::TListType values;
                    for (const auto& value : bound.Values) {
                        Y_ENSURE(value.IsNative() && value.GetRawInputIUs().Empty(), "Read bound depends on a row");
                        values.push_back(LowerNativeLambda(*value.GetNativeArena(), value.GetNativeRoot(), Ctx, Pos)->TailPtr());
                    }
                    return Ctx.NewCallable(Pos, bound.Inclusive ? "KqlKeyInc" : "KqlKeyExc", std::move(values));
                };
                const auto& range = Read.RangeInfo->NativeRanges->front();
                ranges = Build<TKqlKeyRange>(Ctx, Pos)
                    .From(TKqlKeyTuple(lower(range.Lower)))
                    .To(TKqlKeyTuple(lower(range.Upper)))
                    .Done().Ptr();
            } else if (Read.RangeInfo && Read.RangeInfo->NativeRanges) {
                Y_ENSURE(TableSchema, "Native ranges require the complete storage key schema");
                ranges = LowerNativeRanges(*Read.RangeInfo, *TableSchema, Ctx, Pos);
            }

            TKqpReadTableSettings settings;
            if (Read.SortDir != ESortDir::None) {
                settings.SetSorting(Read.SortDir == ESortDir::Asc ? ERequestSorting::ASC : ERequestSorting::DESC);
                if (Read.Limit) {
                    settings.SetItemsLimit(Build<TCoUint64>(Ctx, Pos).Literal().Build(::ToString(*Read.Limit)).Done().Ptr());
                }
            }

            TExprNode::TPtr sourceSettings;
            // System views have no datashard partitions and need their own reader.
            if (IsSysView) {
                // clang-format off
                sourceSettings = Build<TKqpReadSysViewSourceSettings>(Ctx, Pos)
                    .Table(Read.TableCallable)
                    .Columns().Add(columns).Build()
                    .Settings(settings.BuildNode(Ctx, Pos))
                    .RangesExpr(ranges)
                .Done().Ptr();
                // clang-format on
            } else {
                // clang-format off
                sourceSettings = Build<TKqpReadRangesSourceSettings>(Ctx, Pos)
                    .Table(Read.TableCallable)
                    .Columns()
                        .Add(columns)
                    .Build()
                    .Settings(settings.BuildNode(Ctx, Pos))
                    .RangesExpr(ranges)
                    .ExplainPrompt<TCoNameValueTupleList>().Build()
                .Done().Ptr();
                // clang-format on
            }

            // clang-format off
            source = Build<TDqSource>(Ctx, Pos)
                .DataSource<TCoDataSource>()
                    .Category<TCoAtom>().Build(IsSysView ? NYql::KqpSysViewSourceName : NYql::KqpReadRangesSourceName)
                .Build()
                .Settings(sourceSettings)
            .Done().Ptr();
            // clang-format on

            const auto programArg = Build<TCoArgument>(Ctx, Pos).Name("program_arg").Done().Ptr();
            const auto renameMap = NPhysicalConvertionUtils::BuildRenameMap(programArg, renames, Ctx);
            // clang-format off
            source = Build<TDqPhyStage>(Ctx, Pos)
                .Inputs()
                    .Add({source})
                .Build()
                .Program()
                    .Args({programArg})
                    .Body(renameMap)
                .Build()
                .Settings(NYql::NDq::TDqStageSettings::New(StageGUID).BuildNode(Ctx, Pos))
            .Done().Ptr();
            // clang-format on
            break;
        }
        case NYql::EStorageType::ColumnStorage: {
            if (Read.RangeInfo && Read.RangeInfo->NativeRanges) {
                Y_ENSURE(TableSchema, "Native column ranges require the complete storage key schema");
                ranges = LowerNativeRanges(*Read.RangeInfo, *TableSchema, Ctx, Pos);
            }
            // clang-format off
            auto processLambda = Build<TCoLambda>(Ctx, Pos)
                .Args({"arg"})
                .Body("arg")
            .Done().Ptr();
            // clang-format on

            if (Read.OlapProgram) {
                // Do not carry the optimizer's ID-keyed argument type across the
                // storage-name boundary. The read annotator supplies the storage row.
                const auto program = Read.OlapProgram.IsNative() ? LowerNativeOlapProgram(Read.OlapProgram, Ctx, Pos) : Read.OlapProgram.Legacy();
                processLambda = Ctx.DeepCopyLambda(*NOpt::TOlapFilterInspector::RenameColumns(program, olapNames, Ctx));
            }

            TKqpReadTableSettings settings;
            if (Read.Limit) {
                settings.SetItemsLimit(Build<TCoUint64>(Ctx, Pos).Literal().Build(::ToString(*Read.Limit)).Done().Ptr());
            }

            if (Read.SortDir != ESortDir::None) {
                const auto sortDirection = Read.SortDir == ESortDir::Asc ? ERequestSorting::ASC : ERequestSorting::DESC;
                settings.SetSorting(sortDirection);
            } else if (Read.Limit) {
                // Limit without sort.
                settings.SequentialInFlight = 1;
            }

            // clang-format off
            auto olapRead = Build<TKqpBlockReadOlapTableRanges>(Ctx, Pos)
                .Table(Read.TableCallable)
                .Ranges(ranges)
                .Columns().Add(columns).Build()
                .Settings(settings.BuildNode(Ctx, Pos))
                .ExplainPrompt<TCoNameValueTupleList>().Build()
                .Process(processLambda)
            .Done().Ptr();

            // From blocks.
            auto flowNonBlockRead = Build<TCoToFlow>(Ctx, Pos)
                .Input<TCoWideFromBlocks>()
                    .Input<TCoFromFlow>()
                        .Input(olapRead)
                    .Build()
                .Build()
            .Done().Ptr();
            // clang-format on

            TExprNode::TListType args;
            for (ui32 i = 0; i < storageColumns.size(); ++i) {
                args.push_back(Ctx.NewArgument(Pos, "column_" + ToString(i)));
            }
            TExprNode::TListType fields;
            for (const auto& [column, name] : renames) {
                fields.push_back(Ctx.NewList(Pos, {Ctx.NewAtom(Pos, name), args.at(positions.at(column))}));
            }
            auto row = Ctx.NewCallable(Pos, "AsStruct", std::move(fields));
            auto lambda = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), std::move(row));
            auto narrowMap = Build<TCoNarrowMap>(Ctx, Pos).Input(flowNonBlockRead).Lambda(lambda).Done().Ptr();

            // clang-format off
            source = Build<TCoFromFlow>(Ctx, Pos)
                .Input(narrowMap)
            .Done().Ptr();
            // clang-format on
            break;
        }
        default:
            Y_ENSURE(false, "Unsupported table source type.");
    }

    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical source] " << KqpExprToPrettyString(TExprBase(source), Ctx);

    return source;
}
