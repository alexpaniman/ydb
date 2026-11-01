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

TStringBuf OlapBinaryName(O op) {
    switch (op) {
        case O::Equal: return "eq";
        case O::NotEqual: return "neq";
        case O::Less: return "lt";
        case O::LessEqual: return "lte";
        case O::Greater: return "gt";
        case O::GreaterEqual: return "gte";
        case O::Add: return "+";
        case O::Subtract: return "-";
        case O::Multiply: return "*";
        case O::Divide: return "/";
        case O::Modulo: return "%";
        case O::Coalesce: return "??";
        case O::Contains: return "string_contains";
        case O::StartsWith: return "starts_with";
        case O::EndsWith: return "ends_with";
        default: return {};
    }
}

bool OlapIdentityConversion(const NFrontend::TExpressionArena& arena, const NFrontend::TExprNode& node) {
    if (node.Op != O::Convert) return false;
    const auto from = arena[node.Arguments[0]].Type;
    return from.Required() == node.Type.Required() || (from.Kind == K::Utf8 && node.Type.Kind == K::String);
}

bool OlapNodeSupported(const NFrontend::TExpressionArena& arena, const NFrontend::TExprNode& node) {
    if (node.Type.IsJson() || node.Type.Kind == K::Decimal || node.Type.Kind == K::Calendar || node.Type.Kind == K::Null) return false;
    if (OlapIdentityConversion(arena, node)) return true;
    if (node.Op >= O::Add && node.Op <= O::Modulo && !node.Type.IsNumeric()) return false;
    if (!OlapBinaryName(node.Op).empty()) {
        if (node.Arguments.size() != 2) return false;
        // The existing storage kernels cannot consume the same column twice.
        auto column = [&](NFrontend::TExprId id) -> std::optional<NFrontend::TColumnId> {
            while (OlapIdentityConversion(arena, arena[id])) id = arena[id].Arguments[0];
            return arena[id].Op == O::Column ? std::optional{arena[id].ColumnId} : std::nullopt;
        };
        const auto left = column(node.Arguments[0]), right = column(node.Arguments[1]);
        return !left || !right || *left != *right;
    }
    switch (node.Op) {
        case O::Literal: return !std::holds_alternative<std::monostate>(node.Value);
        case O::Column: case O::Parameter: case O::And: case O::Or: case O::Not: case O::If:
            return true;
        case O::Negate: case O::Abs: return node.Type.IsNumeric();
        case O::IsNull: case O::IsNotNull: return arena[node.Arguments[0]].Op == O::Column;
        default: return false;
    }
}

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
        for (const auto& [id, type] : Columns) fields.push_back(Ctx.MakeType<TItemExprType>(Ctx.GetIndexAsString(id), ExportNativeType(type, Ctx, &Arena)));
        Row->SetTypeAnn(Ctx.MakeType<TStructExprType>(fields));
        auto args = Ctx.NewArguments(Pos, {Row}); args->SetTypeAnn(Ctx.MakeType<TUnitExprType>());
        auto body = Lowered.at(root);
        auto lambda = Ctx.NewLambda(Pos, std::move(args), std::move(body)); lambda->SetTypeAnn(ExportNativeType(Arena[root].Type, Ctx, &Arena));
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
        auto result = Ctx.NewCallable(Pos, name, std::move(args)); result->SetTypeAnn(ExportNativeType(type, Ctx, &Arena)); return result;
    }
    TExprNode::TPtr Type(const TTypeAnnotationNode* type) {
        // ExpandType caches syntax, but leaves it untyped. Complete child type
        // expressions before marking the parent complete, so the backend's
        // constraint pass never encounters untyped descendants of typed nodes.
        if (type->GetKind() == ETypeAnnotationKind::Optional) Type(type->Cast<TOptionalExprType>()->GetItemType());
        if (type->GetKind() == ETypeAnnotationKind::Tuple)
            for (const auto* item : type->Cast<TTupleExprType>()->GetItems()) Type(item);
        if (type->GetKind() == ETypeAnnotationKind::Struct)
            for (const auto* item : type->Cast<TStructExprType>()->GetItems()) Type(item->GetItemType());
        if (type->GetKind() == ETypeAnnotationKind::Variant) Type(type->Cast<TVariantExprType>()->GetUnderlyingType());
        if (type->GetKind() == ETypeAnnotationKind::Dict) {
            Type(type->Cast<TDictExprType>()->GetKeyType());
            Type(type->Cast<TDictExprType>()->GetPayloadType());
        }
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
    TExprNode::TPtr Type(NFrontend::TType type) { return Type(ExportNativeType(type, Ctx, &Arena)); }
    TExprNode::TPtr Nothing(NFrontend::TType type) {
        return type.Kind == K::Null ? Call("Null", {}, type) : Call("Nothing", {Type(type.Optional())}, type.Optional());
    }
    TExprNode::TPtr Literal(NFrontend::TType type, const NFrontend::TScalar& value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return type.Kind == K::Null ? Call("Null", {}, type) : Nothing(type);
        }
        if (type.Kind == K::Calendar)
            return Lift(CalendarApply("DateTime2.Split", {Literal({K::Timestamp}, value)}, {{K::Timestamp}}, {K::Calendar}), type);
        if (type.Kind == K::Tuple) {
            TExprNode::TListType items;
            for (const auto& item : std::get<NFrontend::TTuple>(value)->Items) items.push_back(Literal(item.Type, item.Data));
            auto tuple = Ctx.NewList(Pos, std::move(items));
            tuple->SetTypeAnn(ExportNativeType(type.Required(), Ctx, &Arena));
            return Lift(std::move(tuple), type);
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
            argument->SetTypeAnn(ExportNativeType(types[i].Required(), Ctx, &Arena));
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
    TExprNode::TPtr Cast(TExprNode::TPtr value, NFrontend::TType source, NFrontend::TType target) {
        if (source == target) return value;
        if (source.Kind == K::Null) return Nothing(target);
        if (source.Required() == target.Required()) {
            return !source.Nullable && target.Nullable ? Call("Just", {value}, target) : value;
        }
        // Native temporal values format their numeric execution payload. Keep
        // formatting independent from YQL's ISO string conversion policy.
        if (target.IsString() && (source.Kind == K::Date || source.Kind == K::Datetime || source.Kind == K::Timestamp || source.Kind == K::Interval)) {
            NFrontend::TType payload{source.Kind == K::Interval ? K::Int64 : K::Uint64, source.Nullable};
            return Cast(Cast(value, source, payload), payload, target);
        }
        const auto* sourceType = ExportNativeType(source.Required(), Ctx, &Arena);
        const auto* requiredTarget = ExportNativeType(target.Required(), Ctx, &Arena);
        const auto options = CastResult<false>(sourceType, requiredTarget);
        if (options & NUdf::ECastOptions::Impossible) {
            throw NFrontend::TFrontendError({}, "unsupported execution cast from " + source.ToString() + " to " + target.ToString());
        }
        const bool mayFail = options & NUdf::ECastOptions::MayFail;
        // Emit optional lifting immediately. SafeCast's MiniKQL implementation
        // preserves optionality on total conversions such as integer widening.
        auto actual = target;
        if (target.Nullable && !source.Nullable && !mayFail) actual = target.Required();
        auto result = Call("SafeCast", {value, Type(actual)}, actual);
        return actual != target ? Call("Just", {result}, target) : result;
    }

    TExprNode::TPtr CalendarApply(TStringBuf name, TExprNode::TListType values,
        const std::vector<NFrontend::TType>& types, NFrontend::TType resultType) {
        return Present(std::move(values), types, resultType, [&](TExprNode::TListType args) {
            auto returned = resultType.Required();
            if (name == "DateTime2.ShiftMonths" || name == "DateTime2.ShiftYears") returned.Nullable = true;
            TVector<const TTypeAnnotationNode*> argumentTypes;
            TVector<TCallableExprType::TArgumentInfo> arguments;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const TTypeAnnotationNode* type = args[i]->GetTypeAnn();
                argumentTypes.push_back(type);
                arguments.push_back({type, {}, i == 0 ? ui64{NUdf::ICallablePayload::TArgumentFlags::AutoMap} : ui64{0}});
            }
            const auto* voidType = Ctx.MakeType<TVoidExprType>();
            const TTypeAnnotationNode* userType = voidType;
            if (name == "DateTime2.Split" || name == "DateTime2.GetYear" || name == "DateTime2.ShiftMonths" || name == "DateTime2.ShiftYears")
                userType = Ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{
                    Ctx.MakeType<TTupleExprType>(argumentTypes), Ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), voidType});
            const auto* signature = Ctx.MakeType<TCallableExprType>(ExportNativeType(returned, Ctx, &Arena), arguments, 0, TStringBuf{});
            auto voidValue = Ctx.NewCallable(Pos, "Void", {});
            voidValue->SetTypeAnn(voidType);
            auto settings = Ctx.NewList(Pos, {Ctx.NewList(Pos, {Atom("strict")})});
            AnnotateTypeSyntax(*settings);
            auto udf = Ctx.NewCallable(Pos, "Udf", {Atom(name), voidValue, Type(userType), Atom(""), Type(signature), Type(voidType), Atom(""), settings});
            udf->SetTypeAnn(signature);
            args.insert(args.begin(), std::move(udf));
            return Call("Apply", std::move(args), returned);
        });
    }
    TExprNode::TPtr StringCase(TExprNode::TPtr value, NFrontend::TType type, bool upper, bool unicode = false) {
        return Present({value}, {type}, type, [&](TExprNode::TListType args) {
            const NFrontend::TType scalar{unicode ? K::Utf8 : K::String};
            const auto* stringType = ExportNativeType(scalar, Ctx, &Arena);
            const auto* voidType = Ctx.MakeType<TVoidExprType>();
            auto voidValue = Ctx.NewCallable(Pos, "Void", {});
            voidValue->SetTypeAnn(voidType);
            const auto* signature = Ctx.MakeType<TCallableExprType>(stringType,
                TVector<TCallableExprType::TArgumentInfo>{{stringType, {}, NUdf::ICallablePayload::TArgumentFlags::AutoMap}}, 0, TStringBuf{});
            auto settings = Ctx.NewList(Pos, {Ctx.NewList(Pos, {Atom("strict")})});
            AnnotateTypeSyntax(*settings);
            const auto* name = unicode ? (upper ? "Unicode.ToUpper" : "Unicode.ToLower") :
                (upper ? "String.AsciiToUpper" : "String.AsciiToLower");
            auto udf = Ctx.NewCallable(Pos, "Udf", {Atom(name),
                voidValue, Type(voidType), Atom(""), Type(signature),
                Type(voidType), Atom(""), settings});
            udf->SetTypeAnn(signature);
            auto result = Call("Apply", {udf, Cast(args[0], type.Required(), scalar)}, scalar);
            if (type.Kind == K::Utf8 && !unicode) {
                // ASCII case changes preserve the validity of the original UTF-8.
                result = Call("Unwrap", {Cast(result, {K::String}, {K::Utf8, true})}, {K::Utf8});
            }
            return result;
        });
    }

    TExprNode::TPtr JsonValue(TExprNode::TPtr value, NFrontend::TType inputType, std::string_view path) {
        const auto typed = [&](TStringBuf name, TExprNode::TListType args, const TTypeAnnotationNode* type) {
            auto result = Ctx.NewCallable(Pos, name, std::move(args)); result->SetTypeAnn(type); return result;
        };
        const auto lambda = [&](const TExprNode::TPtr& arg, TExprNode::TPtr body) {
            auto args = Ctx.NewArguments(Pos, {arg}); args->SetTypeAnn(Ctx.MakeType<TUnitExprType>());
            const TTypeAnnotationNode* type = body->GetTypeAnn();
            auto result = Ctx.NewLambda(Pos, std::move(args), std::move(body)); result->SetTypeAnn(type); return result;
        };
        const auto apply = [&](TStringBuf name, TExprNode::TListType args, const TTypeAnnotationNode* returned,
                               bool strict = false, bool autoMap = false) {
            TVector<TCallableExprType::TArgumentInfo> arguments;
            for (size_t i = 0; i < args.size(); ++i)
                arguments.push_back({args[i]->GetTypeAnn(), {}, autoMap && i == 0 ? ui64{NUdf::ICallablePayload::TArgumentFlags::AutoMap} : ui64{0}});
            const auto* signature = Ctx.MakeType<TCallableExprType>(returned, arguments, 0, TStringBuf{});
            const auto* voidType = Ctx.MakeType<TVoidExprType>();
            auto settings = Ctx.NewList(Pos, strict ? TExprNode::TListType{Ctx.NewList(Pos, {Atom("strict")})} : TExprNode::TListType{});
            AnnotateTypeSyntax(*settings);
            auto udf = typed("Udf", {Atom(name), typed("Void", {}, voidType), Type(voidType), Atom(""),
                Type(signature), Type(voidType), Atom(""), settings}, signature);
            args.insert(args.begin(), std::move(udf));
            return typed("Apply", std::move(args), returned);
        };
        const auto* jsonNode = Ctx.MakeType<TResourceExprType>("JsonNode");
        const auto* optionalNode = Ctx.MakeType<TOptionalExprType>(jsonNode);
        if (inputType.Kind == K::Json) {
            if (inputType.Nullable) {
                auto arg = Ctx.NewArgument(Pos, "native_json"); arg->SetTypeAnn(ExportNativeType(inputType.Required(), Ctx));
                auto parsed = typed("Just", {apply("Json2.Parse", {arg}, jsonNode, false, true)}, optionalNode);
                value = typed("IfPresent", {value, lambda(arg, parsed), typed("Nothing", {Type(optionalNode)}, optionalNode)}, optionalNode);
            } else value = typed("Just", {apply("Json2.Parse", {value}, jsonNode, false, true)}, optionalNode);
        } else value = Lift(value, inputType.Optional());
        const auto* pathType = Ctx.MakeType<TResourceExprType>("JsonPath");
        auto compiledPath = apply("Json2.CompilePath", {Literal({K::Utf8}, std::string(path))}, pathType);
        const auto* variablesType = Ctx.MakeType<TDictExprType>(ExportNativeType({K::Utf8}, Ctx), jsonNode);
        auto variables = typed("Dict", {Type(variablesType)}, variablesType);
        const auto* resultType = ExportNativeType({K::Utf8, true}, Ctx);
        const auto* errorType = Ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{
            ExportNativeType({K::Uint8}, Ctx), ExportNativeType({K::String}, Ctx)});
        const auto* variant = Ctx.MakeType<TVariantExprType>(Ctx.MakeType<TTupleExprType>(
            TVector<const TTypeAnnotationNode*>{errorType, resultType}));
        auto extracted = apply(inputType.Kind == K::Json ? "Json2.SqlValueConvertToUtf8" : "Json2.JsonDocumentSqlValueConvertToUtf8",
            {value, compiledPath, variables}, variant, true);
        auto error = Ctx.NewArgument(Pos, "json_error"); error->SetTypeAnn(errorType);
        auto result = Ctx.NewArgument(Pos, "json_value"); result->SetTypeAnn(resultType);
        // Both EMPTY and ERROR select SQL NULL. The runtime compiles the path
        // and extracts/converts the value; no YQL semantic expansion is needed.
        return typed("Visit", {extracted, Atom("0"), lambda(error, Nothing({K::Utf8, true})), Atom("1"), lambda(result, result)}, resultType);
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
            case O::JsonValue: return JsonValue(args[0], types[0], node.Name);
            case O::Literal: return Literal(type, node.Value);
            case O::Column: {
                const auto [it, inserted] = Columns.emplace(node.ColumnId, type);
                if (!inserted && it->second != type) throw NFrontend::TFrontendError(node.Position, "inconsistent native column type");
                return Call("Member", {Row, Atom(Ctx.GetIndexAsString(node.ColumnId))}, type);
            }
            case O::Parameter: return Call("Parameter", {Atom("$" + String(node.Name)), Type(type)}, type);
            case O::Ensure:
                args.push_back(Literal({K::String}, node.Name));
                return Call("Ensure", std::move(args), type);
            case O::StablePickle: return Call("StablePickle", std::move(args), type);
            case O::Unwrap: return Call("Unwrap", std::move(args), type);
            case O::Convert: {
                const auto from = Arena[node.Arguments[0]].Type;
                if (from.IsNumeric() && type.IsNumeric() && from.Kind != K::Decimal && type.Kind != K::Decimal && from.Kind != type.Kind) {
                    auto actual = type; actual.Nullable = from.Nullable;
                    // Terminal MiniKQL conversion: implicit integer promotion
                    // wraps and preserves NULL; explicit CAST remains checked.
                    return Lift(Call("Convert", {args[0], Type(type.Required())}, actual), type);
                }
                return Cast(args[0], from, type);
            }
            case O::Cast: return Cast(args[0], Arena[node.Arguments[0]].Type, type);
            case O::Negate: return Call("Minus", std::move(args), type);
            case O::Not: return Call("Not", std::move(args), type);
            case O::Add: case O::Subtract: case O::Multiply: case O::Divide: case O::Modulo: {
                if (type.Kind == K::Decimal && (node.Op == O::Multiply || node.Op == O::Divide || node.Op == O::Modulo))
                    return Call(node.Op == O::Multiply ? "DecimalMul" : node.Op == O::Divide ? "DecimalDiv" : "DecimalMod", std::move(args), type);
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
                for (std::size_t i = 1; i < args.size(); ++i) result = Call("Coalesce", {result, Lift(args[i], actual)}, actual);
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
            case O::UnicodeLower: case O::UnicodeUpper: return StringCase(args[0], type, node.Op == O::UnicodeUpper, true);
            case O::Substring: return Call("Substring", std::move(args), type);
            case O::Find: return Call("Find", std::move(args), type);
            case O::CalendarSplit: return CalendarApply("DateTime2.Split", std::move(args), types, type);
            case O::CalendarDate: return CalendarApply("DateTime2.MakeDate", std::move(args), types, type);
            case O::CalendarDatetime: return CalendarApply("DateTime2.MakeDatetime", std::move(args), types, type);
            case O::CalendarTimestamp: return CalendarApply("DateTime2.MakeTimestamp", std::move(args), types, type);
            case O::CalendarYear: return CalendarApply("DateTime2.GetYear", std::move(args), types, type);
            case O::CalendarShiftMonths: return CalendarApply("DateTime2.ShiftMonths", std::move(args), types, type);
            case O::CalendarShiftYears: return CalendarApply("DateTime2.ShiftYears", std::move(args), types, type);
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

const NYql::TTypeAnnotationNode* ExportNativeType(NFrontend::TType type, NYql::TExprContext& ctx,
    const NFrontend::TExpressionArena* arena) {
    const TTypeAnnotationNode* result;
    if (type.Kind == K::Null) return ctx.MakeType<TNullExprType>();
    if (type.Kind == K::Tuple) {
        Y_ENSURE(arena, "Tuple export requires its native arena");
        TVector<const TTypeAnnotationNode*> items;
        for (const auto item : arena->TupleItems(type)) items.push_back(ExportNativeType(item, ctx));
        result = ctx.MakeType<TTupleExprType>(items);
    } else if (type.Kind == K::Decimal)
        result = ctx.MakeType<TDataExprParamsType>(EDataSlot::Decimal, ToString(type.Precision), ToString(type.Scale));
    else if (type.Kind == K::Calendar) result = ctx.MakeType<TResourceExprType>("DateTime2.TM");
    else {
        const auto slot = NUdf::FindDataSlot(String(type.Required().ToString()));
        Y_ENSURE(slot, "Unknown native type"); result = ctx.MakeType<TDataExprType>(*slot);
    }
    return type.Nullable ? ctx.MakeType<TOptionalExprType>(result) : result;
}

NFrontend::TType ImportNativeType(const NYql::TTypeAnnotationNode& type, NFrontend::TExpressionArena* arena) {
    if (type.GetKind() == ETypeAnnotationKind::Optional) {
        const auto* item = type.Cast<TOptionalExprType>()->GetItemType();
        if (item->GetKind() == ETypeAnnotationKind::Optional)
            throw NFrontend::TFrontendError({}, "nested optional types are not native types");
        return ImportNativeType(*item, arena).Optional();
    }
    if (type.GetKind() == ETypeAnnotationKind::Tuple) {
        Y_ENSURE(arena, "Tuple import requires a native arena");
        std::vector<NFrontend::TType> items;
        for (const auto* item : type.Cast<TTupleExprType>()->GetItems()) {
            if (RemoveOptionality(*item).GetKind() == ETypeAnnotationKind::Tuple)
                throw NFrontend::TFrontendError({}, "nested tuple states are not supported");
            items.push_back(ImportNativeType(*item, arena));
        }
        return arena->TupleType(items);
    }
    if (type.GetKind() == ETypeAnnotationKind::Null) return {};
    if (type.GetKind() == ETypeAnnotationKind::Resource && type.Cast<TResourceExprType>()->GetTag() == "DateTime2.TM")
        return {K::Calendar};
    if (type.GetKind() != ETypeAnnotationKind::Data) throw NFrontend::TFrontendError({}, "unsupported native type");
    const auto* data = type.Cast<TDataExprType>();
    auto text = std::string(data->GetName());
    if (data->GetSlot() == EDataSlot::Decimal) {
        const auto* decimal = static_cast<const TDataExprParamsType*>(data);
        text += "(" + std::string(decimal->GetParamOne()) + "," + std::string(decimal->GetParamTwo()) + ")";
    }
    return NFrontend::ParseType(text);
}

NYql::TExprNode::TPtr LowerNativeLambda(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    return TLowerer(arena, ctx, pos).Run(root);
}

bool CanLowerNativeOlapPredicate(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root) {
    if (arena[root].Type.Kind != K::Bool) return false;
    std::unordered_map<NFrontend::TExprId, bool> hasColumn;
    std::vector<std::pair<NFrontend::TExprId, bool>> pending{{root, false}};
    while (!pending.empty()) {
        const auto [id, ready] = pending.back(); pending.pop_back();
        if (hasColumn.contains(id)) continue;
        const auto& node = arena[id];
        if (!OlapNodeSupported(arena, node)) return false;
        if (!ready) {
            pending.emplace_back(id, true);
            for (auto arg : node.Arguments) pending.emplace_back(arg, false);
            continue;
        }
        bool column = node.Op == O::Column;
        for (auto arg : node.Arguments) {
            // Storage streaming logic needs a row count before combining
            // branches. Keep scalar-only Boolean branches in compute.
            if ((node.Op == O::And || node.Op == O::Or) && !hasColumn.at(arg)) return false;
            column |= hasColumn.at(arg);
        }
        hasColumn.emplace(id, column);
    }
    return hasColumn.at(root);
}

NYql::TExprNode::TPtr LowerNativeOlapPredicate(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    Y_ENSURE(CanLowerNativeOlapPredicate(arena, root), "Unsupported native OLAP predicate");
    std::unordered_map<NFrontend::TExprId, TExprNode::TPtr> lowered;
    std::vector<std::pair<NFrontend::TExprId, bool>> pending{{root, false}};
    while (!pending.empty()) {
        const auto [id, ready] = pending.back(); pending.pop_back();
        if (lowered.contains(id)) continue;
        const auto& node = arena[id];
        if (!ready) {
            pending.emplace_back(id, true);
            for (auto arg : node.Arguments) pending.emplace_back(arg, false);
            continue;
        }
        TExprNode::TListType args;
        for (auto arg : node.Arguments) args.push_back(lowered.at(arg));
        TExprNode::TPtr value;
        if (OlapIdentityConversion(arena, node)) value = args[0];
        else if (node.Op == O::Column) value = ctx.NewAtom(pos, ctx.GetIndexAsString(node.ColumnId));
        else if (node.Op == O::Literal || node.Op == O::Parameter) {
            value = LowerNativeLambda(arena, id, ctx, pos)->TailPtr();
            if (node.Op == O::Literal && node.Type.Nullable)
                value = ctx.NewList(pos, {ctx.NewAtom(pos, "just"), value->HeadPtr()});
        }
        else if (const auto name = OlapBinaryName(node.Op); !name.empty()) {
            args.insert(args.begin(), ctx.NewAtom(pos, name));
            args.push_back(ExpandType(pos, *ExportNativeType(node.Type, ctx, &arena), ctx));
            value = ctx.NewList(pos, std::move(args));
        } else if (node.Op == O::If) {
            if (arena[node.Arguments[0]].Type.Nullable) {
                const auto boolean = ExportNativeType({K::Bool}, ctx);
                auto literal = ctx.NewAtom(pos, "false");
                literal->SetTypeAnn(ctx.MakeType<TUnitExprType>());
                auto otherwise = ctx.NewCallable(pos, "Bool", {literal});
                otherwise->SetTypeAnn(boolean);
                args[0] = ctx.NewList(pos, {
                    ctx.NewAtom(pos, "??"), args[0], otherwise, ExpandType(pos, *boolean, ctx)});
            }
            args.insert(args.begin(), ctx.NewAtom(pos, "if"));
            args.push_back(ExpandType(pos, *ExportNativeType(node.Type, ctx, &arena), ctx));
            value = ctx.NewList(pos, std::move(args));
        } else if (node.Op == O::And || node.Op == O::Or || node.Op == O::Not) {
            value = ctx.NewCallable(pos, node.Op == O::And ? "KqpOlapAnd" : node.Op == O::Or ? "KqpOlapOr" : "KqpOlapNot", std::move(args));
        } else {
            args.insert(args.begin(), ctx.NewAtom(pos, node.Op == O::IsNull ? "empty" : node.Op == O::IsNotNull ? "exists" :
                node.Op == O::Negate ? "minus" : "abs"));
            value = ctx.NewList(pos, std::move(args));
        }
        lowered.emplace(id, std::move(value));
    }
    auto input = ctx.NewArgument(pos, "native_olap_rows");
    // The storage filter entry point accepts operations, including a one-item
    // conjunction, but not a bare Boolean column atom.
    auto condition = ctx.NewCallable(pos, "KqpOlapAnd", {lowered.at(root)});
    auto filter = ctx.NewCallable(pos, "KqpOlapFilter", {input, std::move(condition)});
    return ctx.NewLambda(pos, ctx.NewArguments(pos, {input}), std::move(filter));
}

} // namespace NKikimr::NKqp
