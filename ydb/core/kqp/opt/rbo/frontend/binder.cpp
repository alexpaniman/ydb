#include "binder.h"
#include "diagnostic.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace NKikimr::NKqp::NFrontend {
namespace {

std::string Lower(std::string_view value) {
    std::string result(value);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return result;
}

bool IsAggregate(std::string_view name) {
    return name == "count" || name == "sum" || name == "avg" || name == "min" || name == "max" || name == "some";
}

struct TScopeColumn {
    TColumnId Id;
    std::string Name;
    std::string Qualifier;
    TType Type;
};

class TScope {
public:
    void Add(TScopeColumn column) {
        const auto index = Columns.size();
        auto addName = [&](const std::string& name) {
            auto [it, inserted] = Names.emplace(name, index);
            if (!inserted) it->second = Ambiguous;
        };
        addName(column.Name);
        if (!column.Qualifier.empty()) addName(column.Qualifier + "." + column.Name);
        Columns.push_back(std::move(column));
    }

    const TScopeColumn* Find(std::string_view name, TSourcePosition pos) const {
        auto it = Names.find(std::string(name));
        if (it == Names.end()) return nullptr;
        if (it->second == Ambiguous) throw TFrontendError(pos, "ambiguous column: " + std::string(name));
        return &Columns[it->second];
    }

    std::vector<TColumnId> Ids() const {
        std::vector<TColumnId> result;
        for (const auto& column : Columns) result.push_back(column.Id);
        return result;
    }

    std::vector<TScopeColumn> Columns;
private:
    static constexpr std::size_t Ambiguous = std::numeric_limits<std::size_t>::max();
    std::unordered_map<std::string, std::size_t> Names;
};

struct TBoundSource {
    TRelId Relation;
    TScope Scope;
};

struct TSyntaxKey {
    ESyntaxKind Kind;
    std::string Text;
    std::vector<std::uint32_t> Arguments;
    bool Distinct;
    bool operator==(const TSyntaxKey&) const = default;
};

struct TSyntaxHash {
    std::size_t operator()(const TSyntaxKey& key) const {
        std::size_t value = std::hash<std::string>{}(key.Text) ^ static_cast<unsigned>(key.Kind) ^ key.Distinct;
        for (auto arg : key.Arguments) value ^= arg + 0x9e3779b9 + (value << 6) + (value >> 2);
        return value;
    }
};

struct TAggregationContext {
    TScope Source;
    std::vector<TDefinition> Definitions;
    std::vector<TColumnId> Keys;
    std::vector<TAggregate> Aggregates;
    std::unordered_map<std::uint32_t, TScopeColumn> Groups;
    std::unordered_map<std::uint32_t, TExprId> Calls;
};

struct TExprContext {
    TExprContext(const TScope& scope, TAggregationContext* aggregation = nullptr,
        const std::unordered_map<std::string, TExprId>* arguments = nullptr)
        : Scope(scope), Aggregation(aggregation), Arguments(arguments) {}
    const TScope& Scope;
    TAggregationContext* Aggregation = nullptr;
    const std::unordered_map<std::string, TExprId>* Arguments = nullptr;
    std::unordered_map<TSyntaxId, TExprId> Cache;
};

class TBinder {
public:
    TBinder(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings)
        : Syntax(syntax), Catalog(catalog), Settings(settings), Prefix(settings.TablePathPrefix)
    {
        Program.Parameters = settings.ParameterTypes;
        Canonical.reserve(syntax.Expressions.size());
        ContainsAggregate.reserve(syntax.Expressions.size());
        std::unordered_map<TSyntaxKey, std::uint32_t, TSyntaxHash> interned;
        for (std::size_t i = 0; i < syntax.Expressions.size(); ++i) {
            const auto& expr = syntax.Expressions[i];
            const auto normalized = (expr.Kind == ESyntaxKind::Call && !expr.Text.starts_with('$')) || expr.Kind == ESyntaxKind::Cast;
            TSyntaxKey key{expr.Kind, normalized ? Lower(expr.Text) : expr.Text, {}, expr.Distinct};
            bool aggregate = expr.Kind == ESyntaxKind::Call && IsAggregate(Lower(expr.Text));
            for (auto child : expr.Arguments) {
                if (child >= i) throw TFrontendError(expr.Position, "syntax tree is not topologically ordered");
                key.Arguments.push_back(Canonical[child]);
                aggregate = aggregate || ContainsAggregate[child];
            }
            auto [it, inserted] = interned.emplace(std::move(key), static_cast<std::uint32_t>(interned.size()));
            Canonical.push_back(it->second);
            ContainsAggregate.push_back(aggregate);
        }
    }

