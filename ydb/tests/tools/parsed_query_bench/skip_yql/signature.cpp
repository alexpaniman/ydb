#include "signature.h"

#include <ydb/core/kqp/opt/rbo/kqp_native_types.h>
#include <util/string/builder.h>
#include <util/string/cast.h>

#include <algorithm>
#include <bit>
#include <map>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

namespace F = NFrontend;

TString Tuple(TStringBuf name, const TVector<TString>& parts) {
    TStringBuilder result;
    result << name << '(';
    for (const auto& part : parts) {
        Y_ENSURE(result.size() + part.size() < (16 << 20), "Logical comparison size limit exceeded");
        result << part.size() << ':' << part;
    }
    return result << ')';
}

TString Type(const F::TExpressionArena& arena, F::TType type) {
    if (!type.IsCollection()) {
        return TString(type.ToString());
    }
    TVector<TString> parts{ToString(type.OptionalDepth)};
    if (type.Kind == F::ETypeKind::List) {
        parts.push_back(Type(arena, arena.ListItem(type)));
        return Tuple("list-type", parts);
    }
    const bool structure = type.Kind == F::ETypeKind::Struct;
    const auto items = structure ? arena.StructItems(type) : arena.TupleItems(type);
    for (size_t index = 0; index < items.size(); ++index) {
        const auto item = Type(arena, items[index]);
        parts.push_back(structure ? Tuple("field", {TString(arena.StructNames(type)[index]), item}) : item);
    }
    return Tuple(structure ? "struct-type" : "tuple-type", parts);
}

TString Scalar(const F::TScalar& value) {
    return std::visit([](const auto& item) -> TString {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return "null";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return Tuple("bytes", {TString(item)});
        } else if constexpr (std::is_same_v<T, double>) {
            return ToString(std::bit_cast<ui64>(item));
        } else if constexpr (std::is_same_v<T, F::TDecimal>) {
            const auto bits = static_cast<unsigned __int128>(item.Coefficient);
            return Tuple("decimal", {ToString(static_cast<ui64>(bits >> 64)), ToString(static_cast<ui64>(bits))});
        } else if constexpr (std::is_same_v<T, F::TTuple> || std::is_same_v<T, F::TOptionalValue>) {
            ythrow yexception() << "Unsupported composite literal in logical comparison";
        } else {
            return ToString(item);
        }
    }, value);
}

struct TValue {
    TString Signature;
    bool NonNullLiteral = false;
};

using TValues = THashMap<TInfoUnitId, TValue>;

class TScalarSignature {
public:
    TScalarSignature(const F::TExpressionArena& arena, const TValues& values)
        : Arena(arena)
        , Values(values)
    {}

    TValue Read(F::TExprId id, size_t depth = 0) {
        Y_ENSURE(depth < 1024, "Logical expression comparison depth exceeded");
        if (const auto* previous = Cache.FindPtr(id)) {
            return *previous;
        }
        const auto& node = Arena[id];
        using O = F::EOpcode;
        Y_ENSURE(node.Repeatable && node.Op != O::Ensure && node.Op != O::Unwrap,
            "Unsupported volatile or asserting expression in logical comparison");
        if (node.Op == O::Column) {
            return Values.at(node.ColumnId);
        }
        TVector<TString> parts{ToString(static_cast<unsigned>(node.Op)), Type(Arena, node.Type), TString(node.Name),
            ToString(node.Value.index()), Scalar(node.Value)};
        for (const auto child : node.Arguments) {
            parts.push_back(Read(child, depth + 1).Signature);
        }
        TValue result{Tuple("expression", parts),
            node.Op == O::Literal && !node.Type.OptionalDepth && !std::holds_alternative<std::monostate>(node.Value)};
        Cache.emplace(id, result);
        return result;
    }

private:
    const F::TExpressionArena& Arena;
    const TValues& Values;
    THashMap<F::TExprId, TValue> Cache;
};

TValue Expression(const TExpression& expression, const TValues& values) {
    Y_ENSURE(expression.IsNative(), "Expected an arena expression in logical comparison");
    return TScalarSignature(*expression.GetNativeArena(), values).Read(expression.GetNativeRoot());
}

struct TRelation {
    TString Signature;
    TValues Values;
};

