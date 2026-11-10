#include "frontend_lowering.h"
#include "kqp_operator.h"

#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_expr_optimize.h>

#include <map>
#include <limits>
#include <unordered_map>

namespace NKikimr::NKqp {
const NYql::TTypeAnnotationNode* ExportIUType(const IOperator& op, unsigned int id, NYql::TExprContext& ctx) {
    Y_ENSURE(op.NativeSchema, "Missing operator schema");
    return ExportNativeType(op.GetNativeIUType(id), ctx, op.NativeSchema->Arena.get());
}

const NYql::TTypeAnnotationNode* ExportOperatorType(const IOperator& op, NYql::TExprContext& ctx) {
    Y_ENSURE(op.NativeSchema, "Missing operator schema");
    using namespace NYql;
    const auto& schema = *op.NativeSchema;
    auto row = [&](const TNativeRow& fields) {
        TVector<const TItemExprType*> items;
        for (const auto& [id, type] : fields)
            items.push_back(ctx.MakeType<TItemExprType>(ctx.GetIndexAsString(id), ExportNativeType(type, ctx, schema.Arena.get())));
        return ctx.MakeType<TStructExprType>(items);
    };
    const TTypeAnnotationNode* item = row(schema.Row);
    if (schema.LookupLeft) {
        const TVector<const TTypeAnnotationNode*> fields{row(*schema.LookupLeft), ctx.MakeType<TOptionalExprType>(item),
            ctx.MakeType<TDataExprType>(EDataSlot::Uint64)};
        item = ctx.MakeType<TTupleExprType>(fields);
    }
    return ctx.MakeType<TListExprType>(item);
}

NYql::TExprNode::TPtr LowerTable(const TRboTable& table, NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    return NYql::NNodes::Build<NYql::NNodes::TKqpTable>(ctx, pos)
        .Path().Build(table.Path)
        .PathId().Build(table.PathId)
        .SysView().Build(table.SysView)
        .Version().Build(table.Version)
        .Done().Ptr();
}

namespace {
using namespace NYql;
using K = NFrontend::ETypeKind;
using O = NFrontend::EOpcode;

TString String(std::string_view text) { return TString(text.data(), text.size()); }

void AnnotateTypeSyntax(TExprNode& node, TExprContext& ctx) {
    for (const auto& child : node.Children()) AnnotateTypeSyntax(*child, ctx);
    if (node.GetTypeAnn()) return;
    if (node.IsAtom()) {
        node.SetTypeAnn(ctx.MakeType<TUnitExprType>());
    } else {
        YQL_ENSURE(node.IsList(), "Unannotated native execution type");
        TVector<const TTypeAnnotationNode*> items;
        bool unit = true;
        for (const auto& child : node.Children()) {
            items.push_back(child->GetTypeAnn());
            unit &= child->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Unit;
        }
        node.SetTypeAnn(unit ? static_cast<const TTypeAnnotationNode*>(ctx.MakeType<TUnitExprType>()) : ctx.MakeType<TTupleExprType>(items));
    }
}

TExprNode::TPtr ExecutionType(const TTypeAnnotationNode* type, TExprContext& ctx, TPositionHandle pos) {
    const auto child = [&](const TTypeAnnotationNode* value) { return ExecutionType(value, ctx, pos); };
    // ExpandType caches syntax, but leaves it untyped. Complete child type
    // expressions before marking the parent complete, so the backend's
    // constraint pass never encounters untyped descendants of typed nodes.
    if (type->GetKind() == ETypeAnnotationKind::Optional) child(type->Cast<TOptionalExprType>()->GetItemType());
    if (type->GetKind() == ETypeAnnotationKind::List) child(type->Cast<TListExprType>()->GetItemType());
    if (type->GetKind() == ETypeAnnotationKind::Tuple)
        for (const auto* item : type->Cast<TTupleExprType>()->GetItems()) child(item);
    if (type->GetKind() == ETypeAnnotationKind::Struct)
        for (const auto* item : type->Cast<TStructExprType>()->GetItems()) child(item->GetItemType());
    if (type->GetKind() == ETypeAnnotationKind::Variant) child(type->Cast<TVariantExprType>()->GetUnderlyingType());
    if (type->GetKind() == ETypeAnnotationKind::Dict) {
        child(type->Cast<TDictExprType>()->GetKeyType());
        child(type->Cast<TDictExprType>()->GetPayloadType());
    }
    if (type->GetKind() == ETypeAnnotationKind::Callable) {
        const auto* callable = type->Cast<TCallableExprType>();
        child(callable->GetReturnType());
        for (const auto& argument : callable->GetArguments()) child(argument.Type);
    }
    auto result = ExpandType(pos, *type, ctx);
    result->SetTypeAnn(ctx.MakeType<TTypeExprType>(type));
    AnnotateTypeSyntax(*result, ctx);
    return result;
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
    TExprNode::TPtr Call(TStringBuf name, TExprNode::TListType args, NFrontend::TType type) {
        auto result = Ctx.NewCallable(Pos, name, std::move(args)); result->SetTypeAnn(ExportNativeType(type, Ctx, &Arena)); return result;
    }
    TExprNode::TPtr Type(const TTypeAnnotationNode* type) { return ExecutionType(type, Ctx, Pos); }
    TExprNode::TPtr Type(NFrontend::TType type) { return Type(ExportNativeType(type, Ctx, &Arena)); }
    TExprNode::TPtr Nothing(NFrontend::TType type) {
        return type.Kind == K::Null ? Call("Null", {}, type) : Call("Nothing", {Type(type.Optional())}, type.Optional());
    }
    TExprNode::TPtr Literal(NFrontend::TType type, const NFrontend::TScalar& value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return type.Kind == K::Null ? Call("Null", {}, type) : Nothing(type);
        }
        if (type.OptionalDepth > 1) {
            const auto& inner = *std::get<NFrontend::TOptionalValue>(value);
            return Call("Just", {Literal(inner.Type, inner.Data)}, type);
        }
        if (type.Kind == K::Calendar)
            return Lift(CalendarApply("DateTime2.Split", {Literal({K::Timestamp}, value)}, {{K::Timestamp}}, {K::Calendar}), type);
        if (type.Kind == K::List) {
            TExprNode::TListType items{Type(type.Required())};
            for (const auto& item : std::get<NFrontend::TTuple>(value)->Items) items.push_back(Literal(item.Type, item.Data));
            return Lift(Call("List", std::move(items), type.Required()), type);
        }
        if (type.Kind == K::Tuple) {
            TExprNode::TListType items;
            for (const auto& item : std::get<NFrontend::TTuple>(value)->Items) items.push_back(Literal(item.Type, item.Data));
            auto tuple = Ctx.NewList(Pos, std::move(items));
            tuple->SetTypeAnn(ExportNativeType(type.Required(), Ctx, &Arena));
            return Lift(std::move(tuple), type);
        }
        const auto text = type.Kind == K::Uuid ? std::get<std::string>(value) : NFrontend::FormatValue({type, value});
        TExprNode::TListType args{Atom(String(text))};
        if (type.Kind == K::Decimal) {
            args.push_back(Atom(ToString(type.Precision)));
            args.push_back(Atom(ToString(type.Scale)));
        }
        const auto name = type.Kind == K::Decimal ? std::string("Decimal") : type.Required().ToString();
        auto result = Call(String(name), std::move(args), type.Required());
        return type.OptionalDepth ? Call("Just", {result}, type) : result;
    }
    TExprNode::TPtr Lift(TExprNode::TPtr value, NFrontend::TType target) {
        return target.OptionalDepth && value->GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional ?
            Call("Just", {value}, target) : value;
    }
    TExprNode::TPtr Present(TExprNode::TListType values, const std::vector<NFrontend::TType>& types,
        NFrontend::TType resultType, const std::function<TExprNode::TPtr(TExprNode::TListType)>& build) {
        TExprNode::TListType optionals, arguments;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!types[i].OptionalDepth) continue;
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
            return !source.OptionalDepth && target.OptionalDepth ? Call("Just", {value}, target) : value;
        }
        // Native temporal values format their numeric execution payload. Keep
        // formatting independent from YQL's ISO string conversion policy.
        if (target.IsString() && (source.IsDate() || source.IsInterval())) {
            NFrontend::TType payload{source.IsInterval() || source.IsExtendedTemporal() ? K::Int64 : K::Uint64, source.OptionalDepth};
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
        if (target.OptionalDepth && !source.OptionalDepth && !mayFail) actual = target.Required();
        auto result = Call("SafeCast", {value, Type(actual)}, actual);
        return actual != target ? Call("Just", {result}, target) : result;
    }