    TBoundProgram Run() {
        for (const auto& statement : Syntax.Statements) {
            switch (statement.Kind) {
                case EStatementKind::Query: Program.Results.push_back(Select(*statement.Query)); break;
                case EStatementKind::Declaration: {
                    auto type = ParseType(statement.Value, statement.Position);
                    auto [it, inserted] = Program.Parameters.emplace(statement.Name, type);
                    if (!inserted && it->second != type) throw TFrontendError(statement.Position, "parameter type does not match DECLARE: " + statement.Name);
                    if (!Declared.insert(statement.Name).second) throw TFrontendError(statement.Position, "duplicate DECLARE: " + statement.Name);
                    break;
                }
                case EStatementKind::Binding: {
                    if (!BoundNames.insert(statement.Name).second || Program.Parameters.contains(statement.Name)) {
                        throw TFrontendError(statement.Position, "duplicate binding: $" + statement.Name);
                    }
                    if (statement.Query) Queries.emplace("$" + statement.Name, statement.Query);
                    else if (statement.Lambda) {
                        std::unordered_set<std::string> params;
                        for (const auto& param : statement.Parameters) if (!params.insert(param).second) throw TFrontendError(statement.Position, "duplicate lambda parameter");
                        Lambdas.emplace(statement.Name, &statement);
                    } else {
                        const TScope empty;
                        TExprContext context{empty};
                        Values.emplace(statement.Name, Expr(statement.Expression, context));
                    }
                    break;
                }
                case EStatementKind::Use:
                    throw TUnsupportedFeature(EFeature::Statement, statement.Position, "USE is not supported; resolve table paths through the catalog");
                case EStatementKind::Pragma: {
                    const auto name = Lower(statement.Name);
                    if (name == "tablepathprefix") Prefix = statement.Value;
                    else if (name == "yqlselect" && Lower(statement.Value) == "force") {
                        // This selects relational translation, which is intrinsic here.
                    } else if (name == "ansiimplicitcrossjoin" && statement.Value.empty()) {
                        // Comma joins have one unambiguous cross-product meaning.
                    } else throw TUnsupportedFeature(EFeature::Pragma, statement.Position, "unsupported pragma: " + statement.Name);
                    break;
                }
            }
            CheckSize(statement.Position);
        }
        if (Program.Results.empty()) throw TUnsupportedFeature(EFeature::Statement, {}, "native read queries require a result statement");
        return std::move(Program);
    }

private:
    struct TDepth {
        TBinder& Binder;
        TDepth(TBinder& binder, TSourcePosition pos) : Binder(binder) {
            if (++Binder.Depth > Binder.Settings.MaxExpansionDepth) throw TResourceLimitError(pos, "binding expansion limit exceeded");
        }
        ~TDepth() { --Binder.Depth; }
    };

    void CheckSize(TSourcePosition pos) const {
        if (Program.Expressions.Size() + Program.Relations.size() + Program.Columns.size() > Settings.MaxNodes) {
            throw TResourceLimitError(pos, "bound program size limit exceeded");
        }
    }
    TColumnId Column(std::string name, std::string qualifier, TType type, TSourcePosition pos) {
        CheckSize(pos);
        auto id = static_cast<TColumnId>(Program.Columns.size());
        Program.Columns.push_back({std::move(name), std::move(qualifier), type}); return id;
    }
    TRelId Relation(TRelation relation) {
        CheckSize(relation.Position);
        auto id = static_cast<TRelId>(Program.Relations.size()); Program.Relations.push_back(std::move(relation)); return id;
    }
    TScopeColumn ScopeColumn(TColumnId id) const {
        const auto& column = Program.Columns.at(id); return {id, column.Name, column.Qualifier, column.Type};
    }
    TExprId Access(const TScopeColumn& column, TSourcePosition pos) {
        return Program.Expressions.Column(column.Id, column.Type, pos);
    }
    TColumnId Materialize(TExprId expr, std::vector<TDefinition>& definitions, TSourcePosition pos, std::string name = {}) {
        const auto& node = Program.Expressions[expr];
        if (node.Op == EOpcode::Column) return node.ColumnId;
        if (name.empty()) name = "expr" + std::to_string(Program.Columns.size());
        auto column = Column(std::move(name), {}, node.Type, pos);
        definitions.push_back({column, expr}); return column;
    }
    TRelId Map(TRelId input, std::vector<TDefinition> definitions, TSourcePosition pos) {
        if (definitions.empty()) return input;
        TRelation map{ERelation::Map, pos}; map.Inputs = {input}; map.Columns = Program.Relations[input].Columns;
        for (const auto& definition : definitions) map.Columns.push_back(definition.Column);
        map.Definitions = std::move(definitions); return Relation(std::move(map));
    }
    TRelId Filter(TRelId input, TExprId predicate, TSourcePosition pos) {
        const auto type = Program.Expressions[predicate].Type;
        if (type.Kind != ETypeKind::Bool && type.Kind != ETypeKind::Null) throw TFrontendError(pos, "predicate must have Boolean type");
        if (type.Kind == ETypeKind::Null) predicate = Program.Expressions.Literal({ETypeKind::Bool}, false, pos);
        TRelation filter{ERelation::Filter, pos}; filter.Inputs = {input}; filter.Columns = Program.Relations[input].Columns;
        filter.Predicate = predicate; return Relation(std::move(filter));
    }

