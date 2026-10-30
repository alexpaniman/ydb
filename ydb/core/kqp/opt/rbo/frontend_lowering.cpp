#include "frontend_lowering.h"

#include <yql/essentials/core/yql_expr_type_annotation.h>

#include <map>
#include <limits>
#include <unordered_map>

namespace NKikimr::NKqp {
namespace {
using namespace NYql;
using K = NFrontend::ETypeKind;
using O = NFrontend::EOpcode;

TString String(std::string_view text) { return TString(text.data(), text.size()); }

class TLowerer {
public:
    TLowerer(const NFrontend::TExpressionArena& arena, TExprContext& ctx, TPositionHandle pos)
        : Arena(arena), Ctx(ctx), Pos(pos), Row(ctx.NewArgument(pos, "native_row")) {}

    TExprNode::TPtr Run(NFrontend::TExprId root) {
        std::vector<std::pair<NFrontend::TExprId, bool>> pending{{root, false}};
        while (!pending.empty()) {
            const auto [id, ready] = pending.back(); pending.pop_back();
            if (Lowered.contains(id)) continue;
            const auto& node = Arena[id];
            if (!ready) {
                pending.emplace_back(id, true);
                for (auto it = node.Arguments.rbegin(); it != node.Arguments.rend(); ++it) if (!Lowered.contains(*it)) pending.emplace_back(*it, false);
            } else Lowered.emplace(id, Lower(node));
        }
        TVector<const TItemExprType*> fields;
        for (const auto& [id, type] : Columns) fields.push_back(Ctx.MakeType<TItemExprType>(Ctx.GetIndexAsString(id), ExportNativeType(type, Ctx)));
        Row->SetTypeAnn(Ctx.MakeType<TStructExprType>(fields));
        auto args = Ctx.NewArguments(Pos, {Row}); args->SetTypeAnn(Ctx.MakeType<TUnitExprType>());
        auto body = Lowered.at(root);
        auto lambda = Ctx.NewLambda(Pos, std::move(args), std::move(body)); lambda->SetTypeAnn(ExportNativeType(Arena[root].Type, Ctx));
        return lambda;
    }

private:
    TExprNode::TPtr Atom(TStringBuf text) {
        auto result = Ctx.NewAtom(Pos, text);
        result->SetTypeAnn(Ctx.MakeType<TUnitExprType>());
        return result;
    }
    void AnnotateTypeSyntax(TExprNode& node) {
        for (const auto& child : node.Children()) AnnotateTypeSyntax(*child);
        if (node.GetTypeAnn()) return;
        if (node.IsAtom()) {
            node.SetTypeAnn(Ctx.MakeType<TUnitExprType>());
        } else {
            YQL_ENSURE(node.IsList(), "Unannotated native execution type");
            TVector<const TTypeAnnotationNode*> items;
            bool unit = true;
            for (const auto& child : node.Children()) {
                items.push_back(child->GetTypeAnn());
                unit &= child->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Unit;
            }
            node.SetTypeAnn(unit ? static_cast<const TTypeAnnotationNode*>(Ctx.MakeType<TUnitExprType>()) : Ctx.MakeType<TTupleExprType>(items));
        }
    }
    TExprNode::TPtr Call(TStringBuf name, TExprNode::TListType args, NFrontend::TType type) {
        auto result = Ctx.NewCallable(Pos, name, std::move(args)); result->SetTypeAnn(ExportNativeType(type, Ctx)); return result;
    }
    TExprNode::TPtr Type(const TTypeAnnotationNode* type) {
        // ExpandType caches syntax, but leaves it untyped. Complete child type
        // expressions before marking the parent complete, so the backend's
        // constraint pass never encounters untyped descendants of typed nodes.
        if (type->GetKind() == ETypeAnnotationKind::Optional) Type(type->Cast<TOptionalExprType>()->GetItemType());
        if (type->GetKind() == ETypeAnnotationKind::Callable) {
            const auto* callable = type->Cast<TCallableExprType>();
            Type(callable->GetReturnType());
            for (const auto& argument : callable->GetArguments()) Type(argument.Type);
        }
        auto result = ExpandType(Pos, *type, Ctx);
        result->SetTypeAnn(Ctx.MakeType<TTypeExprType>(type));
        AnnotateTypeSyntax(*result);
        return result;
    }
    TExprNode::TPtr Type(NFrontend::TType type) { return Type(ExportNativeType(type, Ctx)); }
    TExprNode::TPtr Nothing(NFrontend::TType type) {
        return type.Kind == K::Null ? Call("Null", {}, type) : Call("Nothing", {Type(type.Optional())}, type.Optional());
    }
    TExprNode::TPtr Literal(NFrontend::TType type, const NFrontend::TScalar& value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return type.Kind == K::Null ? Call("Null", {}, type) : Nothing(type);
        }
        const auto text = NFrontend::FormatValue({type, value});
        TExprNode::TListType args{Atom(String(text))};
        if (type.Kind == K::Decimal) {
            args.push_back(Atom(ToString(type.Precision)));
            args.push_back(Atom(ToString(type.Scale)));
        }
        const auto name = type.Kind == K::Decimal ? std::string("Decimal") : type.Required().ToString();
        auto result = Call(String(name), std::move(args), type.Required());
        return type.Nullable ? Call("Just", {result}, type) : result;
    }
    TExprNode::TPtr Lift(TExprNode::TPtr value, NFrontend::TType target) {
        return target.Nullable && value->GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional ?
            Call("Just", {value}, target) : value;
    }
    TExprNode::TPtr Present(TExprNode::TListType values, const std::vector<NFrontend::TType>& types,
        NFrontend::TType resultType, const std::function<TExprNode::TPtr(TExprNode::TListType)>& build) {
        TExprNode::TListType optionals, arguments;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!types[i].Nullable) continue;
            optionals.push_back(values[i]);
            auto argument = Ctx.NewArgument(Pos, "present");
            argument->SetTypeAnn(ExportNativeType(types[i].Required(), Ctx));
            values[i] = argument;
            arguments.push_back(argument);
        }
        auto body = Lift(build(std::move(values)), resultType);
        if (optionals.empty()) return body;
        auto args = Ctx.NewArguments(Pos, std::move(arguments));
        args->SetTypeAnn(Ctx.MakeType<TUnitExprType>());
        const TTypeAnnotationNode* bodyType = body->GetTypeAnn();
        auto lambda = Ctx.NewLambda(Pos, std::move(args), std::move(body));
        lambda->SetTypeAnn(bodyType);
        optionals.push_back(lambda);
        optionals.push_back(Nothing(resultType));
        return Call("IfPresent", std::move(optionals), resultType);
    }
    TExprNode::TPtr DecimalFinite(TExprNode::TPtr value, NFrontend::TType type) {
        __int128 maximum = 1;
        for (unsigned i = 0; i < type.Precision; ++i) maximum *= 10;
        --maximum;
        auto below = Call("<=", {value, Literal(type.Required(), NFrontend::TDecimal{maximum})}, {K::Bool, type.Nullable});
        auto above = Call(">=", {value, Literal(type.Required(), NFrontend::TDecimal{-maximum})}, {K::Bool, type.Nullable});
        return Call("And", {below, above}, {K::Bool, type.Nullable});
    }
    TExprNode::TPtr Cast(TExprNode::TPtr value, NFrontend::TType source, NFrontend::TType target) {
        if (source == target) return value;
        if (source.Kind == K::Null) return Nothing(target);
        if (source.Required() == target.Required()) {
            return !source.Nullable && target.Nullable ? Call("Just", {value}, target) : value;
        }
        if (source.Kind == K::Decimal && source.Scale && target.IsIntegral()) {
            // Round once with room for a carry before narrowing. The generic
            // backend integer cast otherwise rounds through Decimal(p-s,0),
            // which can overflow even when the requested integer fits.
            NFrontend::TType rounded{K::Decimal, source.Nullable, 35, 0};
            auto integral = Call("SafeCast", {value, Type(rounded)}, rounded);
            return Cast(integral, rounded, target);
        }
        // Native temporal values format their numeric execution payload. Keep
        // formatting independent from YQL's ISO string conversion policy.
        if (target.IsString() && (source.Kind == K::Date || source.Kind == K::Datetime || source.Kind == K::Timestamp || source.Kind == K::Interval)) {
            NFrontend::TType payload{source.Kind == K::Interval ? K::Int64 : K::Uint64, source.Nullable};
            return Cast(Cast(value, source, payload), payload, target);
        }
        const auto* sourceType = ExportNativeType(source.Required(), Ctx);
        const auto* requiredTarget = ExportNativeType(target.Required(), Ctx);
        const auto options = CastResult<false>(sourceType, requiredTarget);
        if (options & NUdf::ECastOptions::Impossible) {
            throw NFrontend::TFrontendError({}, "unsupported execution cast from " + source.ToString() + " to " + target.ToString());
        }
        const bool mayFail = options & NUdf::ECastOptions::MayFail;
        if (target.Kind == K::Decimal) {
            auto actual = target.Required();
            actual.Nullable = source.Nullable || source.IsString();
            auto converted = Call("SafeCast", {value, Type(actual)}, actual);
            if (!target.Nullable) return converted;
            return Present({converted}, {actual}, target, [&](TExprNode::TListType args) {
                auto finite = DecimalFinite(args[0], target.Required());
                return Call("If", {finite, Call("Just", {args[0]}, target), Nothing(target)}, target);
            });
        }
        // Emit optional lifting immediately. SafeCast's MiniKQL implementation
        // preserves optionality on total conversions such as integer widening.
        auto actual = target;
        if (target.Nullable && !source.Nullable && !mayFail) actual = target.Required();
        auto result = Call("SafeCast", {value, Type(actual)}, actual);
        return actual != target ? Call("Just", {result}, target) : result;
    }

    TExprNode::TPtr StringCase(TExprNode::TPtr value, NFrontend::TType type, bool upper) {
        return Present({value}, {type}, type, [&](TExprNode::TListType args) {
            const auto* stringType = ExportNativeType({K::String}, Ctx);
            const auto* voidType = Ctx.MakeType<TVoidExprType>();
            auto voidValue = Ctx.NewCallable(Pos, "Void", {});
            voidValue->SetTypeAnn(voidType);
            const auto* signature = Ctx.MakeType<TCallableExprType>(stringType,
                TVector<TCallableExprType::TArgumentInfo>{{stringType, {}, NUdf::ICallablePayload::TArgumentFlags::AutoMap}}, 0, TStringBuf{});
            auto settings = Ctx.NewList(Pos, {Ctx.NewList(Pos, {Atom("strict")})});
            AnnotateTypeSyntax(*settings);
            auto udf = Ctx.NewCallable(Pos, "Udf", {Atom(upper ? "String.AsciiToUpper" : "String.AsciiToLower"),
                voidValue, Type(voidType), Atom(""), Type(signature),
                Type(voidType), Atom(""), settings});
            udf->SetTypeAnn(signature);
            auto result = Call("Apply", {udf, Cast(args[0], type.Required(), {K::String})}, {K::String});
            if (type.Kind == K::Utf8) {
                // ASCII case changes preserve the validity of the original UTF-8.
                result = Call("Unwrap", {Cast(result, {K::String}, {K::Utf8, true})}, {K::Utf8});
            }
            return result;
        });
    }

    TExprNode::TPtr DecimalArithmetic(const NFrontend::TExprNode& node, TExprNode::TListType args) {
        const auto output = node.Type;
        std::vector<NFrontend::TType> types;
        for (auto id : node.Arguments) types.push_back(Arena[id].Type);
        return Present(std::move(args), types, output, [&](TExprNode::TListType values) {
            const char* name = node.Op == O::Add ? "+" : node.Op == O::Subtract ? "-" :
                node.Op == O::Multiply ? "DecimalMul" : node.Op == O::Divide ? "DecimalDiv" : "DecimalMod";
            auto result = Call(name, values, output.Required());
            auto checked = Call("Ensure", {result, DecimalFinite(result, output.Required()),
                Literal({K::String}, std::string{"Decimal arithmetic overflow"})}, output.Required());
            if (node.Op == O::Divide || node.Op == O::Modulo) {
                auto zero = Literal(types[1].Required(), NFrontend::TDecimal{0});
                return Call("If", {Call("==", {values[1], zero}, {K::Bool}), Nothing(output),
                    Call("Just", {checked}, output)}, output);
            }
            return checked;
        });
    }

    TExprNode::TPtr Lower(const NFrontend::TExprNode& node) {
        TExprNode::TListType args;
        std::vector<NFrontend::TType> types;
        for (auto id : node.Arguments) {
            args.push_back(Lowered.at(id));
            types.push_back(Arena[id].Type);
        }
        auto type = node.Type;
        if (node.Op >= O::Equal && node.Op <= O::GreaterEqual &&
            (types[0].Kind == K::Null || types[1].Kind == K::Null)) return Nothing(type);
        switch (node.Op) {
            case O::Literal: return Literal(type, node.Value);
            case O::Column: {
                const auto [it, inserted] = Columns.emplace(node.ColumnId, type);
                if (!inserted && it->second != type) throw NFrontend::TFrontendError(node.Position, "inconsistent native column type");
                return Call("Member", {Row, Atom(Ctx.GetIndexAsString(node.ColumnId))}, type);
            }
            case O::Parameter: return Call("Parameter", {Atom("$" + String(node.Name)), Type(type)}, type);
            case O::Convert: case O::Cast: return Cast(args[0], Arena[node.Arguments[0]].Type, type);
            case O::Negate: return Call("Minus", std::move(args), type);
            case O::Not: return Call("Not", std::move(args), type);
            case O::Add: case O::Subtract: case O::Multiply: case O::Divide: case O::Modulo: {
                if (type.Kind == K::Decimal) return DecimalArithmetic(node, std::move(args));
                const char* name = node.Op == O::Add ? "+" : node.Op == O::Subtract ? "-" : node.Op == O::Multiply ? "*" : node.Op == O::Divide ? "/" : "%";
                return Call(name, std::move(args), type);
            }
            case O::Equal: return Call("==", std::move(args), type);
            case O::NotEqual: return Call("!=", std::move(args), type);
            case O::Less: return Call("<", std::move(args), type);
            case O::LessEqual: return Call("<=", std::move(args), type);
            case O::Greater: return Call(">", std::move(args), type);
            case O::GreaterEqual: return Call(">=", std::move(args), type);
            case O::And: return Call("And", std::move(args), type);
            case O::Or: return Call("Or", std::move(args), type);
            case O::IsNull: case O::IsNotNull: {
                const auto inputType = Arena[node.Arguments[0]].Type;
                if (inputType.Kind == K::Null) return Literal({K::Bool}, node.Op == O::IsNull);
                if (!inputType.Nullable) return Literal({K::Bool}, node.Op == O::IsNotNull);
                auto exists = Call("Exists", std::move(args), {K::Bool});
                return node.Op == O::IsNull ? Call("Not", {exists}, {K::Bool}) : exists;
            }
            case O::If: {
                if (Arena[node.Arguments[0]].Type.Nullable) args[0] = Call("Coalesce", {args[0], Literal({K::Bool}, false)}, {K::Bool});
                return Call("If", std::move(args), type);
            }
            case O::Coalesce: {
                if (type.Kind == K::Null) return Nothing(type);
                if (!types[0].Nullable) return Lift(args[0], type);
                auto actual = type.Optional();
                auto result = args[0];
                for (std::size_t i = 1; i < args.size(); ++i) result = Call("Coalesce", {result, args[i]}, actual);
                return type.Nullable ? result : Call("Unwrap", {result}, type);
            }
            case O::Abs: return Call("Abs", std::move(args), type);
            case O::Length: {
                auto size = Call("Size", std::move(args), {K::Uint32, type.Nullable});
                return Cast(size, {K::Uint32, type.Nullable}, type);
            }
            case O::Contains: return Call("StringContains", std::move(args), type);
            case O::StartsWith: return Call("StartsWith", std::move(args), type);
            case O::EndsWith: return Call("EndsWith", std::move(args), type);
            case O::Concat: return Call("Concat", std::move(args), type);
            case O::Lower: case O::Upper: return StringCase(args[0], type, node.Op == O::Upper);
            case O::Substring: return Call("Substring", std::move(args), type);
        }
        throw NFrontend::TFrontendError(node.Position, "unknown native opcode");
    }

    const NFrontend::TExpressionArena& Arena;
    TExprContext& Ctx;
    TPositionHandle Pos;
    TExprNode::TPtr Row;
    std::unordered_map<NFrontend::TExprId, TExprNode::TPtr> Lowered;
    std::map<NFrontend::TColumnId, NFrontend::TType> Columns;
};
} // namespace