    TExprNode::TPtr Lambda(TExprNode::TPtr argument, TExprNode::TPtr body) {
        auto args = Ctx.NewArguments(Pos, {argument});
        args->SetTypeAnn(Ctx.MakeType<TUnitExprType>());
        const TTypeAnnotationNode* type = body->GetTypeAnn();
        auto lambda = Ctx.NewLambda(Pos, std::move(args), std::move(body));
        lambda->SetTypeAnn(type);
        return lambda;
    }

    TExprNode::TPtr InCollection(TExprNode::TPtr lookup, TExprNode::TPtr list,
        NFrontend::TType source, NFrontend::TType key, NFrontend::TType resultType, bool ansi) {
        const NFrontend::TType boolean{K::Bool};
        const auto no = Literal(boolean, false), yes = Literal(boolean, true);
        const auto hasItems = Call("HasItems", {list}, boolean);
        auto nullResult = Nothing(resultType);
        if (ansi) nullResult = Call("If", {hasItems, nullResult, Lift(no, resultType)}, resultType);
        if (source.Kind == K::Null) return nullResult;
        if (key.Kind == K::Null) {
            if (ansi) return nullResult;
            return source.OptionalDepth ? Call("If", {Call("Exists", {lookup}, boolean), Lift(no, resultType), nullResult}, resultType) : no;
        }

        auto item = Ctx.NewArgument(Pos, "item");
        item->SetTypeAnn(ExportNativeType(key, Ctx, &Arena));
        auto nothing = Ctx.NewCallable(Pos, "Void", {});
        nothing->SetTypeAnn(Ctx.MakeType<TVoidExprType>());
        auto settings = Ctx.NewList(Pos, {Atom("Auto"), Atom("One"), Atom("Compact")});
        AnnotateTypeSyntax(*settings, Ctx);
        auto payload = Ctx.NewArgument(Pos, "payload");
        payload->SetTypeAnn(ExportNativeType(key, Ctx, &Arena));
        auto dict = Ctx.NewCallable(Pos, "ToDict", {list, Lambda(item, item), Lambda(payload, nothing), settings});
        dict->SetTypeAnn(Ctx.MakeType<TDictExprType>(ExportNativeType(key, Ctx, &Arena), Ctx.MakeType<TVoidExprType>()));

        auto value = lookup;
        if (source.OptionalDepth) {
            value = Ctx.NewArgument(Pos, "lookup");
            value->SetTypeAnn(ExportNativeType(source.Required(), Ctx, &Arena));
        }
        auto contains = [&](TExprNode::TPtr casted) { return Call("Contains", {dict, Lift(casted, key)}, boolean); };
        TExprNode::TPtr found;
        if (source.Required() == key.Required()) found = contains(value);
        else {
            const auto options = CastResult<false>(ExportNativeType(source.Required(), Ctx, &Arena), ExportNativeType(key.Required(), Ctx, &Arena));
            if (options & NUdf::ECastOptions::Impossible) found = no;
            else if (options & (NUdf::ECastOptions::MayFail | NUdf::ECastOptions::MayLoseData)) {
                const auto casted = Cast(value, source.Required(), key.Required().Optional());
                auto argument = Ctx.NewArgument(Pos, "key");
                argument->SetTypeAnn(ExportNativeType(key.Required(), Ctx, &Arena));
                auto body = contains(argument);
                if (options & NUdf::ECastOptions::MayLoseData)
                    body = Call("And", {Call("==", {value, argument}, boolean), body}, boolean);
                found = Call("IfPresent", {casted, Lambda(argument, body), no}, boolean);
            } else found = contains(Cast(value, source.Required(), key.Required()));
        }
        auto result = Lift(found, resultType);
        if (ansi && key.OptionalDepth) {
            const auto hasNull = Call("Contains", {dict, Nothing(key)}, boolean);
            result = Call("If", {found, Lift(yes, resultType),
                Call("If", {hasNull, Nothing(resultType), Lift(no, resultType)}, resultType)}, resultType);
        }
        if (source.OptionalDepth) result = Call("IfPresent", {lookup, Lambda(value, result), nullResult}, resultType);
        return result;
    }