    TBoundSource Source(const TSource& source) {
        TDepth depth(*this, source.Position);
        const auto named = Queries.find(source.Name);
        if (source.Query || named != Queries.end()) {
            auto query = source.Query ? source.Query : named->second;
            if (!ActiveQueries.insert(query.get()).second) throw TFrontendError(source.Position, "recursive query binding is not supported");
            auto result = Select(*query);
            ActiveQueries.erase(query.get());
            TScope scope;
            for (const auto& column : result.Columns) scope.Add({column.Column, column.Name, source.Alias.empty() ? source.Name : source.Alias, column.Type});
            return {result.Root, std::move(scope)};
        }
        if (source.Name.starts_with('$')) throw TFrontendError(source.Position, "unknown query binding: " + source.Name);
        std::string path = source.Name;
        if (!Prefix.empty() && !path.starts_with('/')) path = Prefix + (Prefix.ends_with('/') ? "" : "/") + path;
        auto table = Catalog(path, source.Position);
        if (table.Name.empty()) throw TFrontendError(source.Position, "catalog returned an unnamed table");
        TRelation read{ERelation::Read, source.Position}; read.Table = table.Name;
        std::string alias = source.Alias;
        if (alias.empty()) alias = source.Name.substr(source.Name.find_last_of("/.") + 1);
        TScope scope;
        std::unordered_set<std::string> names;
        for (const auto& column : table.Columns) {
            if (!names.insert(column.Name).second) throw TFrontendError(source.Position, "duplicate catalog column: " + column.Name);
            auto id = Column(column.Name, alias, column.Type, source.Position); read.Columns.push_back(id); scope.Add(ScopeColumn(id));
        }
        return {Relation(std::move(read)), std::move(scope)};
    }

    TBoundSource From(const TSelect& query) {
        TBoundSource current;
        if (query.Source) current = Source(*query.Source);
        else current.Relation = Relation({ERelation::Unit, query.Position});
        for (const auto& join : query.Joins) {
            auto right = Source(join.Source);
            TScope combined;
            for (const auto& column : current.Scope.Columns) combined.Add(column);
            for (const auto& column : right.Scope.Columns) combined.Add(column);
            TExprContext context{combined};
            std::optional<TExprId> predicate;
            if (join.Predicate != NoSyntax) predicate = Expr(join.Predicate, context);
            if (!join.Using.empty()) {
                // USING also changes unqualified projection/star semantics.
                // Do not accept it until that output coalescing is represented.
                throw TUnsupportedFeature(EFeature::JoinUsing, join.Source.Position, "JOIN USING is not supported; use an explicit ON predicate");
            }
            if (predicate && Program.Expressions[*predicate].Type.Kind != ETypeKind::Bool) throw TFrontendError(join.Source.Position, "join predicate must have Boolean type");
            TScope output;
            const bool keepLeft = join.Kind != EJoinKind::RightSemi && join.Kind != EJoinKind::RightOnly;
            const bool keepRight = join.Kind != EJoinKind::LeftSemi && join.Kind != EJoinKind::LeftOnly;
            if (keepLeft) for (auto column : current.Scope.Columns) {
                if (join.Kind == EJoinKind::Right || join.Kind == EJoinKind::Full) column.Type = column.Type.Optional();
                output.Add(std::move(column));
            }
            if (keepRight) for (auto column : right.Scope.Columns) {
                if (join.Kind == EJoinKind::Left || join.Kind == EJoinKind::Full) column.Type = column.Type.Optional();
                output.Add(std::move(column));
            }
            TRelation relation{ERelation::Join, join.Source.Position}; relation.Inputs = {current.Relation, right.Relation};
            relation.Columns = output.Ids(); relation.Predicate = predicate; relation.JoinKind = join.Kind;
            current = {Relation(std::move(relation)), std::move(output)};
        }
        return current;
    }

