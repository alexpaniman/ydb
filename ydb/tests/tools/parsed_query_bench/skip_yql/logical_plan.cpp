#include "logical_plan.h"

#include <ydb/core/kqp/provider/yql_kikimr_provider.h>
#include <util/string/cast.h>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

namespace F = NFrontend;
using P = Ydb::Query::NativeQuery;
using L = Ydb::Query::LogicalPlan;
using K = F::ETypeKind;
using O = F::EOpcode;

constexpr int MaxOperators = 1024;
constexpr int MaxIUs = 16 * 1024;
constexpr int MaxExpressions = 64 * 1024;
constexpr ui32 MaxExpressionDepth = 64;

template <class T>
const T& Checked(const T& message) {
    Y_ENSURE(message.GetReflection()->GetUnknownFields(message).empty(), "Unknown LogicalPlan field");
    return message;
}

template <class T>
const auto& ById(const T& entries, ui32 id) {
    Y_ENSURE(id && id <= static_cast<size_t>(entries.size()), "Invalid LogicalPlan reference " << id);
    return entries[id - 1];
}

TString Spelling(TStringBuf text) {
    Y_ENSURE(text.size() <= 1024, "LogicalPlan spelling limit exceeded");
    return TString(text);
}

std::string NativeString(TStringBuf text) {
    Y_ENSURE(text.size() <= 1024, "LogicalPlan spelling limit exceeded");
    return {text.data(), text.size()};
}

using TTypes = std::vector<std::optional<F::TType>>;

F::TType Type(const TTypes& types, ui32 id) {
    const auto& type = ById(types, id);
    Y_ENSURE(type, "Expected a value type");
    return *type;
}

TTypes ReadTypes(const L& plan, F::TExpressionArena& arena) {
    TTypes types;
    for (const auto& type : plan.types()) {
        Checked(type);
        Y_ENSURE(type.id() == types.size() + 1, "Invalid LogicalPlan type ID");
        if (type.has_data()) {
            const auto& data = Checked(type.data());
            auto name = NativeString(data.slot());
            Y_ENSURE(data.parameters_size() == (name == "Decimal" ? 2 : 0), "Invalid type parameters");
            if (name == "Decimal") {
                name += "(" + NativeString(data.parameters(0)) + "," + NativeString(data.parameters(1)) + ")";
            }
            const auto native = F::ParseType(name);
            Y_ENSURE(!native.OptionalDepth && native.Kind != K::Null && native.ToString() == name
                && (native.Kind != K::Decimal || data.slot() == "Decimal"), "Non-canonical LogicalPlan data type");
            types.push_back(native);
        } else if (type.has_optional_item()) {
            auto item = Type(types, type.optional_item());
            Y_ENSURE(item.OptionalDepth < MaxExpressionDepth, "Optional type nesting limit exceeded");
            ++item.OptionalDepth;
            types.push_back(item);
        } else if (type.has_list_item()) {
            types.push_back(arena.ListType(Type(types, type.list_item())));
        } else if (type.has_null_type()) {
            Checked(type.null_type());
            types.push_back(F::TType{K::Null});
        } else if (type.has_unit() || type.has_type_of()) {
            if (type.has_unit()) Checked(type.unit());
            else Type(types, type.type_of());
            types.push_back(std::nullopt);
        } else {
            ythrow yexception() << "Unsupported LogicalPlan type";
        }
    }
    return types;
}

struct TImport {
    const L& Plan;
    const TNativeTableResolver& Resolve;
    const F::TBindSettings& Settings;
    std::shared_ptr<F::TExpressionArena> Arena;
    const TTypes& Types;
    NYql::TPositionHandle Pos;
    TNativeQuery Query;
    THashMap<TString, TNativeTable> Tables;
    THashSet<TString> UsedParameters;

    const TNativeTable& Table(const TString& path) {
        if (const auto* existing = Tables.FindPtr(path)) return *existing;
        auto table = Resolve({Settings.DefaultCluster, NativeString(path)}, {});
        Y_ENSURE(table.Table && !path.empty(), "Unresolved LogicalPlan table");
        table.Table.NativeCatalog = table.Schema;
        auto& operation = Query.Execution.TableOperations.emplace_back();
        operation.MutablePosition()->SetRow(1);
        operation.MutablePosition()->SetColumn(1);
        operation.SetTable(table.Table.Path);
        operation.SetOperation(static_cast<ui32>(NYql::TYdbOperation::Select));
        return Tables.emplace(path, std::move(table)).first->second;
    }
};

