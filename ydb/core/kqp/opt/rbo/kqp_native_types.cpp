#include "kqp_native_types.h"
#include "kqp_operator.h"
#include "kqp_rbo_utils.h"

namespace NKikimr::NKqp {
namespace {
using K = NFrontend::ETypeKind;
using NFrontend::TType;

const TNativeRow& Row(const IOperator& op) {
    Y_ENSURE(op.NativeSchema && !op.NativeSchema->LookupLeft, "Expected native row schema for " << op.GetExplainName());
    return op.NativeSchema->Row;
}

TNativeRow ReadRow(const TRboTable& table, const TUnorderedIUs& columns, const TPlanProps& props) {
    Y_ENSURE(table.NativeColumns, "Missing native catalog schema for " << table.Path);
    TNativeRow row;
    for (const auto id : columns) row.emplace(id, table.NativeColumns->at(props.InfoUnitRegistry.Get(id).GetColumnName()));
    return row;
}

TExpression Bind(const TExpression& expression, const TNativeRow& row, TPlanProps& props) {
    Y_ENSURE(expression.IsNative(), "Legacy expression in a native plan");
    return expression.WithNativeColumnTypes([&](TInfoUnitId id) {
        if (auto it = row.find(id); it != row.end()) return it->second;
        const auto& call = props.Subplans.At(id);
        if (call.Type == ESubplanType::EXPR) {
            Y_ENSURE(call.ResultIU, "Scalar subplan has no result binding");
            return call.Plan->GetNativeIUType(*call.ResultIU).Optional();
        }
        TType type{K::Bool};
        if (call.Type == ESubplanType::IN_SUBPLAN) {
            Y_ENSURE(call.Tuple.Items().size() == 1 && call.ResultIU, "Missing native IN bindings");
            const auto lookup = row.at(call.Tuple.Items()[0]);
            const auto item = call.Plan->GetNativeIUType(*call.ResultIU);
            type.OptionalDepth = lookup.OptionalDepth || lookup.Kind == K::Null || item.OptionalDepth || item.Kind == K::Null;
        }
        return type;
    });
}

void Append(TNativeRow& to, const TNativeRow& from, bool optional = false) {
    for (const auto& [id, type] : from) to.emplace(id, optional ? type.Optional() : type);
}

bool OptionalOnEmpty(TStringBuf function) {
    return function == "min" || function == "max" || function == "sum" || function == "avg" ||
        function == "variance_1_1" || function == "some";
}
} // namespace

NFrontend::TType IOperator::GetNativeIUType(TInfoUnitId iu) const {
    Y_ENSURE(NativeSchema, "Native type access on a legacy or untyped operator");
    if (auto it = NativeSchema->Row.find(iu); it != NativeSchema->Row.end()) return it->second;
    if (NativeSchema->LookupLeft) return NativeSchema->LookupLeft->at(iu);
    Y_ENSURE(false, "Missing native type for IU " << iu << " in " << GetExplainName());
}

TType NativeAggregateType(TType input, const TOpAggregationTraits& traits, EOpPhase phase,
    NFrontend::TExpressionArena& arena) {
    const auto& function = traits.AggFunction;
    if (function == "count") return {K::Uint64};
    if ((function == "avg" || function == "variance_1_1") && phase != EOpPhase::Undefined) {
        TType result;
        if (phase == EOpPhase::Intermediate) {
            Y_ENSURE(input.IsNumeric(), "Native aggregate partial input must be numeric");
            if (function == "variance_1_1") {
                Y_ENSURE(input.Kind != K::Decimal, "Native sample deviation does not accept Decimal");
                const TType fields[]{{K::Double}, {K::Double}, {K::Double}};
                result = arena.TupleType(fields);
            } else {
                auto sum = input.Kind == K::Decimal ? input.Required() : TType{K::Double};
                if (sum.Kind == K::Decimal) sum.Precision = 35;
                const TType fields[]{sum, {K::Uint64}};
                result = arena.TupleType(fields);
            }
        } else {
            Y_ENSURE(input.Kind == K::Tuple, "Native aggregate final input must be a state tuple");
            const auto fields = arena.TupleItems(input);
            if (function == "variance_1_1") {
                Y_ENSURE(fields.size() == 3, "Invalid sample-deviation state");
                for (auto field : fields) Y_ENSURE(field == TType{K::Double}, "Invalid sample-deviation state field");
                result = {K::Double};
            } else {
                Y_ENSURE(fields.size() == 2 && fields[1] == TType{K::Uint64} && traits.ResultType,
                    "Invalid AVG state or missing result type");
                result = *traits.ResultType;
            }
        }
        result.OptionalDepth = std::max(result.OptionalDepth, input.OptionalDepth);
        return result;
    }
    if (function == "sum" || function == "avg" || function == "variance_1_1") {
        Y_ENSURE(input.IsNumeric() || (function == "sum" && input.Kind == K::Interval), "Native aggregate input must be numeric or a SUM interval");
        if ((function == "avg" && input.Kind != K::Decimal) || function == "variance_1_1")
            return {K::Double, input.OptionalDepth};
        if (input.IsIntegral()) input.Kind = input.IsSigned() ? K::Int64 : K::Uint64;
        if (input.Kind == K::Decimal && function == "sum") input.Precision = 35;
        // Interval accumulation can overflow even when every input is required.
        if (input.Kind == K::Interval && function == "sum") input = input.Optional();
    } else {
        Y_ENSURE(function == "min" || function == "max" || function == "some" || function == "distinct",
            "Unsupported native aggregation " << function);
    }
    return input;
}

void ComputeNativeTypes(IOperator& op, TPlanProps& props) {
    Y_ENSURE(props.NativeExpressions, "Missing native query arena");
    TNativeSchema schema{{}, std::nullopt, props.NativeExpressions};
    auto& row = schema.Row;
    switch (op.Kind) {
        case EOperator::Source: {
            auto& read = CastOperator<TOpRead>(op);
            row = ReadRow(read.TableRef, read.GetColumns(), props);
            if (read.OriginalPredicate) read.OriginalPredicate = Bind(*read.OriginalPredicate, row, props);
            if (read.OlapProgram.IsNative()) {
                auto& program = read.OlapProgram.Native();
                if (program.Predicate) program.Predicate = Bind(*program.Predicate, row, props);
                for (const auto id : program.Projections.Keys())
                    program.Projections.At(id) = Bind(program.Projections.At(id), row, props);
                for (const auto id : program.Projections.Keys()) if (row.contains(id))
                    row.at(id) = program.Projections.At(id).GetNativeType();
            }
            break;
        }
        case EOperator::EmptySource: {
            const auto& source = CastOperator<TOpEmptySource>(op);
            if (source.NativeInput) for (const auto& [id, field] : source.NativeInput->Fields.Items()) row.emplace(id, field.Type);
            break;
        }
        case EOperator::Replicate: {
            auto& port = CastOperator<TOpReplicate>(op);
            for (const auto& [id, type] : Row(*port.GetInput()))
                row.emplace(port.IsPrimary() ? id : port.GetRebindings().At(id), type);
            break;
        }
        case EOperator::Filter: {
            auto& filter = CastOperator<TOpFilter>(op);
            row = Row(*filter.GetInput());
            auto predicate = Bind(filter.GetFilterExpression(), row, props);
            Y_ENSURE(predicate.GetNativeType().Kind == K::Bool, "Native filter must be Bool");
            filter.SetFilterExpression(std::move(predicate));
            break;
        }
        case EOperator::Map: {
            auto& map = CastOperator<TOpMap>(op);
            const auto& input = Row(*map.GetInput());
            row = input;
            for (const auto output : map.GetMapElements().Keys()) {
                auto expression = Bind(map.GetMapElements().At(output).GetExpression(), input, props);
                row.emplace(output, expression.GetNativeType());
                map.SetMapElementExpression(output, std::move(expression));
            }
            break;
        }
        case EOperator::AddDependencies: {
            const auto& dependencies = CastOperator<TOpAddDependencies>(op);
            row = Row(*dependencies.GetInput());
            for (const auto& [id, capture] : dependencies.GetDependencies().Items()) {
                Y_ENSURE(capture.NativeType, "Missing capture type");
                row.emplace(id, *capture.NativeType);
            }
            break;
        }
        case EOperator::UnionAll: {
            auto& unionAll = CastOperator<TOpUnionAll>(op);
            for (const auto& [id, column] : unionAll.GetColumns().Items()) {
                auto type = unionAll.GetChild(0)->GetNativeIUType(column.Inputs[0]);
                for (size_t i = 1; i < column.Inputs.size(); ++i) {
                    auto next = unionAll.GetChild(i)->GetNativeIUType(column.Inputs[i]);
                    Y_ENSURE(type.Required() == next.Required(), "Native UnionAll inputs require explicit conversion");
                    type.OptionalDepth = std::max(type.OptionalDepth, next.OptionalDepth);
                }
                row.emplace(id, type);
            }
            break;
        }
        case EOperator::Aggregate: {
            const auto& aggregate = CastOperator<TOpAggregate>(op);
            const auto& input = *aggregate.GetInput();
            if (!aggregate.IsDistinctAll()) for (auto key : aggregate.GetKeyColumns().Items()) row.emplace(key, input.GetNativeIUType(key));
            for (const auto& [id, traits] : aggregate.GetAggregationTraits().Items()) {
                const auto phase = aggregate.GetAggregationPhase();
                auto type = NativeAggregateType(input.GetNativeIUType(traits.Input), traits, phase, *props.NativeExpressions);
                bool countState = false;
                if (phase == EOpPhase::Final && input.Kind == EOperator::Aggregate) {
                    const auto* source = CastOperator<TOpAggregate>(input).GetAggregationTraits().Find(traits.Input);
                    countState = source && source->AggFunction == "count";
                }
                if (phase != EOpPhase::Intermediate && aggregate.GetKeyColumns().Items().empty() &&
                    OptionalOnEmpty(traits.AggFunction) && !countState) type = type.Optional();
                // DISTINCT expansion uses the existing aggregate flag to
                // remove nullable union padding at the aggregate boundary.
                if (traits.Unwrap) type = type.Unwrapped();
                row.emplace(id, type);
            }
            break;
        }
        case EOperator::GroupingSets: {
            const auto& sets = CastOperator<TOpGroupingSets>(op);
            const auto& aggregate = CastOperator<TOpAggregate>(*sets.GetInput());
            auto common = aggregate.GetKeyColumns().Unordered();
            bool empty = false;
            for (const auto& set : sets.GetGroupingSets()) { common.IntersectWith(set); empty |= set.Empty(); }
            for (const auto& [id, source] : sets.GetColumns().Items()) {
                auto type = aggregate.GetNativeIUType(source);
                const auto* traits = aggregate.GetAggregationTraits().Find(source);
                if ((aggregate.GetKeyColumns().Unordered().Contains(source) && !common.Contains(source)) ||
                    (empty && traits && OptionalOnEmpty(traits->AggFunction))) type = type.Optional();
                row.emplace(id, type);
            }
            for (auto id : sets.GetGroupingIndicators().Keys()) row.emplace(id, TType{K::Uint64});
            break;
        }
        case EOperator::Join: {
            auto& join = CastOperator<TOpJoin>(op);
            const auto& left = Row(*join.GetLeftInput());
            const auto& right = Row(*join.GetRightInput());
            auto cross = left;
            Append(cross, right);
            for (auto& filter : join.JoinFilters) filter = Bind(filter, cross, props);
            if (JoinOutputsLeft(join.JoinKind)) Append(row, left, join.JoinKind == "Right" || join.JoinKind == "Full" || join.JoinKind == "Exclusion");
            if (JoinOutputsRight(join.JoinKind)) Append(row, right, join.JoinKind == "Left" || join.JoinKind == "Full" || join.JoinKind == "Exclusion");
            break;
        }
        case EOperator::DependentJoin: {
            const auto& join = CastOperator<TOpDependentJoin>(op);
            row = Row(*join.GetDomain());
            Append(row, Row(*join.GetInput()));
            break;
        }
        case EOperator::Limit:
        case EOperator::Sort: {
            row = Row(*op.GetChild(0));
            op.RewriteExpressions([&](const TExpression& expression) { return Bind(expression, row, props); });
            break;
        }
        case EOperator::Window: {
            const auto& window = CastOperator<TOpWindow>(op);
            row = Row(*window.GetInput());
            for (const auto& [id, function] : window.GetWindowFuncs().Items()) {
                TType type{K::Uint64};
                if (function.Kind != EWindowFuncKind::Native) {
                    Y_ENSURE(function.Arguments.Items().size() == 1, "Window aggregate needs one argument");
                    const auto arg = function.Arguments.Items()[0];
                    type = NativeAggregateType(window.GetInput()->GetNativeIUType(arg), {arg, function.Function},
                        EOpPhase::Undefined, *props.NativeExpressions);
                    if (function.Function != "count" && !window.GetFrame().IsNeverEmpty()) type = type.Optional();
                }
                row.emplace(id, type);
            }
            break;
        }
        case EOperator::TableEffect: {
            const auto& effect = CastOperator<TOpTableEffect>(op);
            Y_ENSURE(effect.Table.NativeColumns, "Native write requires a catalog schema");
            for (const auto& [id, name] : effect.GetColumns().Items())
                Y_ENSURE(effect.GetInput()->GetNativeIUType(id) == effect.Table.NativeColumns->at(name),
                    "Write inputs must be coerced at the native binding boundary");
            for (const auto& [id, name] : effect.GetReturningColumns().Items())
                row.emplace(id, effect.Table.NativeColumns->at(name));
            break;
        }
        case EOperator::TableLookup: {
            auto& lookup = CastOperator<TOpTableLookup>(op);
            row = ReadRow(lookup.Table, lookup.GetColumns(), props);
            if (lookup.FetchedRowFilter) {
                auto predicate = Bind(*lookup.FetchedRowFilter, row, props);
                Y_ENSURE(predicate.GetNativeType().Kind == K::Bool, "Native lookup filter must be Bool");
                lookup.FetchedRowFilter = std::move(predicate);
            }
            if (lookup.IsJoin()) schema.LookupLeft = Row(*lookup.GetLeftInput());
            break;
        }
        case EOperator::IndexLookupJoin: {
            const auto& join = CastOperator<TOpIndexLookupJoin>(op);
            const auto& input = join.GetInput()->NativeSchema;
            Y_ENSURE(input && input->LookupLeft, "Lookup join requires a lookup pair schema");
            row = *input->LookupLeft;
            if (JoinOutputsRight(join.JoinKind)) Append(row, input->Row, join.JoinKind == "Left");
            break;
        }
        case EOperator::CBOTree: {
            auto& tree = CastOperator<TOpCBOTree>(op);
            for (auto& node : tree.TreeNodes) ComputeNativeTypes(*node, props);
            schema = tree.TreeRoot->NativeSchema.value();
            break;
        }
        default: Y_ENSURE(false, "Unsupported native operator " << op.GetExplainName());
    }
    op.NativeSchema = std::move(schema);
}

} // namespace NKikimr::NKqp