class TSignature {
public:
    TString Read(TOpRoot& root) {
        auto& props = root.PlanProps;
        Y_ENSURE(props.ResultIndex && props.Subplans.Empty() && !props.WithEffects && !props.WithReturning
            && !props.MaterializedResult && !props.ReturningRowsLimit, "Unsupported root in logical comparison");
        const auto& input = ReadRelation(*root.GetInput(), props, 0);
        TVector<TString> parts{ToString(*props.ResultIndex), ToString(props.PhysicalTxGroup), input.Signature};
        const auto& arena = *root.GetInput()->NativeSchema->Arena;
        for (const auto& [id, label] : root.GetColumns().Items()) {
            parts.push_back(Tuple("output", {label, Type(arena, root.GetInput()->GetNativeIUType(id)),
                input.Values.at(id).Signature}));
        }
        return Tuple("result", parts);
    }

private:
    const TRelation& ReadRelation(IOperator& op, TPlanProps& props, size_t depth) {
        Y_ENSURE(depth < 1024, "Logical relation comparison depth exceeded");
        if (const auto it = Relations.find(&op); it != Relations.end()) {
            return it->second;
        }
        TVector<const TRelation*> children;
        for (auto* child : op.GetChildren()) {
            children.push_back(&ReadRelation(*child, props, depth + 1));
        }
        ComputeNativeTypes(op, props);
        auto relation = Operator(op, props, children);
        if (const auto& order = op.Props.OrderEnforcer; order && order->Reason == USER) {
            TVector<TString> parts{relation.Signature, ToString(static_cast<unsigned>(order->Action))};
            AppendSortKeys(parts, order->SortElements, relation.Values);
            relation.Signature = Tuple("required-order", parts);
        }
        return Relations.emplace(&op, std::move(relation)).first->second;
    }

    void AppendSortKeys(TVector<TString>& parts, const TSortIUs& order, const TValues& values) const {
        for (const auto& [id, spec] : order.Items()) {
            parts.push_back(Tuple("sort-key", {values.at(id).Signature,
                ToString(spec.Ascending), ToString(spec.NullsFirst)}));
        }
    }