struct TOperatorEntry {
    TIntrusivePtr<IOperator> Node;
    ui32 Depth;
};

struct TExpressionEntry {
    ui32 Generation = 0;
    F::TExprId Scalar = 0;
    bool Used = false;
};

class TReader {
public:
    TReader(TImport& import, const L::Graph& graph)
        : Import(import)
        , Graph(Checked(graph))
        , Arena(*import.Arena)
    {
        Y_ENSURE(Graph.subqueries().empty() && Graph.replicate_hubs_size() <= Graph.operators_size()
            && Graph.information_units_size() <= MaxIUs && Graph.expressions_size() <= MaxExpressions
            && Graph.operators_size() && Graph.operators_size() <= MaxOperators, "Unsupported LogicalPlan graph size or features");
        Props.NativeExpressions = import.Arena;
        ActiveTypes.resize(Graph.information_units_size());
        std::vector<TString> names(ActiveTypes.size());
        for (const auto& op : Graph.operators()) {
            for (const auto& col : op.read().columns()) {
                Y_ENSURE(col.iu_id() && col.iu_id() <= names.size(), "Invalid LogicalPlan read IU");
                names[col.iu_id() - 1] = Spelling(col.column());
            }
        }
        for (const auto& iu : Graph.information_units()) {
            Checked(iu);
            Y_ENSURE(iu.id() == DeclaredTypes.size() + 1, "Invalid LogicalPlan IU");
            DeclaredTypes.push_back(Type(Import.Types, iu.defined_type_id()));
            const auto& name = names[iu.id() - 1];
            Props.InfoUnitRegistry.Add(TInfoUnit("", name.empty() ? ToString(iu.id()) : name));
        }
        for (const auto& expr : Graph.expressions()) {
            Y_ENSURE(expr.id() == Expressions.size() + 1 && !expr.has_location(), "Invalid LogicalPlan expression ID or location");
            Expressions.emplace_back();
        }
        for (const auto& hub : Graph.replicate_hubs()) {
            Checked(hub);
            Y_ENSURE(hub.id() == Hubs.size() + 1 && !hub.has_location(), "Invalid LogicalPlan replicate hub");
            Hubs.push_back(hub.input_operator_id());
        }
    }

    TIntrusivePtr<TOpRoot> Read(const L::Root& root) {
        for (const auto& op : Graph.operators()) {
            Checked(op);
            Y_ENSURE(op.id() == Operators.size() + 1, "Invalid LogicalPlan operator order");
            ui32 depth = 1;
            auto node = Operator(op, depth);
            Y_ENSURE(depth <= 256, "LogicalPlan operator nesting limit exceeded");
            node->InitializeOutputIUs();
            Operators.push_back({std::move(node), depth});
        }
        ui32 depth = 0;
        auto input = Take(Graph.input_operator_id(), depth);
        Y_ENSURE(std::ranges::none_of(Operators, [](const auto& op) { return bool(op.Node); })
            && std::ranges::all_of(ActiveTypes, [](const auto& t) { return t.has_value(); })
            && std::ranges::all_of(Expressions, [](const auto& e) { return e.Used; })
            && std::ranges::all_of(Hubs, [](auto id) { return id == 0; }), "Unreferenced LogicalPlan graph member");
        TOrderedIUs<TString> columns;
        for (const auto& col : root.columns()) {
            Checked(col);
            columns.Append(Available(col.iu_id(), *input), Spelling(col.name()));
        }
        auto result = MakeIntrusive<TOpRoot>(std::move(input), Import.Pos, std::move(columns));
        Props.ResultIndex = root.result_index();
        Props.EffectPriority = root.result_index();
        result->PlanProps = std::move(Props);
        for (const auto& entry : *result) entry.Current->BindExpressionPlanProps(&result->PlanProps);
        result->ComputeParents();
        return result;
    }

private:
    ui32 Available(ui32 wire, IOperator& input) const {
        Y_ENSURE(ById(ActiveTypes, wire) && input.GetOutputIUs().Contains(wire - 1), "Out-of-scope LogicalPlan input IU");
        return wire - 1;
    }