    TQueryResult Select(const TSelect& query) {
        TDepth depth(*this, query.Position);
        const auto savedQueries = Queries;
        for (const auto& [name, cte] : query.Ctes) {
            if (Queries.contains(name)) throw TFrontendError(cte->Position, "duplicate query binding: " + name);
            Queries.emplace(name, cte);
        }
        auto source = From(query);
        TExprContext sourceContext{source.Scope};
        if (query.Where != NoSyntax) source.Relation = Filter(source.Relation, Expr(query.Where, sourceContext), Syntax.Expressions[query.Where].Position);

        bool aggregate = !query.GroupBy.empty();
        for (const auto& projection : query.Projection) aggregate = aggregate || ContainsAggregate[projection.Expression];
        if (query.Having != NoSyntax) aggregate = aggregate || ContainsAggregate[query.Having];
        for (const auto& key : query.OrderBy) aggregate = aggregate || ContainsAggregate[key.Expression];
        TAggregationContext aggregation;
        TScope grouped;
        if (aggregate) {
            aggregation.Source = source.Scope;
            std::unordered_set<TColumnId> seen;
            for (const auto& key : query.GroupBy) {
                if (key.Alias.empty() && Syntax.Expressions[key.Expression].Kind != ESyntaxKind::Column)
                    throw TUnsupportedFeature(EFeature::Expression, Syntax.Expressions[key.Expression].Position,
                        "computed GROUP BY keys require an explicit alias in native-read-v1");
                auto expression = Expr(key.Expression, sourceContext);
                auto id = Materialize(expression, aggregation.Definitions, Syntax.Expressions[key.Expression].Position, key.Alias);
                if (seen.insert(id).second) aggregation.Keys.push_back(id);
                auto column = ScopeColumn(id);
                column.Type = Program.Expressions[expression].Type;
                aggregation.Groups.emplace(Canonical[key.Expression], column);
                if (!key.Alias.empty()) column.Name = key.Alias;
                grouped.Add(std::move(column));
            }
        }
        TExprContext context{aggregate ? grouped : source.Scope, aggregate ? &aggregation : nullptr};
        std::vector<TDefinition> definitions;
        std::vector<TResultColumn> outputs;
        TScope outputScope;
        for (std::size_t index = 0; index < query.Projection.size(); ++index) {
            const auto& projection = query.Projection[index];
            const auto& syntax = Syntax.Expressions[projection.Expression];
            if (syntax.Kind == ESyntaxKind::Star) {
                if (!query.Joins.empty()) throw TUnsupportedFeature(EFeature::Expression, syntax.Position,
                    "stars over joins require YQL column-name coalescing and are outside native-read-v1");
                if (!projection.Alias.empty()) throw TFrontendError(syntax.Position, "a star cannot have an alias");
                bool found = false;
                for (const auto& column : context.Scope.Columns) if (syntax.Text.empty() || column.Qualifier == syntax.Text) {
                    outputs.push_back({column.Id, column.Name, column.Type}); outputScope.Add(column); found = true;
                }
                if (!found) throw TFrontendError(syntax.Position, "star has no matching columns");
                continue;
            }
            auto expr = Expr(projection.Expression, context);
            std::string name = projection.Alias;
            if (name.empty()) name = syntax.Kind == ESyntaxKind::Column ?
                (query.Joins.empty() ? syntax.Text.substr(syntax.Text.find_last_of('.') + 1) : syntax.Text) : "column" + std::to_string(index);
            auto id = Materialize(expr, definitions, syntax.Position, name);
            auto type = Program.Expressions[expr].Type;
            outputs.push_back({id, name, type}); outputScope.Add({id, name, {}, type});
        }

        // RBO result bindings are unique IDs even when SQL projects one
        // source column repeatedly under different output names.
        std::unordered_set<TColumnId> projected;
        outputScope = {};
        for (auto& output : outputs) {
            if (!projected.insert(output.Column).second) {
                const auto expression = Program.Expressions.Column(output.Column, output.Type, query.Position);
                output.Column = Column(output.Name, {}, output.Type, query.Position);
                definitions.push_back({output.Column, expression});
            }
            outputScope.Add({output.Column, output.Name, {}, output.Type});
        }

        std::optional<TExprId> having;
        if (query.Having != NoSyntax) {
            if (!aggregate) throw TFrontendError(query.Position, "HAVING requires grouping or aggregation");
            having = Expr(query.Having, context);
        }
        // ORDER BY aliases bind to result columns; other expressions see the
        // appropriate input/grouping scope. They are materialized once.
        std::vector<TBoundSortKey> sort;
        for (const auto& key : query.OrderBy) {
            const auto& syntax = Syntax.Expressions[key.Expression];
            const TScopeColumn* alias = syntax.Kind == ESyntaxKind::Column ? outputScope.Find(syntax.Text, syntax.Position) : nullptr;
            if (alias) sort.push_back({alias->Id, key.Descending});
            else {
                auto expr = Expr(key.Expression, context);
                auto id = Materialize(expr, definitions, syntax.Position);
                if (query.Distinct && std::none_of(outputs.begin(), outputs.end(), [&](const auto& output) { return output.Column == id; })) {
                    throw TFrontendError(syntax.Position, "ORDER BY in SELECT DISTINCT must name a result column");
                }
                sort.push_back({id, key.Descending});
            }
        }
        auto root = source.Relation;
        if (aggregate) {
            root = Map(root, std::move(aggregation.Definitions), query.Position);
            TRelation node{ERelation::Aggregate, query.Position}; node.Inputs = {root}; node.GroupKeys = std::move(aggregation.Keys);
            node.Columns = node.GroupKeys; node.Aggregates = std::move(aggregation.Aggregates);
            for (const auto& agg : node.Aggregates) node.Columns.push_back(agg.Output);
            root = Relation(std::move(node));
        }
        if (having) root = Filter(root, *having, query.Position);
        root = Map(root, std::move(definitions), query.Position);
        if (query.Distinct) {
            TRelation node{ERelation::Aggregate, query.Position}; node.Inputs = {root};
            std::unordered_set<TColumnId> seen;
            for (const auto& output : outputs) if (seen.insert(output.Column).second) node.GroupKeys.push_back(output.Column);
            node.Columns = node.GroupKeys; root = Relation(std::move(node));
        }

        if (!query.UnionAll.empty()) {
            if (!sort.empty()) throw TUnsupportedFeature(EFeature::SetOperation, query.Position, "ORDER BY over UNION ALL is not supported yet");
            std::vector<TQueryResult> branches{{root, outputs}};
            for (const auto& branch : query.UnionAll) {
                branches.push_back(Select(*branch));
                if (branches.back().Columns.size() != outputs.size()) throw TFrontendError(branch->Position, "UNION ALL column count mismatch");
            }
            std::vector<TType> types;
            for (const auto& output : outputs) types.push_back(output.Type);
            for (std::size_t b = 1; b < branches.size(); ++b) for (std::size_t i = 0; i < types.size(); ++i) {
                types[i] = CommonType(types[i], branches[b].Columns[i].Type, query.Position);
            }
            TRelation node{ERelation::UnionAll, query.Position};
            for (auto& branch : branches) {
                std::vector<TDefinition> conversions;
                std::vector<TColumnId> columns;
                for (std::size_t i = 0; i < branch.Columns.size(); ++i) {
                    auto id = branch.Columns[i].Column;
                    if (branch.Columns[i].Type != types[i]) {
                        auto expr = Program.Expressions.Column(id, branch.Columns[i].Type, query.Position);
                        expr = Program.Expressions.Convert(expr, types[i], query.Position);
                        id = Materialize(expr, conversions, query.Position);
                    }
                    columns.push_back(id);
                }
                node.Inputs.push_back(Map(branch.Root, std::move(conversions), query.Position));
                node.UnionInputs.push_back(std::move(columns));
            }
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                auto id = Column(outputs[i].Name, {}, types[i], query.Position); node.Columns.push_back(id);
                outputs[i] = {id, outputs[i].Name, types[i]};
            }
            root = Relation(std::move(node));
        }
        if (!sort.empty()) {
            TRelation node{ERelation::Sort, query.Position}; node.Inputs = {root}; node.Columns = Program.Relations[root].Columns;
            node.SortKeys = std::move(sort); root = Relation(std::move(node));
        }
        if (query.Limit != NoSyntax) {
            const TScope empty;
            TExprContext limitContext{empty};
            auto count = Expr(query.Limit, limitContext);
            count = CheckCount(count, Syntax.Expressions[query.Limit].Position);
            TRelation node{ERelation::Limit, query.Position}; node.Inputs = {root}; node.Columns = Program.Relations[root].Columns; node.Count = count;
            if (query.Offset != NoSyntax) {
                node.Offset = CheckCount(Expr(query.Offset, limitContext), Syntax.Expressions[query.Offset].Position);
            }
            root = Relation(std::move(node));
        }
        Queries = savedQueries;
        return {root, std::move(outputs)};
    }