const NYql::TTypeAnnotationNode* ExportNativeType(NFrontend::TType type, NYql::TExprContext& ctx) {
    const NYql::TTypeAnnotationNode* result;
    if (type.Kind == K::Null) return ctx.MakeType<NYql::TNullExprType>();
    if (type.Kind == K::Decimal) result = ctx.MakeType<NYql::TDataExprParamsType>(NYql::EDataSlot::Decimal, ToString(type.Precision), ToString(type.Scale));
    else {
        const auto slot = NUdf::FindDataSlot(String(type.Required().ToString()));
        Y_ENSURE(slot, "Unknown native type"); result = ctx.MakeType<NYql::TDataExprType>(*slot);
    }
    return type.Nullable ? ctx.MakeType<NYql::TOptionalExprType>(result) : result;
}

NFrontend::TType ImportNativeType(const NYql::TTypeAnnotationNode& type) {
    if (type.GetKind() == NYql::ETypeAnnotationKind::Optional) {
        const auto* item = type.Cast<NYql::TOptionalExprType>()->GetItemType();
        if (item->GetKind() == NYql::ETypeAnnotationKind::Optional) throw NFrontend::TFrontendError({}, "nested optional types are not native scalar types");
        return ImportNativeType(*item).Optional();
    }
    if (type.GetKind() == NYql::ETypeAnnotationKind::Null) return {};
    if (type.GetKind() != NYql::ETypeAnnotationKind::Data) throw NFrontend::TFrontendError({}, "unsupported native scalar type");
    const auto* data = type.Cast<NYql::TDataExprType>();
    auto text = std::string(data->GetName());
    if (data->GetSlot() == NYql::EDataSlot::Decimal) {
        const auto* decimal = static_cast<const NYql::TDataExprParamsType*>(data);
        text += "(" + std::string(decimal->GetParamOne()) + "," + std::string(decimal->GetParamTwo()) + ")";
    }
    return NFrontend::ParseType(text);
}

NYql::TExprNode::TPtr LowerNativeLambda(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    return TLowerer(arena, ctx, pos).Run(root);
}

} // namespace NKikimr::NKqp