    void Define(ui32 wire, F::TType actual) {
        Y_ENSURE(ById(DeclaredTypes, wire) == actual && !ActiveTypes[wire - 1],
            "Wrong type or repeated definition for LogicalPlan IU " << wire);
        ActiveTypes[wire - 1] = actual;
    }

    TIntrusivePtr<IOperator> Take(ui32 id, ui32& depth) {
        Y_ENSURE(ById(Operators, id).Node, "Reused LogicalPlan operator");
        auto& child = Operators[id - 1];
        depth = std::max(depth, child.Depth + 1);
        return std::move(child.Node);
    }

    const P::Expression& Expr(ui32 id, ui32 parent) {
        Y_ENSURE(id && id < parent && id <= Expressions.size(), "Invalid or forward LogicalPlan expression edge");
        Expressions[id - 1].Used = true;
        return Checked(Graph.expressions(id - 1));
    }

    void UnitAnnotation(const P::Expression& e) const {
        if (e.has_type_id()) {
            Y_ENSURE(ById(Import.Plan.types(), e.type_id()).has_unit(), "Expected unit annotation");
        }
    }

    TStringBuf Atom(ui32 id, ui32 parent) {
        const auto& expr = Expr(id, parent);
        Y_ENSURE(expr.has_atom(), "Expected a LogicalPlan literal atom");
        UnitAnnotation(expr);
        const auto& atom = Checked(expr.atom());
        Y_ENSURE(atom.flags() <= 1 && atom.content().size() <= 1024, "Unsupported atom flags or size");
        return atom.content();
    }

    const P::ExpressionList& List(ui32 id, ui32 parent) {
        const auto& expr = Expr(id, parent);
        Y_ENSURE(expr.has_list(), "Expected LogicalPlan options list");
        UnitAnnotation(expr);
        return Checked(expr.list());
    }

    F::TExprId Call(const P::Expression& expr, ui32 row, IOperator& input, ui32 depth) {
        const auto& call = Checked(expr.call());
        const auto& fn = ById(Import.Plan.functions(), call.function_id());
        Y_ENSURE(fn.kind() == P::Function::YQL_CALLABLE, "Expected LogicalPlan scalar function");
        const TStringBuf name(fn.name());
        const int n = call.arguments_size();
        auto arg = [&](int i) { return Scalar(call.arguments(i), expr.id(), row, input, depth + 1); };
        if (n == 1 && (name == "Int32" || name == "Uint64" || name == "String")) {
            const auto atom = Atom(call.arguments(0), expr.id());
            if (name == "String") return Arena.Literal({K::String}, NativeString(atom));
            if (name == "Uint64") return Arena.Literal({K::Uint64}, FromString<std::uint64_t>(atom));
            return Arena.Literal({K::Int32}, std::int64_t{FromString<i32>(atom)});
        }
        if (n == 1 && (name == "Exists" || name == "Not"))
            return Arena.Unary(name == "Exists" ? O::IsNotNull : O::Not, arg(0));
        if (n == 2 && name == "SafeCast") {
            const auto& target = Expr(call.arguments(1), expr.id());
            Y_ENSURE(target.has_type_literal() && target.has_type_id()
                && ById(Import.Plan.types(), target.type_id()).type_of() == target.type_literal(), "Invalid cast target");
            return Arena.Cast(arg(0), Type(Import.Types, target.type_literal()));
        }
        if (n == 2) {
            O op;
            if (name == "==") op = O::Equal;
            else if (name == ">=") op = O::GreaterEqual;
            else if (name == "<") op = O::Less;
            else if (name == "And") op = O::And;
            else ythrow yexception() << "Unsupported binary LogicalPlan function: " << name;
            const auto left = arg(0);
            return Arena.Binary(op, left, arg(1));
        }
        if (n == 3 && name == "If") {
            const auto predicate = arg(0), thenValue = arg(1);
            return Arena.If(predicate, thenValue, arg(2));
        }
        if (n == 3 && name == "SqlIn") {
            const auto optionsId = call.arguments(2);
            const auto& options = List(optionsId, expr.id());
            bool ansi = false;
            if (options.items_size()) {
                Y_ENSURE(options.items_size() == 1, "Unsupported SqlIn options");
                const auto entry = options.items(0);
                const auto& tuple = List(entry, optionsId);
                Y_ENSURE(tuple.items_size() == 1 && Atom(tuple.items(0), entry) == "ansi", "Unsupported SqlIn option");
                ansi = true;
            }
            const auto collection = arg(0);
            return Arena.InCollection(arg(1), collection, ansi);
        }
        ythrow yexception() << "Unsupported LogicalPlan callable or arity: " << name;
    }