    TRelation Operator(IOperator& op, TPlanProps& props, const TVector<const TRelation*>& children) {
        switch (op.Kind) {
            case EOperator::EmptySource: {
                const auto& source = CastOperator<TOpEmptySource>(op);
                if (!source.NativeInput) {
                    return {Tuple("unit", {}), {}};
                }
                TRelation result{Tuple("parameter-source", {source.NativeInput->Name}), {}};
                for (const auto& [id, field] : source.NativeInput->Fields.Items()) {
                    result.Values.emplace(id, TValue{Tuple("field", {TString(field.Name),
                        Type(*op.NativeSchema->Arena, field.Type)})});
                }
                return result;
            }
            case EOperator::Source: {
                const auto& read = CastOperator<TOpRead>(op);
                Y_ENSURE(!read.RangeInfo && !read.OlapProgram.IsNative() && !read.Limit && !read.OriginalPredicate
                    && read.SortDir == ESortDir::None, "Unsupported scan restriction in logical comparison");
                const auto& table = read.GetTable();
                TRelation result{Tuple("scan", {ToString(++Scans), table.Path, table.SysView}), {}};
                for (const auto id : read.GetColumns()) {
                    result.Values.emplace(id, TValue{Tuple("column", {result.Signature,
                        props.InfoUnitRegistry.Get(id).GetColumnName(), Type(*op.NativeSchema->Arena, op.GetNativeIUType(id))})});
                }
                return result;
            }
            case EOperator::Replicate: {
                auto& port = CastOperator<TOpReplicate>(op);
                const auto& input = *children.at(0);
                if (port.IsPrimary() && port.GetReplicate().GetOutputs().size() == 1) {
                    return input;
                }
                TRelation result{Tuple("shared-port", {input.Signature, ToString(++Ports)}), {}};
                for (const auto& [id, value] : input.Values) {
                    const auto output = port.IsPrimary() ? id : port.GetRebindings().At(id);
                    result.Values.emplace(output, TValue{Tuple("shared-value", {result.Signature, value.Signature}),
                        value.NonNullLiteral});
                }
                return result;
            }
            case EOperator::Map: {
                const auto& input = *children.at(0);
                auto result = input;
                for (const auto& [id, elem] : CastOperator<TOpMap>(op).GetMapElements().Items()) {
                    auto value = Expression(elem.GetExpression(), input.Values);
                    Y_ENSURE(result.Values.emplace(id, std::move(value)).second, "Duplicate map output");
                }
                return result;
            }
            case EOperator::Filter: {
                const auto& input = *children.at(0);
                const auto& expr = CastOperator<TOpFilter>(op).GetFilterExpression();
                return {Tuple("filter", {input.Signature, Expression(expr, input.Values).Signature}), input.Values};
            }
            case EOperator::Join: {
                const auto& join = CastOperator<TOpJoin>(op);
                const auto& left = *children.at(0);
                const auto& right = *children.at(1);
                TVector<TString> keys;
                for (const auto& key : join.JoinKeys.Items()) {
                    keys.push_back(Tuple(key.EqualNulls ? "null-safe" : "equal",
                        {left.Values.at(key.first).Signature, right.Values.at(key.second).Signature}));
                }
                // Equijoin keys are a conjunction of already computed columns.
                std::sort(keys.begin(), keys.end());
                auto both = left.Values;
                for (const auto& [id, value] : right.Values) {
                    Y_ENSURE(both.emplace(id, value).second, "Overlapping join inputs");
                }
                const auto occurrence = ToString(++Joins);
                TVector<TString> parts{join.JoinKind, left.Signature, right.Signature, Tuple("keys", keys)};
                for (const auto& filter : join.JoinFilters) {
                    parts.push_back(Expression(filter, both).Signature);
                }
                TRelation result{Tuple("join", parts), {}};
                for (const auto id : op.GetOutputIUs()) {
                    const bool leftSide = left.Values.contains(id);
                    const bool nullableSide = leftSide
                        ? join.JoinKind == "Right" || join.JoinKind == "Full" || join.JoinKind == "Exclusion"
                        : join.JoinKind == "Left" || join.JoinKind == "Full" || join.JoinKind == "Exclusion";
                    const auto& value = both.at(id);
                    result.Values.emplace(id, nullableSide ? TValue{Tuple("null-extension", {occurrence, value.Signature})} : value);
                }
                return result;
            }
            case EOperator::Aggregate: {
                const auto& input = *children.at(0);
                const auto& aggregate = CastOperator<TOpAggregate>(op);
                TVector<TString> parts{input.Signature, ToStringPhase(aggregate.GetAggregationPhase()),
                    ToString(aggregate.IsDistinctAll())};
                for (const auto& key : aggregate.GetKeyColumns().Items()) {
                    parts.push_back(Tuple("group-key", {input.Values.at(key).Signature}));
                }
                TRelation result{Tuple("aggregate", parts), {}};
                for (const auto id : op.GetOutputIUs()) {
                    if (const auto* item = aggregate.GetAggregationTraits().Find(id)) {
                        const auto& argument = input.Values.at(item->Input);
                        const auto value = item->AggFunction == "count" && !item->Distinct && !item->Unwrap && argument.NonNullLiteral
                            ? TString("all-rows") : argument.Signature;
                        result.Values.emplace(id, TValue{Tuple("aggregate-value", {item->AggFunction,
                            ToString(item->Distinct), ToString(item->Unwrap), value,
                            Type(*op.NativeSchema->Arena, op.GetNativeIUType(id))})});
                    } else {
                        result.Values.emplace(id, input.Values.at(id));
                    }
                }
                return result;
            }
            case EOperator::Limit: {
                const auto& input = *children.at(0);
                const auto& limit = CastOperator<TOpLimit>(op);
                const auto offset = limit.GetOffsetCond();
                return {Tuple("limit", {input.Signature, ToStringPhase(limit.GetLimitPhase()),
                    Expression(limit.GetLimitCond(), input.Values).Signature,
                    offset ? Expression(*offset, input.Values).Signature : TString()}), input.Values};
            }
            case EOperator::Sort: {
                const auto& input = *children.at(0);
                const auto& sort = CastOperator<TOpSort>(op);
                TVector<TString> parts{input.Signature, ToStringPhase(sort.GetSortPhase()),
                    sort.LimitCond ? Expression(*sort.LimitCond, input.Values).Signature : TString()};
                AppendSortKeys(parts, sort.GetSortElements(), input.Values);
                return {Tuple("sort", parts), input.Values};
            }
            default:
                ythrow yexception() << "Unsupported operator in logical comparison: " << op.GetExplainName();
        }
    }

    size_t Scans = 0;
    size_t Ports = 0;
    size_t Joins = 0;
    std::map<IOperator*, TRelation> Relations;
};

} // namespace

TString BuildLogicalSignature(const TVector<TIntrusivePtr<TOpRoot>>& roots, NYql::TExprContext&) {
    TVector<TString> result;
    for (const auto& root : roots) {
        result.push_back(TSignature().Read(*root));
    }
    return Tuple("results", result);
}

} // namespace NKikimr::NKqp::NParsedQueryBench