    TExprNode::TPtr CalendarApply(TStringBuf name, TExprNode::TListType values,
        const std::vector<NFrontend::TType>& types, NFrontend::TType resultType) {
        return Present(std::move(values), types, resultType, [&](TExprNode::TListType args) {
            auto returned = resultType.Required();
            if (name == "DateTime2.ShiftMonths" || name == "DateTime2.ShiftYears") returned.OptionalDepth = true;
            TVector<const TTypeAnnotationNode*> argumentTypes;
            TVector<TCallableExprType::TArgumentInfo> arguments;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const TTypeAnnotationNode* type = args[i]->GetTypeAnn();
                argumentTypes.push_back(type);
                arguments.push_back({type, {}, i == 0 && name != "DateTime2.ToSeconds" ? ui64{NUdf::ICallablePayload::TArgumentFlags::AutoMap} : ui64{0}});
            }
            const auto* voidType = Ctx.MakeType<TVoidExprType>();
            const TTypeAnnotationNode* userType = voidType;
            if (name == "DateTime2.Split" || name == "DateTime2.GetYear" || name == "DateTime2.GetMinute" || name == "DateTime2.ToSeconds" || name == "DateTime2.ShiftMonths" || name == "DateTime2.ShiftYears")
                userType = Ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{
                    Ctx.MakeType<TTupleExprType>(argumentTypes), Ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), voidType});
            const auto* signature = Ctx.MakeType<TCallableExprType>(ExportNativeType(returned, Ctx, &Arena), arguments, 0, TStringBuf{});
            auto voidValue = Ctx.NewCallable(Pos, "Void", {});
            voidValue->SetTypeAnn(voidType);
            auto settings = Ctx.NewList(Pos, {Ctx.NewList(Pos, {Atom("strict")})});
            AnnotateTypeSyntax(*settings, Ctx);
            auto udf = Ctx.NewCallable(Pos, "Udf", {Atom(name), voidValue, Type(userType), Atom(""), Type(signature), Type(voidType), Atom(""), settings});
            udf->SetTypeAnn(signature);
            args.insert(args.begin(), std::move(udf));
            return Call("Apply", std::move(args), returned);
        });
    }
    TExprNode::TPtr Url(TStringBuf name, TExprNode::TPtr input) {
        // Both URL operations accept/return String? and turn an empty slice
        // into NULL. Preserve that fixed public runtime signature directly.
        const NFrontend::TType type{K::String, true};
        const auto* stringType = ExportNativeType(type, Ctx);
        const auto* voidType = Ctx.MakeType<TVoidExprType>();
        const auto* signature = Ctx.MakeType<TCallableExprType>(stringType,
            TVector<TCallableExprType::TArgumentInfo>{{stringType, {}, 0}}, 0, TStringBuf{});
        auto config = Ctx.NewCallable(Pos, "Void", {}); config->SetTypeAnn(voidType);
        auto settings = Ctx.NewList(Pos, {}); AnnotateTypeSyntax(*settings, Ctx);
        auto udf = Ctx.NewCallable(Pos, "Udf", {Atom(name), config, Type(voidType), Atom(""),
            Type(signature), Type(voidType), Atom(""), settings});
        udf->SetTypeAnn(signature);
        return Call("Apply", {udf, input}, type);
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
            AnnotateTypeSyntax(*settings, Ctx);
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

    TExprNode::TPtr Like(TExprNode::TPtr input, TExprNode::TPtr pattern, std::string_view escape, bool caseSensitive) {
        const auto typed = [&](TStringBuf name, TExprNode::TListType args, const TTypeAnnotationNode* type) {
            auto result = Ctx.NewCallable(Pos, name, std::move(args)); result->SetTypeAnn(type); return result;
        };
        const auto* voidType = Ctx.MakeType<TVoidExprType>();
        const auto* stringType = ExportNativeType({K::String}, Ctx);
        const auto* optionalString = ExportNativeType({K::String, true}, Ctx);
        const auto* boolType = ExportNativeType({K::Bool}, Ctx);
        const auto udf = [&](TStringBuf name, const TCallableExprType* signature, TExprNode::TPtr config) {
            auto settings = Ctx.NewList(Pos, {}); AnnotateTypeSyntax(*settings, Ctx);
            return typed("Udf", {Atom(name), config, Type(voidType), Atom(""), Type(signature),
                Type(config->GetTypeAnn()), Atom(""), settings}, signature);
        };
        const auto* converter = Ctx.MakeType<TCallableExprType>(stringType,
            TVector<TCallableExprType::TArgumentInfo>{{stringType, {}, 0}, {optionalString, {}, 0}}, 1, TStringBuf{});
        auto escapeValue = escape.empty() ? Nothing({K::String, true}) : Literal({K::String, true}, std::string(escape));
        auto regex = typed("Apply", {udf("Re2.PatternFromLike", converter, typed("Void", {}, voidType)), pattern, escapeValue}, stringType);

        // Re2.Match's public run-config ABI. Supplying its standard SQL options
        // keeps '_' Unicode-aware even when the pattern contains only ASCII.
        TVector<const TItemExprType*> fields;
        TExprNode::TListType values;
        for (const TStringBuf name : {"CaseSensitive", "DotNl", "Literal", "LogErrors", "LongestMatch", "MaxMem",
                "NeverCapture", "NeverNl", "OneLine", "PerlClasses", "PosixSyntax", "Utf8", "WordBoundary"}) {
            auto value = name == "MaxMem" ? Literal({K::Uint64}, std::uint64_t{8 << 20}) :
                Literal({K::Bool}, (name == "CaseSensitive" && caseSensitive) || name == "LogErrors" || name == "Utf8");
            fields.push_back(Ctx.MakeType<TItemExprType>(name, value->GetTypeAnn()));
            auto field = Ctx.NewList(Pos, {Atom(name), value});
            field->SetTypeAnn(Ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{
                Ctx.MakeType<TUnitExprType>(), value->GetTypeAnn()}));
            values.push_back(std::move(field));
        }
        const auto* optionsType = Ctx.MakeType<TStructExprType>(fields);
        const auto* optionalOptions = Ctx.MakeType<TOptionalExprType>(optionsType);
        auto options = typed("Just", {typed("AsStruct", std::move(values), optionsType)}, optionalOptions);
        auto config = Ctx.NewList(Pos, {regex, options});
        config->SetTypeAnn(Ctx.MakeType<TTupleExprType>(TVector<const TTypeAnnotationNode*>{stringType, optionalOptions}));
        const auto* signature = Ctx.MakeType<TCallableExprType>(boolType,
            TVector<TCallableExprType::TArgumentInfo>{{optionalString, {}, 0}}, 0, TStringBuf{});
        return typed("Apply", {udf("Re2.Match", signature, config), input}, boolType);
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
                arguments.push_back({args[i]->GetTypeAnn(), {}, autoMap && i == 0 && name != "DateTime2.ToSeconds" ? ui64{NUdf::ICallablePayload::TArgumentFlags::AutoMap} : ui64{0}});
            const auto* signature = Ctx.MakeType<TCallableExprType>(returned, arguments, 0, TStringBuf{});
            const auto* voidType = Ctx.MakeType<TVoidExprType>();
            auto settings = Ctx.NewList(Pos, strict ? TExprNode::TListType{Ctx.NewList(Pos, {Atom("strict")})} : TExprNode::TListType{});
            AnnotateTypeSyntax(*settings, Ctx);
            auto udf = typed("Udf", {Atom(name), typed("Void", {}, voidType), Type(voidType), Atom(""),
                Type(signature), Type(voidType), Atom(""), settings}, signature);
            args.insert(args.begin(), std::move(udf));
            return typed("Apply", std::move(args), returned);
        };
        const auto* jsonNode = Ctx.MakeType<TResourceExprType>("JsonNode");
        const auto* optionalNode = Ctx.MakeType<TOptionalExprType>(jsonNode);
        if (inputType.Kind == K::Json) {
            if (inputType.OptionalDepth) {
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
            case O::Collection: {
                if (type.Kind == K::List) {
                    args.insert(args.begin(), Type(type));
                    return Call("List", std::move(args), type);
                }
                auto result = Ctx.NewList(Pos, std::move(args));
                result->SetTypeAnn(ExportNativeType(type, Ctx, &Arena));
                return result;
            }
            case O::InCollection: return InCollection(args[0], args[1], types[0], Arena.ListItem(types[1]), type, node.Name == "ansi");
            case O::JsonValue: return JsonValue(args[0], types[0], node.Name);
            case O::RandomUuid:
                if (args.empty()) return Call("Parameter", {Atom(QueryRandomUuidParameter), Type(type)}, type);
                // Runtime randomness depends on row identities, not argument
                // values. Preserve the terminal dependency-call contract.
                for (std::size_t i = 0; i < args.size(); ++i) args[i] = Call("DependsOn", {args[i]}, types[i]);
                return Call("RandomUuid", std::move(args), type);
            case O::Like: case O::ILike: return Like(args[0], args[1], node.Name, node.Op == O::Like);
            case O::ListElement: {
                const auto item = Arena.ListItem(types[0]);
                return Present(std::move(args), types, type, [&](TExprNode::TListType values) {
                    auto dict = Ctx.NewCallable(Pos, "ToIndexDict", {values[0]});
                    dict->SetTypeAnn(Ctx.MakeType<TDictExprType>(ExportNativeType({K::Uint64}, Ctx), ExportNativeType(item, Ctx, &Arena)));
                    return Call("Lookup", {dict, values[1]}, type);
                });
            }
            case O::TupleElement: {
                const auto index = std::get<std::uint64_t>(node.Value);
                const auto itemType = Arena.TupleItems(types[0])[index];
                return Present(args, types, type, [&](TExprNode::TListType values) {
                    return Call("Nth", {values[0], Atom(ToString(index))}, itemType);
                });
            }
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
            case O::OptionalWrap: return Call("Just", std::move(args), type);
            case O::Unwrap: return Call("Unwrap", std::move(args), type);
            case O::Convert: {
                const auto from = Arena[node.Arguments[0]].Type;
                if (from.IsNumeric() && type.IsNumeric() && from.Kind != K::Decimal && type.Kind != K::Decimal && from.Kind != type.Kind) {
                    auto actual = type; actual.OptionalDepth = from.OptionalDepth;
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
                if (!inputType.OptionalDepth) return Literal({K::Bool}, node.Op == O::IsNotNull);
                auto exists = Call("Exists", std::move(args), {K::Bool});
                return node.Op == O::IsNull ? Call("Not", {exists}, {K::Bool}) : exists;
            }
            case O::If: {
                if (Arena[node.Arguments[0]].Type.OptionalDepth) args[0] = Call("Coalesce", {args[0], Literal({K::Bool}, false)}, {K::Bool});
                return Call("If", std::move(args), type);
            }
            case O::Coalesce: {
                if (type.Kind == K::Null) return Nothing(type);
                if (!types[0].OptionalDepth) return Lift(args[0], type);
                auto actual = type.Optional();
                auto result = args[0];
                for (std::size_t i = 1; i < args.size(); ++i) result = Call("Coalesce", {result, Lift(args[i], actual)}, actual);
                return type.OptionalDepth ? result : Call("Unwrap", {result}, type);
            }
            case O::Abs: return Call("Abs", std::move(args), type);
            case O::Length: {
                auto size = Call("Size", std::move(args), {K::Uint32, type.OptionalDepth});
                return Cast(size, {K::Uint32, type.OptionalDepth}, type);
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
            case O::CalendarMinute: return CalendarApply("DateTime2.GetMinute", std::move(args), types, type);
            case O::TemporalSeconds: return CalendarApply("DateTime2.ToSeconds", std::move(args), types, type);
            case O::UrlHost: return Url("Url.GetHost", args[0]);
            case O::UrlCutWWW: return Url("Url.CutWWW", args[0]);
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
    if (type.Kind == K::List) {
        Y_ENSURE(arena, "List export requires its native arena");
        result = ctx.MakeType<TListExprType>(ExportNativeType(arena->ListItem(type), ctx, arena));
    } else if (type.Kind == K::Tuple) {
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
    for (unsigned depth = 0; depth < type.OptionalDepth; ++depth) result = ctx.MakeType<TOptionalExprType>(result);
    return result;
}

const NYql::TTypeAnnotationNode* ExportNativeParameterType(const NFrontend::TParameterType& type, NYql::TExprContext& ctx) {
    if (const auto* scalar = type.Scalar()) return ExportNativeType(*scalar, ctx);
    if (const auto* list = type.ScalarList()) return ctx.MakeType<TListExprType>(ExportNativeType(list->Item, ctx));
    TVector<const TItemExprType*> fields;
    for (const auto& field : *type.Rows()) fields.push_back(ctx.MakeType<TItemExprType>(String(field.Name), ExportNativeType(field.Type, ctx)));
    return ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(fields));
}

NYql::TExprNode::TPtr LowerNativeParameter(const TString& name, const NFrontend::TParameterType& type,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    const auto* annotation = ExportNativeParameterType(type, ctx);
    auto atom = ctx.NewAtom(pos, name); atom->SetTypeAnn(ctx.MakeType<TUnitExprType>());
    auto parameter = ctx.NewCallable(pos, "Parameter", {std::move(atom), ExecutionType(annotation, ctx, pos)});
    parameter->SetTypeAnn(annotation);
    return parameter;
}

NFrontend::TType ImportNativeType(const NYql::TTypeAnnotationNode& type, NFrontend::TExpressionArena* arena) {
    if (type.GetKind() == ETypeAnnotationKind::List) {
        Y_ENSURE(arena, "List import requires a native arena");
        return arena->ListType(ImportNativeType(*type.Cast<TListExprType>()->GetItemType(), arena));
    }
    if (type.GetKind() == ETypeAnnotationKind::Optional) {
        const auto* item = type.Cast<TOptionalExprType>()->GetItemType();
        auto result = ImportNativeType(*item, arena);
        if (result.OptionalDepth == 64) throw NFrontend::TResourceLimitError({}, "Optional type nesting exceeds 64");
        ++result.OptionalDepth;
        return result;
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

NYql::TExprNode::TPtr LowerExpressionLambda(const TExpression& expression, NYql::TExprContext& ctx) {
    return LowerNativeLambda(*expression.GetNativeArena(), expression.GetNativeRoot(), ctx, expression.Pos());
}

NYql::TExprNode::TPtr LowerNativeOlapPredicate(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    Y_ENSURE(CanLowerNativeOlapPredicate(arena, root, true), "Unsupported native OLAP predicate");
    if (!CanLowerNativeOlapPredicate(arena, root)) {
        // Emit the existing scalar storage kernel only after native optimization.
        // One argument per column also avoids duplicate storage-kernel inputs.
        const auto scalar = LowerNativeLambda(arena, root, ctx, pos);
        const auto row = scalar->Head().HeadPtr();
        TNodeOnNodeOwnedMap replacements;
        TExprNode::TListType arguments, values;
        THashMap<TString, TExprNode::TPtr> columns;
        for (const auto& node : FindNodes(scalar->TailPtr(), [&](const auto& node) {
            return node->IsCallable("Parameter") || (node->IsCallable("Member") && node->HeadPtr() == row);
        })) {
            if (node->IsCallable("Parameter")) {
                auto argument = ctx.NewArgument(pos, TStringBuilder() << "parameter_" << arguments.size());
                argument->SetTypeAnn(node->GetTypeAnn());
                arguments.push_back(argument); values.push_back(node);
                replacements.emplace(node.Get(), argument);
            } else {
                const TString name(node->Tail().Content());
                auto& argument = columns[name];
                if (!argument) {
                    argument = ctx.NewArgument(pos, TStringBuilder() << "column_" << name);
                    argument->SetTypeAnn(node->GetTypeAnn());
                    arguments.push_back(argument);
                    values.push_back(ctx.NewCallable(pos, "KqpOlapApplyColumnArg", {
                        ExpandType(pos, *row->GetTypeAnn(), ctx), ctx.NewAtom(pos, name)}));
                }
                replacements.emplace(node.Get(), argument);
            }
        }
        auto apply = ctx.NewCallable(pos, "KqpOlapApply", {
            ctx.NewLambda(pos, ctx.NewArguments(pos, std::move(arguments)), ctx.ReplaceNodes(scalar->TailPtr(), replacements)),
            ctx.NewList(pos, std::move(values)), ctx.NewAtom(pos, "")});
        auto input = ctx.NewArgument(pos, "native_olap_rows");
        return ctx.NewLambda(pos, ctx.NewArguments(pos, {input}), ctx.NewCallable(pos, "KqpOlapFilter", {input, std::move(apply)}));
    }
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
        if (NDetail::OlapIdentityConversion(arena, node)) value = args[0];
        else if (node.Op == O::Column) value = ctx.NewAtom(pos, ctx.GetIndexAsString(node.ColumnId));
        else if (node.Op == O::Literal || node.Op == O::Parameter) {
            value = LowerNativeLambda(arena, id, ctx, pos)->TailPtr();
            if (node.Op == O::Literal && node.Type.OptionalDepth)
                value = ctx.NewList(pos, {ctx.NewAtom(pos, "just"), value->HeadPtr()});
        }
        else if (const auto name = NDetail::OlapBinaryName(node.Op); !name.empty()) {
            args.insert(args.begin(), ctx.NewAtom(pos, name));
            args.push_back(ExpandType(pos, *ExportNativeType(node.Type, ctx, &arena), ctx));
            value = ctx.NewList(pos, std::move(args));
        } else if (node.Op == O::If) {
            if (arena[node.Arguments[0]].Type.OptionalDepth) {
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

NYql::TExprNode::TPtr LowerNativeOlapProgram(const TOlapProgram& storage,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos) {
    Y_ENSURE(storage.IsNative());
    const auto& program = storage.Native();
    TExprNode::TPtr lambda;
    if (program.Predicate) {
        const auto& predicate = *program.Predicate;
        lambda = LowerNativeOlapPredicate(*predicate.GetNativeArena(), predicate.GetNativeRoot(), ctx, pos);
    } else {
        auto input = ctx.NewArgument(pos, "native_olap_rows");
        auto arguments = ctx.NewArguments(pos, {input});
        lambda = ctx.NewLambda(pos, std::move(arguments), std::move(input));
    }
    if (program.Projections.Keys().Empty()) return lambda;
    TExprNode::TListType projections;
    for (const auto& [column, expression] : program.Projections.Items()) {
        const auto& arena = *expression.GetNativeArena();
        const auto& node = arena[expression.GetNativeRoot()];
        Y_ENSURE(CanLowerNativeOlapProjection(arena, expression.GetNativeRoot()) &&
            arena[node.Arguments[0]].ColumnId == column, "Unsupported native storage projection");
        // Storage annotation runs after column-name conversion. Leave value
        // subtrees unannotated so the backend also visits their literal children.
        auto path = ctx.NewCallable(pos, "Utf8", {ctx.NewAtom(pos, String(node.Name))});
        auto operation = ctx.NewCallable(pos, "KqpOlapJsonValue", {ctx.NewAtom(pos, ctx.GetIndexAsString(column)),
            path, ExpandType(pos, *ExportNativeType({K::Utf8}, ctx), ctx)});
        projections.push_back(ctx.NewCallable(pos, "KqpOlapProjection", {operation, ctx.NewAtom(pos, ctx.GetIndexAsString(column))}));
    }
    auto projected = ctx.NewCallable(pos, "KqpOlapProjections", {lambda->TailPtr(), ctx.NewList(pos, std::move(projections))});
    return ctx.NewLambda(pos, lambda->HeadPtr(), std::move(projected));
}

} // namespace NKikimr::NKqp