    F::TExprId Scalar(ui32 id, ui32 parent, ui32 row, IOperator& input, ui32 depth) {
        Y_ENSURE(depth <= MaxExpressionDepth, "LogicalPlan scalar nesting limit exceeded");
        const auto& expr = Expr(id, parent);
        auto& cache = Expressions[id - 1];
        if (cache.Generation == Generation) return cache.Scalar;
        F::TExprId result;
        if (expr.has_column()) {
            const auto& column = Checked(expr.column());
            Y_ENSURE(column.row_argument_expr_id() == row, "Unbound LogicalPlan row argument");
            const auto iu = Available(column.iu_id(), input);
            result = Arena.Column(iu, *ActiveTypes[iu]);
        } else if (expr.has_parameter()) {
            const auto name = Spelling(expr.parameter());
            Y_ENSURE(name.StartsWith('$') && expr.has_type_id(), "Invalid parameter reference");
            const auto nativeName = NativeString(TStringBuf(name).SubStr(1));
            const auto type = Type(Import.Types, expr.type_id());
            const auto& declared = Import.Query.Parameters.at(nativeName);
            Y_ENSURE((declared.Scalar() && *declared.Scalar() == type) || (declared.ScalarList() && type.Kind == K::List
                && type.OptionalDepth == 0 && declared.ScalarList()->Item == Arena.ListItem(type)), "Parameter type mismatch");
            Import.UsedParameters.insert(name);
            result = Arena.Parameter(nativeName, type);
        } else {
            Y_ENSURE(expr.has_call(), "Expected a scalar LogicalPlan expression");
            result = Call(expr, row, input, depth);
        }
        if (expr.has_type_id()) {
            Y_ENSURE(Arena[result].Type == Type(Import.Types, expr.type_id()), "Scalar annotation mismatch");
        }
        cache.Generation = Generation;
        cache.Scalar = result;
        return result;
    }

    TExpression Lambda(ui32 id, IOperator& input) {
        const auto& expr = Expr(id, Expressions.size() + 1);
        Y_ENSURE(expr.has_lambda() && !expr.has_type_id(), "Expected LogicalPlan row lambda");
        const auto& lambda = Checked(expr.lambda());
        const auto& list = Expr(lambda.arguments_expr_id(), id);
        Y_ENSURE(list.has_arguments() && !list.has_type_id() && Checked(list.arguments()).arguments_size() == 1
            && lambda.body_size() == 1, "Expected a unary LogicalPlan row lambda");
        const auto row = list.arguments().arguments(0);
        const auto& binding = Expr(row, list.id());
        Y_ENSURE(binding.has_argument() && !binding.has_type_id() && Checked(binding.argument()).debug_name().size() <= 1024,
            "Invalid LogicalPlan lambda argument");
        ++Generation;
        return TExpression::FromNative(Import.Arena, Scalar(lambda.body(0), id, row, input, 1), &Props, Import.Pos);
    }

