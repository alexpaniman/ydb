#include "kqp_rbo_physical_table_effect_builder.h"
#include <algorithm>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

TExprNode::TPtr TPhysicalTableEffectBuilder::BuildPhysicalOp(TExprNode::TPtr input) {
    TVector<std::pair<TString, TString>> columns;
    for (const auto& [id, label] : TableEffect.GetColumns().Items()) {
        columns.emplace_back(Names.Get(id), label);
    }
    return NPhysicalConvertionUtils::BuildRenameMap(input, columns, Ctx, /*ordered=*/true);
}

TExprNode::TPtr NKikimr::NKqp::LowerTableSinkSettings(const TTableSinkSettings& sink, TExprContext& ctx, TPositionHandle Pos, ui64 priority) {
    const auto& Options = sink.Options;
    const auto EffectType = sink.EffectType;

    TString mode;

    switch(EffectType) {
        case EEffectType::InsertRows:
        case EEffectType::InsertRowsIndex:
            Y_ENSURE(!Options.OnConflict || Options.OnConflict->empty() || *Options.OnConflict == "abort",
                "INSERT conflict handling must be lowered before sink emission");
            mode = "insert";
            break;
        case EEffectType::UpdateRows:
        case EEffectType::UpdateRowsIndex:
            mode = "update";
            break;
        case EEffectType::UpsertRows:
        case EEffectType::UpsertRowsIndex:
            mode = "upsert";
            break;
        case EEffectType::DeleteRows:
        case EEffectType::DeleteRowsIndex:
            mode = "delete";
            break;
        default:
            Y_ENSURE(false, "Unsupported DML in new optimizer");
    }

    if (Options.Settings) for (const auto& setting : *Options.Settings) {
        if (!setting.empty() && setting.front() == "Mode") {
            Y_ENSURE(setting.size() == 2);
            mode = setting[1];
        }
    }
    TVector<TExprNode::TPtr> returningColumns;
    if (Options.ReturningColumns) for (const auto& name : *Options.ReturningColumns)
        returningColumns.push_back(ctx.NewAtom(Pos, name));
    // Storage row layouts use canonical names; the result projection restores
    // the SQL RETURNING order after the sink.
    std::sort(returningColumns.begin(), returningColumns.end(), [](const auto& a, const auto& b) {
        return a->Content() < b->Content();
    });

    TString isBatch = "false";
    if (Options.IsBatch.has_value() && Options.IsBatch.value()){
        isBatch = "true";
    }

    TVector<TExprNode::TPtr> defaultColumns;
    if (Options.DefaultColumns.has_value()) {
        for (auto c : Options.DefaultColumns.value()) {
            defaultColumns.push_back(ctx.NewAtom(Pos, c));
        }
    }

    TVector<TExprNode::TPtr> settings;
    if (Options.Settings.has_value()) {
        for (const auto& setting : *Options.Settings) {
            TVector<TExprNode::TPtr> atoms;
            for (const auto& atom : setting) atoms.push_back(ctx.NewAtom(Pos, atom));
            settings.push_back(ctx.NewList(Pos, std::move(atoms)));
        }
    }

    return Build<TKqpTableSinkSettings>(ctx, Pos)
            .Table(LowerTable(sink.Table, ctx, Pos))
            .InconsistentWrite().Build("false")
            .Mode().Build(mode)
            .Priority().Build(ToString(priority))
            .StreamWrite().Build("false")
            .IsBatch().Build(isBatch)
            .IsIndexImplTable().Build("false")
            .DefaultColumns()
                .Add(defaultColumns)
            .Build()
            .ReturningColumns().Add(returningColumns).Build()
            .Settings()
                .Add(settings)
            .Build()
            .Done().Ptr();
}
