#include "frontend.h"

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

// Resolve each global reference to its defining statement once. Closures keep
// these IDs when later statements shadow a name; expanding a lambda or query
// never copies an environment or re-resolves its free names.
class TGlobalBindings {
public:
    static constexpr std::uint32_t Unknown = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t Parameter = Unknown - 1;
    static constexpr std::uint32_t Argument = Unknown - 2;

    TGlobalBindings(const TParsedQuery& syntax, const TBindSettings& settings)
        : Syntax(syntax), Settings(settings), References(syntax.Expressions.size(), Unknown), Seen(syntax.Expressions.size()) {
        for (const auto& [name, type] : settings.ParameterTypes) Names[name] = Parameter;
        for (std::uint32_t i = 0; i < syntax.Statements.size(); ++i) {
            const auto& statement = syntax.Statements[i];
            Arguments.clear();
            if (statement.Lambda) for (const auto& name : statement.Parameters) {
                if (!Arguments.insert(name).second) throw TFrontendError(statement.Position, "duplicate lambda parameter");
            }
            Expression(statement.Expression);
            if (statement.Query) Select(*statement.Query);
            if (statement.Kind == EStatementKind::Declaration) Names[statement.Name] = Parameter;
            if (statement.Kind == EStatementKind::Binding) Names[statement.Name] = i;
        }
    }

    std::uint32_t operator[](TSyntaxId expression) const { return References.at(expression); }
    bool IsReferenced(TSyntaxId expression) const { return Seen.at(expression); }
    std::shared_ptr<TSelect> Query(const TSource& source) const {
        if (source.Query || !source.Name.starts_with('$')) return source.Query;
        const auto binding = Sources.at(&source);
        if (binding >= Syntax.Statements.size()) throw TFrontendError(source.Position, "unknown query binding: " + source.Name);
        const auto& statement = Syntax.Statements[binding];
        if (!statement.Query) throw TUnsupportedFeature(EFeature::Statement, source.Position, "native sources require a named query: " + source.Name);
        return statement.Query;
    }

private:
    std::uint32_t Resolve(const std::string& name) const {
        if (Arguments.contains(name)) return Argument;
        const auto found = Names.find(name);
        return found == Names.end() ? Unknown : found->second;
    }
    void Expression(TSyntaxId root) {
        if (root == NoSyntax || Seen.at(root)) return;
        std::vector<TSyntaxId> pending{root};
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (Seen.at(id)) continue;
            Seen[id] = true;
            const auto& node = Syntax.Expressions[id];
            if (node.Kind == ESyntaxKind::Variable) References[id] = Resolve(node.Text);
            if (node.Kind == ESyntaxKind::Call && node.Text.starts_with('$')) References[id] = Resolve(node.Text.substr(1));
            pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
            if (node.Query) Select(*node.Query);
        }
    }
    void Source(const TSource& source) {
        if (source.Query) Select(*source.Query);
        else if (source.Name.starts_with('$')) Sources.emplace(&source, Resolve(source.Name.substr(1)));
    }
    void Select(const TSelect& query) {
        if (++Depth > Settings.MaxExpansionDepth) throw TResourceLimitError(query.Position, "binding scope depth limit exceeded");
        for (const auto& [name, cte] : query.Ctes) Select(*cte);
        if (query.Source) Source(*query.Source);
        for (const auto& join : query.Joins) { Source(join.Source); Expression(join.Predicate); }
        for (const auto& item : query.Projection) Expression(item.Expression);
        Expression(query.Where);
        for (const auto& item : query.GroupBy) Expression(item.Expression);
        Expression(query.Having);
        for (const auto& item : query.OrderBy) Expression(item.Expression);
        Expression(query.Limit);
        Expression(query.Offset);
        for (const auto& branch : query.UnionAll) Select(*branch);
        --Depth;
    }

    const TParsedQuery& Syntax;
    const TBindSettings& Settings;
    std::vector<std::uint32_t> References;
    std::vector<bool> Seen;
    std::unordered_map<std::string, std::uint32_t> Names;
    std::unordered_set<std::string> Arguments;
    std::unordered_map<const TSource*, std::uint32_t> Sources;
    std::size_t Depth = 0;
};

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
    std::uint32_t Binding;
    bool operator==(const TSyntaxKey&) const = default;
};