    TIntrusivePtr<IOperator> Operator(const L::Operator& op, ui32& depth) {
        const auto pos = Import.Pos;
        switch (op.node_case()) {
            case L::Operator::kRead: {
                const auto& read = Checked(op.read());
                const auto& table = Import.Table(Spelling(read.table_path()));
                TUnorderedIUs columns;
                THashSet<TString> names;
                for (const auto& column : read.columns()) {
                    Checked(column);
                    const auto name = Spelling(column.column());
                    const auto* schema = table.Schema.FindColumn(NativeString(name));
                    Y_ENSURE(schema && names.insert(name).second, "Unknown or duplicate LogicalPlan read column " << name);
                    Define(column.iu_id(), schema->Type);
                    columns.Add(column.iu_id() - 1);
                }
                return MakeIntrusive<TOpRead>("", std::move(columns), table.Storage, table.Table, nullptr, std::nullopt,
                    std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, pos);
            }
            case L::Operator::kRows: {
                const auto& rows = Checked(op.rows());
                Y_ENSURE(!rows.has_rows_expr_id() && rows.columns().empty(), "Expected a unit row");
                return MakeIntrusive<TOpEmptySource>(pos);
            }
            case L::Operator::kMap: {
                const auto& map = Checked(op.map());
                auto input = Take(map.input(), depth);
                Y_ENSURE(map.definitions_size() && map.definitions_size() <= MaxIUs, "Invalid LogicalPlan map size");
                TMapIUs definitions;
                for (const auto& definition : map.definitions()) {
                    Checked(definition);
                    auto e = Lambda(definition.lambda_expr_id(), *input);
                    Define(definition.output_iu_id(), e.GetNativeType());
                    definitions.Add(definition.output_iu_id() - 1, TMapElement(std::move(e)));
                }
                return MakeIntrusive<TOpMap>(std::move(input), pos, std::move(definitions));
            }
            case L::Operator::kFilter: {
                const auto& filter = Checked(op.filter());
                auto input = Take(filter.input(), depth);
                auto e = Lambda(filter.predicate_lambda_expr_id(), *input);
                Y_ENSURE(e.GetNativeType().Kind == K::Bool && e.GetNativeType().OptionalDepth <= 1, "Non-boolean LogicalPlan filter");
                return MakeIntrusive<TOpFilter>(std::move(input), pos, std::move(e));
            }
            case L::Operator::kJoin: {
                const auto& join = Checked(op.join());
                TString kind;
                switch (join.kind()) {
                    case P::Join::INNER: kind = "Inner"; break;
                    case P::Join::CROSS: kind = "Cross"; break;
                    case P::Join::LEFT: kind = "Left"; break;
                    case P::Join::LEFT_ONLY: kind = "LeftOnly"; break;
                    default: ythrow yexception() << "Unsupported LogicalPlan join kind";
                }
                Y_ENSURE(join.filter_lambda_expr_ids().empty() && join.keys_size() <= MaxOperators
                    && (join.keys().empty() == (join.kind() == P::Join::CROSS)), "Unsupported LogicalPlan join conditions");
                auto left = Take(join.left(), depth), right = Take(join.right(), depth);
                TJoinIUs keys;
                for (const auto& key : join.keys()) {
                    Checked(key);
                    const auto l = Available(key.left_iu_id(), *left), r = Available(key.right_iu_id(), *right);
                    const auto type = ActiveTypes[l]->Required();
                    Y_ENSURE(!key.equal_nulls() && type == ActiveTypes[r]->Required() && !type.IsCollection()
                        && !type.IsDocument() && type.Kind != K::Null && keys.Add(l, r), "Invalid LogicalPlan equijoin key");
                }
                if (join.kind() == P::Join::LEFT) {
                    for (auto iu : right->GetOutputIUs()) ActiveTypes[iu] = ActiveTypes[iu]->Optional();
                }
                return MakeIntrusive<TOpJoin>(std::move(left), std::move(right), pos, std::move(kind), std::move(keys));
            }
            case L::Operator::kReplicatePort: {
                const auto& port = Checked(op.replicate_port());
                Y_ENSURE(!port.port_index() && port.columns().empty() && ById(Hubs, port.hub_id()), "Expected the primary replicate port");
                auto input = Take(std::exchange(Hubs[port.hub_id() - 1], 0), depth);
                return TReplicate::Create(std::move(input), pos, Props.InfoUnitRegistry)->AddOutput();
            }
            case L::Operator::kAggregate: {
                const auto& aggregate = Checked(op.aggregate());
                Y_ENSURE(aggregate.key_iu_ids().empty() && !aggregate.distinct_all() && aggregate.aggregations_size()
                    && aggregate.aggregations_size() <= MaxIUs, "Expected COUNT without GROUP BY");
                auto input = Take(aggregate.input(), depth);
                TAggregationIUs traits;
                for (const auto& item : aggregate.aggregations()) {
                    Checked(item);
                    const auto& fn = ById(Import.Plan.functions(), item.function_id());
                    const auto iu = Available(item.input_iu_id(), *input);
                    Y_ENSURE(fn.kind() == P::Function::AGGREGATE && fn.name() == "count" && !item.unwrap()
                        && !ActiveTypes[iu]->IsCollection() && !ActiveTypes[iu]->IsDocument(), "Unsupported LogicalPlan aggregate");
                    Define(item.output_iu_id(), {K::Uint64});
                    traits.Add(item.output_iu_id() - 1, TOpAggregationTraits{iu, "count", item.distinct(), false, std::nullopt});
                }
                return MakeIntrusive<TOpAggregate>(std::move(input), std::move(traits), TOrderedIUs<>{}, EOpPhase::Undefined, false, pos);
            }
            default: ythrow yexception() << "Unsupported LogicalPlan operator";
        }
    }