    TExprId CheckCount(TExprId expression, TSourcePosition pos) {
        const auto& node = Program.Expressions[expression];
        if (!node.Type.IsIntegral() || node.Type.Nullable) throw TFrontendError(pos, "LIMIT/OFFSET must be a non-null integer");
        if (node.Op == EOpcode::Literal && std::holds_alternative<std::int64_t>(node.Value) && std::get<std::int64_t>(node.Value) < 0) {
            throw TFrontendError(pos, "LIMIT/OFFSET must be nonnegative");
        }
        if (node.Op == EOpcode::Literal) {
            const auto value = std::holds_alternative<std::int64_t>(node.Value) ?
                static_cast<std::uint64_t>(std::get<std::int64_t>(node.Value)) : std::get<std::uint64_t>(node.Value);
            return Program.Expressions.Literal({ETypeKind::Uint64}, value, pos);
        }
        if (node.Type.IsSigned()) throw TUnsupportedFeature(EFeature::Expression, pos, "dynamic LIMIT/OFFSET requires an unsigned integer");
        return Program.Expressions.Convert(expression, {ETypeKind::Uint64}, pos);
    }

    TExprId Aggregate(const TSyntaxExpr& syntax, std::uint32_t canonical, TAggregationContext& context) {
        if (auto it = context.Calls.find(canonical); it != context.Calls.end()) return it->second;
        const auto name = Lower(syntax.Text);
        if (syntax.Arguments.size() != 1) throw TFrontendError(syntax.Position, "aggregate requires one argument");
        auto arg = syntax.Arguments[0];
        TExprId input;
        if (Syntax.Expressions[arg].Kind == ESyntaxKind::Star) {
            if (name != "count" || syntax.Distinct) throw TFrontendError(syntax.Position, "only COUNT(*) accepts star");
            input = Program.Expressions.Literal({ETypeKind::Uint64}, std::uint64_t{1}, syntax.Position);
        } else {
            TExprContext sourceContext{context.Source}; input = Expr(arg, sourceContext);
        }
        const auto inputType = Program.Expressions[input].Type;
        auto type = inputType;
        if (name == "count") type = {ETypeKind::Uint64};
        else if (name == "sum" || name == "avg") {
            if (!type.IsNumeric()) throw TFrontendError(syntax.Position, name + " expects a numeric argument");
            if (name == "avg" && type.Kind != ETypeKind::Decimal) type = {ETypeKind::Double, type.Nullable};
            else if (type.IsIntegral()) type.Kind = type.IsSigned() ? ETypeKind::Int64 : ETypeKind::Uint64;
            if (type.Kind == ETypeKind::Decimal && name == "sum") type.Precision = 35;
        }
        if (name != "count" && context.Keys.empty()) type = type.Optional();
        auto inputColumn = Materialize(input, context.Definitions, syntax.Position);
        auto output = Column(name + std::to_string(Program.Columns.size()), {}, type, syntax.Position);
        context.Aggregates.push_back({output, inputColumn, name, syntax.Distinct});
        auto result = Program.Expressions.Column(output, type, syntax.Position); context.Calls.emplace(canonical, result); return result;
    }