struct TSyntaxHash {
    std::size_t operator()(const TSyntaxKey& key) const {
        std::size_t value = std::hash<std::string>{}(key.Text) ^ static_cast<unsigned>(key.Kind) ^ key.Distinct ^ key.Binding;
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
        : Syntax(syntax), Catalog(catalog), Settings(settings), Bindings(syntax, settings), Prefix(settings.TablePathPrefix), Values(syntax.Statements.size())
    {
        Program.Parameters = settings.ParameterTypes;
        Canonical.reserve(syntax.Expressions.size());
        ContainsAggregate.reserve(syntax.Expressions.size());
        std::unordered_map<TSyntaxKey, std::uint32_t, TSyntaxHash> interned;
        for (std::size_t i = 0; i < syntax.Expressions.size(); ++i) {
            const auto& expr = syntax.Expressions[i];
            const auto normalized = (expr.Kind == ESyntaxKind::Call && !expr.Text.starts_with('$')) || expr.Kind == ESyntaxKind::Cast;
            if (Bindings.IsReferenced(i) && (expr.Kind == ESyntaxKind::Variable || (expr.Kind == ESyntaxKind::Call && expr.Text.starts_with('$'))) &&
                Bindings[i] == TGlobalBindings::Unknown)
                throw TFrontendError(expr.Position, "unknown binding: " + expr.Text);
            TSyntaxKey key{expr.Kind, normalized ? Lower(expr.Text) : expr.Text, {}, expr.Distinct, Bindings[i]};
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
        for (std::size_t index = 0; index < Syntax.Statements.size(); ++index) {
            const auto& statement = Syntax.Statements[index];
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
                    if (!statement.Query && !statement.Lambda) {
                        const TScope empty;
                        TExprContext context{empty};
                        Values[index] = Expr(statement.Expression, context);
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
        auto query = Bindings.Query(source);
        if (!query) if (const auto named = Queries.find(source.Name); named != Queries.end()) query = named->second;
        if (query) {
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
                const auto binding = Bindings[id];
                if (binding == TGlobalBindings::Argument) {
                    if (!context.Arguments || !context.Arguments->contains(syntax.Text))
                        throw TUnsupportedFeature(EFeature::Function, syntax.Position, "lambda argument is not a scalar value: " + syntax.Text);
                    result = context.Arguments->at(syntax.Text);
                } else if (binding == TGlobalBindings::Parameter) {
                    result = arena.Parameter(syntax.Text, Program.Parameters.at(syntax.Text), syntax.Position);
                } else if (binding < Values.size() && Values[binding]) result = *Values[binding];
                else throw TUnsupportedFeature(EFeature::Expression, syntax.Position, "binding is not a scalar value: $" + syntax.Text);
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
                    const auto binding = Bindings[id];
                    if (binding >= Syntax.Statements.size() || !Syntax.Statements[binding].Lambda)
                        throw TUnsupportedFeature(EFeature::Function, syntax.Position, "native calls require a named scalar lambda: " + syntax.Text);
                    const auto& lambda = Syntax.Statements[binding];
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
    TGlobalBindings Bindings;
    TBoundProgram Program;
    std::string Prefix;
    std::vector<std::uint32_t> Canonical;
    std::vector<bool> ContainsAggregate;
    std::vector<std::optional<TExprId>> Values;
    std::unordered_map<std::string, std::shared_ptr<TSelect>> Queries;
    std::unordered_set<std::string> Declared;
    std::unordered_set<const TSelect*> ActiveQueries;
    std::unordered_set<const TStatement*> ActiveLambdas;
    std::size_t Depth = 0;
};

} // namespace

TBoundProgram NDetail::Bind(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings) {
    return TBinder(syntax, catalog, settings).Run();
}

TBoundProgram NDetail::Compile(std::string_view sql, const TTableResolver& catalog, const TBindSettings& bindSettings, const TParseSettings& parseSettings) {
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

namespace {

void CheckType(TType type, TSourcePosition pos) {
    if (type.Kind == ETypeKind::Decimal) {
        throw TUnsupportedFeature(EFeature::Type, pos,
            "Decimal is outside native-read-v1: finite-only arithmetic does not implement YQL special values");
    }
}

void CheckSyntax(const TParsedQuery& syntax) {
    for (const auto& node : syntax.Expressions) {
        if (node.Kind == ESyntaxKind::Case && node.Text == "implicit_else")
            throw TUnsupportedFeature(EFeature::Expression, node.Position, "CASE requires explicit ELSE in native-read-v1");
        if (node.Kind == ESyntaxKind::Real && (node.Text.ends_with('p') || node.Text.ends_with('P')))
            throw TUnsupportedFeature(EFeature::Type, node.Position, "PostgreSQL numeric literals are outside native-read-v1");
        if (node.Kind == ESyntaxKind::Column && node.Text.substr(node.Text.find_last_of('.') + 1).starts_with("_yql_"))
            throw TUnsupportedFeature(EFeature::Backend, node.Position, "system columns are outside native-read-v1");
        if (node.Kind == ESyntaxKind::Binary &&
            (node.Text == "+" || node.Text == "-" || node.Text == "*" || node.Text == "/" || node.Text == "%" || node.Text == "||")) {
            for (auto arg : node.Arguments) if (syntax.Expressions[arg].Kind == ESyntaxKind::Null)
                throw TUnsupportedFeature(EFeature::Expression, node.Position, "bare NULL arithmetic is outside native-read-v1");
        }
        if (node.Kind == ESyntaxKind::Call) {
            const auto name = Lower(node.Text);
            // These convenience aliases in the experimental expression API do
            // not have the same meaning (or do not exist) in YQL SQL.
            if (name == "lower" || name == "upper" || name == "substr" || name == "contains" ||
                name == "string::tolower" || name == "string::toupper" || name == "concat")
                throw TUnsupportedFeature(EFeature::Function, node.Position, "function is outside the compatible native profile: " + node.Text);
        }
        if (node.Kind == ESyntaxKind::Cast) {
            const auto target = ParseType(node.Text, node.Position);
            if (!target.IsIntegral() && !target.IsString() && target.Kind != ETypeKind::Bool &&
                target.Kind != ETypeKind::Float && target.Kind != ETypeKind::Double)
                throw TUnsupportedFeature(EFeature::Expression, node.Position, "explicit cast target is outside the native scalar profile");
        }
    }
}

void CheckProgram(const TBoundProgram& program) {
    for (const auto& column : program.Columns) CheckType(column.Type, {});
    for (const auto& [name, type] : program.Parameters) CheckType(type, {});
    for (const auto& node : program.Expressions.Nodes()) {
        CheckType(node.Type, node.Position);
        if ((node.Op >= EOpcode::Add && node.Op <= EOpcode::Modulo) || node.Op == EOpcode::Concat) {
            for (auto id : node.Arguments) {
                while (program.Expressions[id].Op == EOpcode::Convert) id = program.Expressions[id].Arguments[0];
                if (program.Expressions[id].Type.Kind == ETypeKind::Null)
                    throw TUnsupportedFeature(EFeature::Expression, node.Position, "bare NULL arithmetic is outside native-read-v1");
            }
        }
        if (node.Op == EOpcode::Convert && !node.Arguments.empty()) {
            const auto from = program.Expressions[node.Arguments[0]].Type;
            if ((node.Type.Kind == ETypeKind::Double && from.Kind == ETypeKind::Float) ||
                (node.Type.Kind == ETypeKind::Float && from.Kind != ETypeKind::Float))
                throw TUnsupportedFeature(EFeature::Type, node.Position, "mixed Float promotion is outside native-read-v1");
            if (from.IsIntegral() && node.Type.IsIntegral() && from.IsSigned() != node.Type.IsSigned())
                throw TUnsupportedFeature(EFeature::Type, node.Position, "implicit signed/unsigned promotion is outside native-read-v1");
        }
        if (node.Op == EOpcode::Cast) {
            const auto from = program.Expressions[node.Arguments[0]].Type;
            if (!from.IsIntegral() && !from.IsString() && from.Kind != ETypeKind::Null &&
                from.Kind != ETypeKind::Bool && from.Kind != ETypeKind::Float && from.Kind != ETypeKind::Double)
                throw TUnsupportedFeature(EFeature::Expression, node.Position, "runtime cast input is outside the native scalar profile");
        }
        if ((node.Op == EOpcode::Lower || node.Op == EOpcode::Upper) && node.Type.Kind != ETypeKind::String)
            throw TUnsupportedFeature(EFeature::Function, node.Position, "native ASCII case functions require String input");
        if (node.Op == EOpcode::Cast && node.Type.IsString()) {
            const auto input = program.Expressions[node.Arguments[0]].Type;
            if (!input.IsString() && !input.IsIntegral() && input.Kind != ETypeKind::Bool) {
                throw TUnsupportedFeature(EFeature::Expression, node.Position,
                    "native-read-v1 does not implement this type's YQL text formatting");
            }
        }
    }
    for (const auto& relation : program.Relations) {
        if (relation.Kind == ERelation::Aggregate && relation.Aggregates.size() > 1) {
            for (const auto& agg : relation.Aggregates) if (agg.Distinct) {
                throw TUnsupportedFeature(EFeature::AggregateCombination, relation.Position,
                    "native RBO supports a DISTINCT aggregate only when it is the sole aggregate");
            }
        }
    }
}

class TTableCollector {
public:
    TTableCollector(const TParsedQuery& syntax, const TBindSettings& settings)
        : Syntax(syntax), Settings(settings), Bindings(syntax, settings), Prefix(settings.TablePathPrefix) {}
    std::vector<TTableRequest> Run() {
        for (const auto& statement : Syntax.Statements) {
            if (statement.Kind == EStatementKind::Pragma && Lower(statement.Name) == "tablepathprefix") Prefix = statement.Value;
            if (statement.Kind == EStatementKind::Query) Select(*statement.Query);
        }
        return std::move(Tables);
    }
private:
    void Source(const TSource& source) {
        auto query = Bindings.Query(source);
        if (!query) if (const auto it = Queries.find(source.Name); it != Queries.end()) query = it->second;
        if (query) {
            if (!Active.insert(query.get()).second) throw TFrontendError(source.Position, "recursive query binding");
            Select(*query);
            Active.erase(query.get());
        } else if (!source.Name.starts_with('$')) {
            auto path = source.Name;
            if (!Prefix.empty() && !path.starts_with('/')) path = Prefix + (Prefix.ends_with('/') ? "" : "/") + path;
            if (Seen.insert(path).second) Tables.push_back({std::move(path), source.Position});
        }
    }
    void Select(const TSelect& query) {
        if (++Depth > Settings.MaxExpansionDepth || ++Nodes > Settings.MaxNodes)
            throw TResourceLimitError(query.Position, "table discovery expansion limit exceeded");
        auto saved = Queries;
        for (const auto& [name, select] : query.Ctes) Queries[name] = select;
        if (query.Source) Source(*query.Source);
        for (const auto& join : query.Joins) Source(join.Source);
        for (const auto& branch : query.UnionAll) Select(*branch);
        Queries = std::move(saved);
        --Depth;
    }
    const TParsedQuery& Syntax;
    const TBindSettings& Settings;
    TGlobalBindings Bindings;
    std::string Prefix;
    std::unordered_map<std::string, std::shared_ptr<TSelect>> Queries;
    std::unordered_set<const TSelect*> Active;
    std::unordered_set<std::string> Seen;
    std::vector<TTableRequest> Tables;
    std::size_t Depth = 0, Nodes = 0;
};

} // namespace

TParsedQuery ParseNative(std::string_view sql, const TCompilerSettings& settings) {
    auto parsing = settings.Parsing;
    // Translation settings apply only to the leading directive-comment lines.
    // Ordinary comments or the first SQL token end this prefix. A string value
    // containing "--!" never changes language settings.
    auto prefix = sql;
    std::uint32_t line = 1;
    while (!prefix.empty()) {
        const auto end = prefix.find_first_of("\r\n");
        auto text = prefix.substr(0, end);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
        if (!text.empty()) {
            if (!text.starts_with("--!")) break;
            text.remove_prefix(3);
            const TSourcePosition pos{sql.size() - prefix.size(), line, 1};
            if (text.find_first_of(" \t") != text.npos) throw TFrontendError(pos, "bad translation settings format");
            if (text == "ansi_lexer") parsing.AnsiQuotes = true;
            else if (text.empty() || text == "syntax_v1" || text == "antlr4_parser") {}
            else if (text.starts_with("syntax_"))
                throw TUnsupportedFeature(EFeature::LanguageVersion, pos, "native frontend supports SQL v1");
            else throw TFrontendError(pos, "unknown SQL translation setting: " + std::string(text));
        }
        if (end == prefix.npos) break;
        prefix.remove_prefix(end + 1);
        ++line;
    }
    if (settings.Parser == EParser::Handwritten) return Parse(sql, parsing);
    if (settings.Parser == EParser::Fast) {
        try {
            return Parse(sql, parsing);
        } catch (const TResourceLimitError&) {
            throw;
        } catch (const TFrontendError&) {
            // This is parser dispatch, not optimizer fallback. The grammar
            // parser supplies authoritative rejection/unsupported diagnostics.
            return ParseGrammar(sql, parsing);
        }
    }
    auto grammar = ParseGrammar(sql, parsing);
    if (settings.Parser == EParser::Verify) {
        auto handwritten = Parse(sql, parsing);
        if (SyntaxFingerprint(grammar) != SyntaxFingerprint(handwritten))
            throw std::logic_error("native parser differential mismatch");
        return handwritten;
    }
    return grammar;
}

std::vector<TTableRequest> CollectTables(const TParsedQuery& syntax, const TBindSettings& settings) {
    return TTableCollector(syntax, settings).Run();
}

TCompiledProgram BindNative(const TParsedQuery& syntax, const TTableResolver& catalog, const TBindSettings& settings) {
    CheckSyntax(syntax);
    auto program = NDetail::Bind(syntax, [&](std::string_view name, TSourcePosition pos) {
        auto table = catalog(name, pos);
        for (const auto& column : table.Columns) CheckType(column.Type, pos);
        return table;
    }, settings);
    CheckProgram(program);
    return TCompiledProgram(std::move(program));
}

TCompileResult CompileNative(std::string_view sql, const TTableResolver& catalog, const TCompilerSettings& settings) {
    if (settings.Mode == ECompilerMode::Legacy) return {};
    try {
        return {BindNative(ParseNative(sql, settings), catalog, settings.Binding), {}};
    } catch (const TUnsupportedFeature& error) {
        if (settings.Mode == ECompilerMode::NativeRequired) throw;
        return {{}, TUnsupported{error.Feature, error.Position, error.what()}};
    }
}

std::string_view SupportManifest() {
    return R"({"profile":"native-read-v1","default_parser":"fast","parser_dispatch":"handwritten subset; shared-grammar-antlr4 for unhandled syntax",
"statements":["select","declare","scalar-binding","query-binding","scalar-lambda"],
"relations":["read","project","filter","on-join","aggregate","sort","limit","union-all"],
"types":["Bool","Int8","Int16","Int32","Int64","Uint8","Uint16","Uint32","Uint64","Float","Double","String","Utf8","Date","Datetime","Timestamp","Interval","Null","single-optional"],
"aggregates":["count","sum","avg","min","max","some"],
"pragmas":["TablePathPrefix","YqlSelect=force","AnsiImplicitCrossJoin"],
"restrictions":["one DISTINCT aggregate, alone","computed GROUP BY keys require aliases","CASE requires ELSE","no stars over joins","constant simple LIKE pattern","casts among integers, Bool, Float/Double and String/Utf8; no Float to 32/64-bit integer or dynamic text parsing","unsigned dynamic LIMIT/OFFSET","no implicit signed/unsigned or mixed Float promotion","no bare NULL arithmetic; dynamic Substring offsets require Uint8/16/32","no UNION ordering","no windows, subqueries, JOIN USING, grouping sets, DML, DDL, UDFs, containers, PostgreSQL or Decimal","no floating or temporal text formatting","no string literals as identifiers, system columns, typed string suffixes or extended string escapes"],
"fallback":"whole query to legacy YQL optimizer, only on structured unsupported feature",
"status":"experimental; grammar recognition exceeds execution coverage"})";
}

} // namespace NKikimr::NKqp::NFrontend