    TImport& Import;
    const L::Graph& Graph;
    F::TExpressionArena& Arena;
    TPlanProps Props;
    std::vector<F::TType> DeclaredTypes;
    std::vector<std::optional<F::TType>> ActiveTypes;
    std::vector<TOperatorEntry> Operators;
    std::vector<ui32> Hubs;
    std::vector<TExpressionEntry> Expressions;
    ui32 Generation = 0;
};

} // namespace

TNativeQuery ImportNativeLogicalPlan(const L& plan, const TNativeTableResolver& resolve,
    NYql::TExprContext& ctx, const F::TBindSettings& settings) {
    Checked(plan);
    Y_ENSURE(plan.version() == 1 && plan.required_features().empty() && plan.roots_size() && plan.roots_size() <= MaxOperators
        && plan.types_size() <= MaxOperators && plan.functions_size() <= 64, "Unsupported LogicalPlan version, features or size");
    auto arena = std::make_shared<F::TExpressionArena>();
    const auto types = ReadTypes(plan, *arena);
    TImport import{plan, resolve, settings, std::move(arena), types, ctx.AppendPosition(NYql::TPosition(1, 1)), {}, {}, {}};
    ui32 number = 0;
    THashSet<TString> names;
    static const THashSet<TStringBuf> scalarNames{
        "Int32", "Uint64", "String", "Exists", "Not", "SafeCast", "==", ">=", "<", "And", "If", "SqlIn"
    };
    for (const auto& fn : plan.functions()) {
        Checked(fn);
        Y_ENSURE(fn.id() == ++number && fn.module().empty() && fn.implementation_version().empty()
            && !fn.has_signature_type_id() && names.insert(Spelling(fn.name())).second
            && ((fn.kind() == P::Function::AGGREGATE && fn.name() == "count")
                || (fn.kind() == P::Function::YQL_CALLABLE && scalarNames.contains(fn.name()))), "Invalid LogicalPlan function");
    }
    for (const auto& parameter : plan.parameters()) {
        Checked(parameter);
        const auto spelling = Spelling(parameter.name());
        Y_ENSURE(spelling.StartsWith('$') && spelling.size() > 1, "Invalid LogicalPlan parameter name");
        const auto name = NativeString(TStringBuf(spelling).SubStr(1));
        const auto type = Type(types, parameter.type_id());
        const F::TParameterType declared = type.Kind == K::List && !type.OptionalDepth
            ? F::TParameterType(F::TParameterType::TList{import.Arena->ListItem(type)}) : F::TParameterType(type);
        const auto supplied = settings.ParameterTypes.find(name);
        Y_ENSURE(supplied != settings.ParameterTypes.end() && supplied->second == declared
            && import.Query.Parameters.emplace(name, declared).second, "Missing or mismatched LogicalPlan parameter " << spelling);
    }
    for (const auto& root : plan.roots()) {
        Checked(root);
        const ui32 index = import.Query.Roots.size();
        Y_ENSURE(root.id() == index + 1 && root.has_result_index() && root.result_index() == index && root.has_graph()
            && root.has_statement_index() && (!index || plan.roots(index - 1).statement_index() < root.statement_index())
            && !root.has_returning_limit_expr_id() && !root.ordered_rows() && root.columns_size() && root.columns_size() <= MaxIUs,
            "Expected unordered read-only logical roots in statement order");
        import.Query.Roots.push_back(TReader(import, root.graph()).Read(root));
        import.Query.Execution.DiscardResults.push_back(false);
    }
    Y_ENSURE(import.UsedParameters.size() == import.Query.Parameters.size(), "Unused LogicalPlan parameter");
    return std::move(import.Query);
}

} // namespace NKikimr::NKqp::NParsedQueryBench