    TExprId Integer(const TSyntaxExpr& syntax, bool negative = false) {
        std::string_view value = syntax.Text;
        int radix = 10;
        if (value.size() > 2 && value[0] == '0') {
            if (value[1] == 'x' || value[1] == 'X') radix = 16;
            if (value[1] == 'o' || value[1] == 'O') radix = 8;
            if (value[1] == 'b' || value[1] == 'B') radix = 2;
            if (radix != 10) value.remove_prefix(2);
        }
        std::uint64_t number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number, radix);
        if (error != std::errc()) throw TFrontendError(syntax.Position, "integer literal is out of range or malformed");
        auto suffix = Lower(std::string_view(end, value.data() + value.size() - end));
        const bool unsignedValue = suffix.starts_with('u');
        if (unsignedValue) suffix.erase(0, 1);
        ETypeKind kind;
        if (suffix.empty()) kind = unsignedValue ? ETypeKind::Uint32 :
            number <= std::uint64_t{INT32_MAX} + negative ? ETypeKind::Int32 :
            number <= std::uint64_t{INT64_MAX} + negative ? ETypeKind::Int64 : ETypeKind::Uint64;
        else if (suffix == "l") kind = unsignedValue ? ETypeKind::Uint64 : ETypeKind::Int64;
        else if (suffix == "s") kind = unsignedValue ? ETypeKind::Uint16 : ETypeKind::Int16;
        else if (suffix == "t") kind = unsignedValue ? ETypeKind::Uint8 : ETypeKind::Int8;
        else throw TUnsupportedFeature(EFeature::Syntax, syntax.Position, "unsupported integer suffix");
        const TType type{kind};
        if (type.IsSigned()) {
            if (number > std::uint64_t{INT64_MAX} + negative) throw TFrontendError(syntax.Position, "signed integer literal is out of range");
            const auto value = negative ? static_cast<std::int64_t>(-static_cast<__int128>(number)) : static_cast<std::int64_t>(number);
            return Program.Expressions.Literal(type, value, syntax.Position);
        }
        if (negative && !unsignedValue) throw TFrontendError(syntax.Position, "negative integer literal is out of range");
        auto literal = Program.Expressions.Literal(type, number, syntax.Position);
        return negative ? Program.Expressions.Unary(EOpcode::Negate, literal, syntax.Position) : literal;
    }

    TExprId Expr(TSyntaxId id, TExprContext& context) {
        if (auto it = context.Cache.find(id); it != context.Cache.end()) return it->second;
        const auto& syntax = Syntax.Expressions.at(id);
        TDepth depth(*this, syntax.Position);
        CheckSize(syntax.Position);
        if (context.Aggregation) if (auto it = context.Aggregation->Groups.find(Canonical[id]); it != context.Aggregation->Groups.end()) {
            auto result = Access(it->second, syntax.Position); context.Cache.emplace(id, result); return result;
        }
        auto& arena = Program.Expressions;
        TExprId result;
        switch (syntax.Kind) {
            case ESyntaxKind::Null: result = arena.Null(syntax.Position); break;
            case ESyntaxKind::Boolean: result = arena.Literal({ETypeKind::Bool}, syntax.Text == "true", syntax.Position); break;
            case ESyntaxKind::Integer: result = Integer(syntax); break;
            case ESyntaxKind::Real: {
                std::string_view literal = syntax.Text;
                const bool single = literal.ends_with('f') || literal.ends_with('F');
                if (single) literal.remove_suffix(1);
                double number;
                std::istringstream stream{std::string(literal)};
                stream.imbue(std::locale::classic());
                stream >> std::noskipws >> number;
                if (!stream || !stream.eof() || !std::isfinite(number)) throw TFrontendError(syntax.Position, "invalid floating point literal");
                result = arena.Literal({single ? ETypeKind::Float : ETypeKind::Double}, number, syntax.Position); break;
            }
            case ESyntaxKind::String: result = arena.Literal({ETypeKind::String}, syntax.Text, syntax.Position); break;
            case ESyntaxKind::Column: {
                auto column = context.Scope.Find(syntax.Text, syntax.Position);
                if (!column) throw TFrontendError(syntax.Position, "unknown or ungrouped column: " + syntax.Text);
                result = Access(*column, syntax.Position); break;
            }
            case ESyntaxKind::Variable: {
                if (context.Arguments) if (auto it = context.Arguments->find(syntax.Text); it != context.Arguments->end()) { result = it->second; break; }
                if (auto it = Values.find(syntax.Text); it != Values.end()) result = it->second;
                else if (auto it = Program.Parameters.find(syntax.Text); it != Program.Parameters.end()) result = arena.Parameter(syntax.Text, it->second, syntax.Position);
                else throw TFrontendError(syntax.Position, "unknown scalar binding: $" + syntax.Text);
                break;
            }
            case ESyntaxKind::Unary: {
                if (syntax.Text == "-" && Syntax.Expressions[syntax.Arguments[0]].Kind == ESyntaxKind::Integer) {
                    result = Integer(Syntax.Expressions[syntax.Arguments[0]], true);
                    break;
                }
                auto arg = Expr(syntax.Arguments[0], context);
                if (syntax.Text == "+") { if (!arena[arg].Type.IsNumeric()) throw TFrontendError(syntax.Position, "unary + expects a number"); result = arg; }
                else if (syntax.Text == "-") result = arena.Unary(EOpcode::Negate, arg, syntax.Position);
                else if (syntax.Text == "NOT") result = arena.Unary(EOpcode::Not, arg, syntax.Position);
                else if (syntax.Text == "IS NULL") result = arena.Unary(EOpcode::IsNull, arg, syntax.Position);
                else if (syntax.Text == "IS NOT NULL") result = arena.Unary(EOpcode::IsNotNull, arg, syntax.Position);
                else throw TUnsupportedFeature(EFeature::Expression, syntax.Position, "unsupported unary operator: " + syntax.Text);
                break;
            }
            case ESyntaxKind::Binary: {
                auto left = Expr(syntax.Arguments[0], context), right = Expr(syntax.Arguments[1], context);
                static const std::unordered_map<std::string, EOpcode> ops{
                    {"+", EOpcode::Add}, {"-", EOpcode::Subtract}, {"*", EOpcode::Multiply}, {"/", EOpcode::Divide}, {"%", EOpcode::Modulo},
                    {"=", EOpcode::Equal}, {"==", EOpcode::Equal}, {"!=", EOpcode::NotEqual}, {"<>", EOpcode::NotEqual},
                    {"<", EOpcode::Less}, {"<=", EOpcode::LessEqual}, {">", EOpcode::Greater}, {">=", EOpcode::GreaterEqual},
                    {"AND", EOpcode::And}, {"OR", EOpcode::Or}, {"||", EOpcode::Concat}};
                if (syntax.Text == "LIKE") {
                    const auto& pattern = arena[right];
                    if (pattern.Op != EOpcode::Literal || !pattern.Type.IsString() || !std::holds_alternative<std::string>(pattern.Value)) {
                        throw TUnsupportedFeature(EFeature::Expression, syntax.Position, "native LIKE requires a constant string pattern");
                    }
                    auto text = std::get<std::string>(pattern.Value);
                    const bool leading = !text.empty() && text.front() == '%';
                    const bool trailing = !text.empty() && text.back() == '%';
                    if (leading) text.erase(text.begin());
                    if (trailing && !text.empty()) text.pop_back();
                    if (text.find_first_of("%_\\") != std::string::npos) {
                        throw TUnsupportedFeature(EFeature::Expression, syntax.Position, "native LIKE supports only a literal with leading/trailing %");
                    }
                    const auto literal = arena.Literal(pattern.Type, text, syntax.Position);
                    const auto op = leading && trailing ? EOpcode::Contains : leading ? EOpcode::EndsWith : trailing ? EOpcode::StartsWith : EOpcode::Equal;
                    result = arena.Binary(op, left, literal, syntax.Position);
                }
                else if (syntax.Text == "XOR") {
                    if (arena[left].Type.Kind != ETypeKind::Bool || arena[right].Type.Kind != ETypeKind::Bool) {
                        throw TFrontendError(syntax.Position, "XOR requires Boolean operands");
                    }
                    result = arena.Binary(EOpcode::NotEqual, left, right, syntax.Position);
                }
                else if (syntax.Text == "??") { const TExprId args[]{left, right}; result = arena.Coalesce(args, syntax.Position); }
                else if (auto it = ops.find(syntax.Text); it != ops.end()) result = arena.Binary(it->second, left, right, syntax.Position);
                else throw TUnsupportedFeature(EFeature::Expression, syntax.Position, "unsupported binary operator: " + syntax.Text);
                break;
            }
            case ESyntaxKind::Cast: result = arena.Cast(Expr(syntax.Arguments[0], context), ParseType(syntax.Text, syntax.Position), syntax.Position); break;
            case ESyntaxKind::Case: {
                result = Expr(syntax.Arguments.back(), context);
                for (std::size_t i = syntax.Arguments.size() - 1; i > 0; i -= 2) {
                    auto condition = Expr(syntax.Arguments[i - 2], context), value = Expr(syntax.Arguments[i - 1], context);
                    result = arena.If(condition, value, result, syntax.Position);
                }
                break;
            }
            case ESyntaxKind::Between: {
                auto value = Expr(syntax.Arguments[0], context), low = Expr(syntax.Arguments[1], context), high = Expr(syntax.Arguments[2], context);
                auto a = arena.Binary(EOpcode::GreaterEqual, value, low, syntax.Position), b = arena.Binary(EOpcode::LessEqual, value, high, syntax.Position);
                result = arena.Binary(EOpcode::And, a, b, syntax.Position); break;
            }
            case ESyntaxKind::In: {
                if (syntax.Query) throw TUnsupportedFeature(EFeature::Subquery, syntax.Position, "IN subqueries are not supported yet");
                auto value = Expr(syntax.Arguments[0], context);
                result = arena.Literal({ETypeKind::Bool}, false, syntax.Position);
                for (std::size_t i = 1; i < syntax.Arguments.size(); ++i) {
                    auto item = Expr(syntax.Arguments[i], context), equal = arena.Binary(EOpcode::Equal, value, item, syntax.Position);
                    result = arena.Binary(EOpcode::Or, result, equal, syntax.Position);
                }
                break;
            }
            case ESyntaxKind::Call: {
                const auto name = Lower(syntax.Text);
                if (IsAggregate(name)) {
                    if (!context.Aggregation) throw TFrontendError(syntax.Position, "aggregate is not allowed in this expression");
                    result = Aggregate(syntax, Canonical[id], *context.Aggregation); break;
                }
                if (syntax.Distinct) throw TFrontendError(syntax.Position, "DISTINCT requires an aggregate function");
                std::vector<TExprId> args;
                for (auto arg : syntax.Arguments) args.push_back(Expr(arg, context));
                if (syntax.Text.starts_with('$')) {
                    auto it = Lambdas.find(syntax.Text.substr(1));
                    if (it == Lambdas.end()) throw TFrontendError(syntax.Position, "unknown scalar lambda: " + syntax.Text);
                    const auto& lambda = *it->second;
                    if (lambda.Parameters.size() != args.size()) throw TFrontendError(syntax.Position, "lambda argument count mismatch");
                    if (!ActiveLambdas.insert(&lambda).second) throw TFrontendError(syntax.Position, "recursive lambda is not supported");
                    std::unordered_map<std::string, TExprId> parameters;
                    for (std::size_t i = 0; i < args.size(); ++i) parameters.emplace(lambda.Parameters[i], args[i]);
                    const TScope empty;
                    TExprContext lambdaContext{empty, nullptr, &parameters}; result = Expr(lambda.Expression, lambdaContext);
                    ActiveLambdas.erase(&lambda);
                } else result = arena.Call(name, args, syntax.Position);
                break;
            }
            case ESyntaxKind::Star: throw TFrontendError(syntax.Position, "star is only valid in projection or COUNT(*)");
            case ESyntaxKind::ScalarSubquery: case ESyntaxKind::Exists:
                throw TUnsupportedFeature(EFeature::Subquery, syntax.Position, "scalar and EXISTS subqueries are not supported yet");
        }
        context.Cache.emplace(id, result); return result;
    }

    const TParsedQuery& Syntax;
    const TTableResolver& Catalog;
    const TBindSettings& Settings;
    TBoundProgram Program;
    std::string Prefix;
    std::vector<std::uint32_t> Canonical;
    std::vector<bool> ContainsAggregate;
    std::unordered_map<std::string, TExprId> Values;
    std::unordered_map<std::string, const TStatement*> Lambdas;
    std::unordered_map<std::string, std::shared_ptr<TSelect>> Queries;
    std::unordered_set<std::string> BoundNames;
    std::unordered_set<std::string> Declared;
    std::unordered_set<const TSelect*> ActiveQueries;
    std::unordered_set<const TStatement*> ActiveLambdas;
    std::size_t Depth = 0;
};

} // namespace

TBoundProgram Bind(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings) {
    return TBinder(syntax, catalog, settings).Run();
}

TBoundProgram Compile(std::string_view sql, const TTableResolver& catalog, const TBindSettings& bindSettings, const TParseSettings& parseSettings) {
    return Bind(Parse(sql, parseSettings), catalog, bindSettings);
}

std::string Explain(const TBoundProgram& program) {
    constexpr const char* names[]{"Unit", "Read", "Map", "Filter", "Join", "Aggregate", "Sort", "Limit", "UnionAll"};
    std::ostringstream out;
    for (std::size_t i = 0; i < program.Relations.size(); ++i) {
        const auto& relation = program.Relations[i];
        out << i << ": " << names[static_cast<unsigned>(relation.Kind)];
        if (!relation.Table.empty()) out << ' ' << relation.Table;
        out << " inputs=[";
        for (std::size_t j = 0; j < relation.Inputs.size(); ++j) out << (j ? "," : "") << relation.Inputs[j];
        out << "] columns=[";
        for (std::size_t j = 0; j < relation.Columns.size(); ++j) out << (j ? "," : "") << relation.Columns[j];
        out << "]\n";
    }
    for (std::size_t i = 0; i < program.Results.size(); ++i) {
        out << "result " << i << " root=" << program.Results[i].Root << ':';
        for (const auto& column : program.Results[i].Columns) out << ' ' << column.Name << '=' << column.Column << ':' << column.Type.ToString();
        out << '\n';
    }
    return out.str();
}

} // namespace NKikimr::NKqp::NFrontend
