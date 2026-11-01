#include "kqp_rbo_statistics.h"
#include "kqp_operator.h"
#include <ydb/core/kqp/common/kqp_yql.h>

#include <cmath>
#include <limits>
#include <utility>

namespace NKikimr {
namespace NKqp {

namespace {

using namespace NFrontend;

// Statistics operate on the reachable typed DAG, not a lowered YQL lambda.
// Only identity/optional conversions retain source-column statistics: casts
// which change values need statistics of their own.
class TNativeSelectivity {
    static constexpr TExprId Missing = std::numeric_limits<TExprId>::max();
    struct TBound {
        TExprId Literal = Missing;
        bool Inclusive = false;
    };
    struct TRange {
        TExprId Column;
        TBound Lower, Upper;
    };
    struct TEstimate {
        double True = 0.5, False = 0.5;
        bool ColumnFree = true;
        TExprId Column = Missing, Literal = Missing;
        std::optional<TRange> Range;
    };

public:
    TNativeSelectivity(const TExpressionArena& arena, const TOptimizerStatistics& stats, TNativePredicateStatistics* usage = nullptr)
        : Arena(arena), Stats(stats), Usage(usage) {}

    double Compute(TExprId root) {
        // A shared expression is estimated once, without a recursion limit or
        // allocating scratch space for unrelated roots in the query's arena.
        TVector<std::pair<TExprId, bool>> work{{root, false}};
        while (!work.empty()) {
            const auto [id, ready] = work.back();
            work.pop_back();
            if (Estimates.contains(id)) continue;
            if (!ready) {
                work.emplace_back(id, true);
                for (const auto arg : Arena[id].Arguments) {
                    if (!Estimates.contains(arg)) work.emplace_back(arg, false);
                }
            } else {
                Estimates.emplace(id, Estimate(id));
            }
        }
        if (Usage) CollectJoinKeys(root);
        return Estimates.at(root).True;
    }

private:
    void CollectJoinKeys(TExprId root) {
        TVector<TExprId> work{root};
        THashSet<TExprId> visited;
        while (!work.empty()) {
            const auto id = work.back();
            work.pop_back();
            if (!visited.insert(id).second) continue;
            const auto& node = Arena[id];
            if (node.Op == EOpcode::And) {
                work.insert(work.end(), node.Arguments.begin(), node.Arguments.end());
            } else if (node.Op == EOpcode::Coalesce) {
                const bool falseFallback = std::all_of(node.Arguments.begin() + 1, node.Arguments.end(), [&](TExprId arg) {
                    const auto literal = Estimates.at(arg).Literal;
                    if (literal == Missing) return false;
                    const auto& value = Arena[literal].Value;
                    return std::holds_alternative<std::monostate>(value) ||
                        (std::holds_alternative<bool>(value) && !std::get<bool>(value));
                });
                if (falseFallback) work.push_back(node.Arguments[0]);
            } else if ((node.Op == EOpcode::Convert || node.Op == EOpcode::Cast || node.Op == EOpcode::Unwrap) &&
                node.Type.Required() == Arena[node.Arguments[0]].Type.Required()) {
                work.push_back(node.Arguments[0]);
            } else if (node.Op == EOpcode::Equal) {
                const auto left = Estimates.at(node.Arguments[0]).Column, right = Estimates.at(node.Arguments[1]).Column;
                if (left != Missing && right != Missing)
                    Usage->JoinKeys.emplace_back(Arena[left].ColumnId, Arena[right].ColumnId);
            }
        }
    }

    template <typename T>
    std::optional<T> LiteralAs(TExprId id) const {
        if (id == Missing) return std::nullopt;
        return std::visit([](const auto& value) -> std::optional<T> {
            using V = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string_view>) {
                if constexpr (std::is_same_v<V, std::string>) return value;
            } else if constexpr (std::is_arithmetic_v<V>) {
                if constexpr (std::is_integral_v<T> && std::is_integral_v<V>) {
                    if constexpr (std::is_same_v<V, bool>) return static_cast<T>(value);
                    else if (std::in_range<T>(value)) return static_cast<T>(value);
                } else {
                    // Do not round an integer key through Double or truncate a
                    // fractional boundary into an integer histogram bucket.
                    const long double number = value;
                    if (!std::isfinite(number)) return std::nullopt;
                    if constexpr (std::is_integral_v<T>) {
                        const auto limit = std::ldexp(1.0L, std::numeric_limits<T>::digits);
                        if (number < (std::is_signed_v<T> ? -limit : 0.0L) || number >= limit || std::trunc(number) != number)
                            return std::nullopt;
                    } else if (std::abs(number) > std::numeric_limits<T>::max()) {
                        return std::nullopt;
                    }
                    const T converted = static_cast<T>(value);
                    if (static_cast<long double>(converted) == number) return converted;
                }
            }
            return std::nullopt;
        }, Arena[id].Value);
    }

    template <typename F>
    auto WithColumnType(TExprId column, F&& f) const -> decltype(f.template operator()<i64>()) {
        using K = ETypeKind;
        switch (Arena[column].Type.Kind) {
            case K::Bool: case K::Uint8: return f.template operator()<ui8>();
            case K::Int8: return f.template operator()<i8>();
            case K::Uint16: case K::Date: return f.template operator()<ui16>();
            case K::Int16: return f.template operator()<i16>();
            case K::Uint32: case K::Datetime: return f.template operator()<ui32>();
            case K::Int32: return f.template operator()<i32>();
            case K::Uint64: case K::Timestamp: return f.template operator()<ui64>();
            case K::Int64: case K::Interval: return f.template operator()<i64>();
            case K::Float: return f.template operator()<float>();
            case K::Double: return f.template operator()<double>();
            case K::String: case K::Utf8: return f.template operator()<std::string_view>();
            default: return {};
        }
    }

    double EqualityDefault(TExprId column) const {
        if (Stats.KeyColumns && Stats.KeyColumns->Data.size() == 1 &&
            Stats.KeyColumns->Data.front() == ToString(Arena[column].ColumnId))
            return 1.0 / std::max(Stats.Nrows, 1.0);
        return Stats.Nrows > 1.0 ? 0.1 : 1.0;
    }

    const TColumnStatistics* ColumnStats(TExprId column) const {
        if (!Stats.ColumnStatistics) return nullptr;
        const auto it = Stats.ColumnStatistics->Data.find(ToString(Arena[column].ColumnId));
        if (it == Stats.ColumnStatistics->Data.end() ||
            it->second.Type != TString(Arena[column].Type.Required().ToString())) return nullptr;
        return &it->second;
    }

    double Fraction(ui64 rows) const {
        return std::clamp(rows / std::max(Stats.Nrows, 1e-4), 0.0, 1.0);
    }

    std::optional<int> Compare(TExprId column, TExprId a, TExprId b) const {
        return WithColumnType(column, [&]<typename T>() -> std::optional<int> {
            const auto left = LiteralAs<T>(a), right = LiteralAs<T>(b);
            if (!left || !right) return std::nullopt;
            return *left < *right ? -1 : (*left > *right ? 1 : 0);
        });
    }

    double RangeEstimate(const TRange& range) const {
        const auto* stats = ColumnStats(range.Column);
        const auto estimate = WithColumnType(range.Column, [&]<typename T>() -> std::optional<double> {
            const auto lower = LiteralAs<T>(range.Lower.Literal), upper = LiteralAs<T>(range.Upper.Literal);
            if ((range.Lower.Literal != Missing && !lower) || (range.Upper.Literal != Missing && !upper))
                return std::nullopt;
            if (lower && upper && (*lower > *upper || (*lower == *upper && (!range.Lower.Inclusive || !range.Upper.Inclusive))))
                return 0.0;
            if (lower && upper && *lower == *upper) {
                if (Usage) Usage->CountMinColumns.Add(Arena[range.Column].ColumnId);
                if (stats && stats->CountMinSketch && Stats.Nrows > 0) {
                    if constexpr (std::is_same_v<T, std::string_view>)
                        return Fraction(stats->CountMinSketch->Probe(lower->data(), lower->size()));
                    else
                        return Fraction(stats->CountMinSketch->Probe(reinterpret_cast<const char*>(&*lower), sizeof(T)));
                }
                return EqualityDefault(range.Column);
            }
            if (!lower && !upper) return 1.0;
            if constexpr (!std::is_same_v<T, std::string_view>) {
                if (Usage) Usage->HistogramColumns.Add(Arena[range.Column].ColumnId);
                if (stats && stats->EqWidthHistogramEstimator && Stats.Nrows > 0) {
                    const auto& histogram = *stats->EqWidthHistogramEstimator;
                    if (!lower) return Fraction(range.Upper.Inclusive ? histogram.EstimateLessOrEqual(*upper) : histogram.EstimateLess(*upper));
                    if (!upper) return Fraction(range.Lower.Inclusive ? histogram.EstimateGreaterOrEqual(*lower) : histogram.EstimateGreater(*lower));
                    if (range.Lower.Inclusive)
                        return Fraction(range.Upper.Inclusive ? histogram.EstimateRangeGreaterOrEqualLessOrEqual(*lower, *upper)
                                                            : histogram.EstimateRangeGreaterOrEqualLess(*lower, *upper));
                    return Fraction(range.Upper.Inclusive ? histogram.EstimateRangeGreaterLessOrEqual(*lower, *upper)
                                                         : histogram.EstimateRangeGreaterLess(*lower, *upper));
                }
            }
            return 0.4;
        });
        return estimate.value_or(range.Lower.Literal == range.Upper.Literal ? EqualityDefault(range.Column) : 0.4);
    }

    std::optional<TRange> MergeRanges(const TRange& a, const TRange& b, bool conjunction) const {
        if (Arena[a.Column].ColumnId != Arena[b.Column].ColumnId || Arena[a.Column].Type != Arena[b.Column].Type)
            return std::nullopt;
        const auto empty = [&](const TRange& range) {
            if (range.Lower.Literal == Missing || range.Upper.Literal == Missing) return false;
            const int cmp = *Compare(range.Column, range.Lower.Literal, range.Upper.Literal);
            return cmp > 0 || (cmp == 0 && (!range.Lower.Inclusive || !range.Upper.Inclusive));
        };
        if (empty(a)) return conjunction ? a : b;
        if (empty(b)) return conjunction ? b : a;
        const auto bound = [&](TBound left, TBound right, bool lower, bool intersection) {
            if (left.Literal == Missing) return intersection ? right : left;
            if (right.Literal == Missing) return intersection ? left : right;
            const int cmp = *Compare(a.Column, left.Literal, right.Literal);
            if (cmp == 0) return TBound{left.Literal, intersection ? left.Inclusive && right.Inclusive : left.Inclusive || right.Inclusive};
            return ((cmp < 0) == (lower == intersection)) ? right : left;
        };
        if (!conjunction) {
            const auto lower = bound(a.Lower, b.Lower, true, true), upper = bound(a.Upper, b.Upper, false, true);
            if (lower.Literal != Missing && upper.Literal != Missing) {
                const int cmp = *Compare(a.Column, lower.Literal, upper.Literal);
                if (cmp > 0 || (cmp == 0 && !lower.Inclusive && !upper.Inclusive)) return std::nullopt;
            }
        }
        return TRange{a.Column, bound(a.Lower, b.Lower, true, conjunction), bound(a.Upper, b.Upper, false, conjunction)};
    }

    TEstimate Estimate(TExprId id) const {
        using O = EOpcode;
        const auto& node = Arena[id];
        TEstimate result;
        for (const auto arg : node.Arguments) result.ColumnFree &= Estimates.at(arg).ColumnFree;
        const auto child = [&](size_t i) -> const TEstimate& { return Estimates.at(node.Arguments[i]); };
        const auto probability = [&](double value) {
            result.True = value;
            result.False = 1.0 - value;
        };
        switch (node.Op) {
            case O::Literal:
                result.Literal = id;
                if (std::holds_alternative<std::monostate>(node.Value)) result.True = result.False = 0;
                else if (const auto* value = std::get_if<bool>(&node.Value)) probability(*value ? 1.0 : 0.0);
                break;
            case O::Column:
                result.Column = id;
                result.ColumnFree = false;
                break;
            case O::Convert: case O::Cast: case O::Unwrap:
                if (node.Type.Required() == Arena[node.Arguments[0]].Type.Required() ||
                    (child(0).Literal != Missing && std::holds_alternative<std::monostate>(Arena[child(0).Literal].Value)))
                    result = child(0);
                break;
            case O::Coalesce:
                if (node.Type.Kind == ETypeKind::Bool) {
                    result.True = result.False = 0;
                    double remaining = 1;
                    for (const auto arg : node.Arguments) {
                        const auto& item = Estimates.at(arg);
                        result.True += remaining * item.True;
                        result.False += remaining * item.False;
                        remaining *= std::max(0.0, 1 - item.True - item.False);
                    }
                }
                break;
            case O::Not:
                result.True = child(0).False;
                result.False = child(0).True;
                if (child(0).Range) {
                    auto range = *child(0).Range;
                    if ((range.Lower.Literal == Missing) != (range.Upper.Literal == Missing)) {
                        std::swap(range.Lower, range.Upper);
                        range.Lower.Inclusive = !range.Lower.Inclusive;
                        range.Upper.Inclusive = !range.Upper.Inclusive;
                        result.Range = range;
                        probability(RangeEstimate(range));
                    }
                }
                break;
            case O::And: case O::Or: {
                const auto& a = child(0);
                const auto& b = child(1);
                const bool conjunction = node.Op == O::And;
                if (node.Arguments[0] == node.Arguments[1]) return a;
                result.True = conjunction ? a.True * b.True : a.True + b.True - a.True * b.True;
                result.False = conjunction ? a.False + b.False - a.False * b.False : a.False * b.False;
                if (a.Range && b.Range && Arena[a.Range->Column].ColumnId == Arena[b.Range->Column].ColumnId) {
                    result.Range = MergeRanges(*a.Range, *b.Range, conjunction);
                    if (result.Range) probability(RangeEstimate(*result.Range));
                    else if (!conjunction) probability(std::min(1.0, a.True + b.True));
                }
                break;
            }
            case O::Equal: case O::NotEqual: case O::Less: case O::LessEqual: case O::Greater: case O::GreaterEqual: {
                auto left = child(0), right = child(1);
                auto op = node.Op;
                if (left.Column == Missing && right.Column != Missing) {
                    std::swap(left, right);
                    if (op == O::Less) op = O::Greater;
                    else if (op == O::LessEqual) op = O::GreaterEqual;
                    else if (op == O::Greater) op = O::Less;
                    else if (op == O::GreaterEqual) op = O::LessEqual;
                }
                if (Arena[node.Arguments[0]].Type.Kind == ETypeKind::Null || Arena[node.Arguments[1]].Type.Kind == ETypeKind::Null ||
                    (left.Literal != Missing && std::holds_alternative<std::monostate>(Arena[left.Literal].Value)) ||
                    (right.Literal != Missing && std::holds_alternative<std::monostate>(Arena[right.Literal].Value))) {
                    result.True = result.False = 0;
                } else if (left.Column != Missing && right.Column != Missing) {
                    probability(op == O::NotEqual ? 0.7 : 0.3);
                } else if (left.Column != Missing && right.ColumnFree) {
                    const bool equality = op == O::Equal || op == O::NotEqual;
                    double value = equality ? EqualityDefault(left.Column) : 0.4;
                    if (right.Literal != Missing && Compare(left.Column, right.Literal, right.Literal)) {
                        TRange range{left.Column, {}, {}};
                        if (equality || op == O::Greater || op == O::GreaterEqual)
                            range.Lower = {right.Literal, equality || op == O::GreaterEqual};
                        if (equality || op == O::Less || op == O::LessEqual)
                            range.Upper = {right.Literal, equality || op == O::LessEqual};
                        value = RangeEstimate(range);
                        if (op != O::NotEqual) result.Range = range;
                    }
                    probability(op == O::NotEqual ? 1 - value : value);
                }
                break;
            }
            case O::IsNull: case O::IsNotNull: {
                const auto type = Arena[node.Arguments[0]].Type;
                double nulls = type.Kind == ETypeKind::Null ? 1.0 : (type.Nullable ? 0.1 : 0.0);
                if (child(0).Literal != Missing)
                    nulls = std::holds_alternative<std::monostate>(Arena[child(0).Literal].Value) ? 1.0 : 0.0;
                probability(node.Op == O::IsNull ? nulls : 1 - nulls);
                break;
            }
            case O::Contains: case O::StartsWith: case O::EndsWith:
                if (child(0).Column != Missing && child(1).ColumnFree)
                    probability(node.Op == O::Contains ? 0.1 : 0.5);
                break;
            case O::If: {
                const auto& condition = child(0);
                result.True = condition.True * child(1).True + (1 - condition.True) * child(2).True;
                result.False = condition.True * child(1).False + (1 - condition.True) * child(2).False;
                break;
            }
            default: break;
        }
        return result;
    }

    const TExpressionArena& Arena;
    const TOptimizerStatistics& Stats;
    TNativePredicateStatistics* Usage;
    THashMap<TExprId, TEstimate> Estimates;
};

} // namespace

double ComputeNativeSelectivity(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root,
    const TOptimizerStatistics& stats)
{
    return TNativeSelectivity(arena, stats).Compute(root);
}

TNativePredicateStatistics CollectNativePredicateStatistics(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root) {
    TNativePredicateStatistics usage;
    const TOptimizerStatistics empty;
    TNativeSelectivity(arena, empty, &usage).Compute(root);
    return usage;
}

const TColumnLineageEntry* FindSourceStatistics(const IOperator& op, TInfoUnitId id, const TColumnLineage& lineage) {
    return op.Props.Metadata && op.Props.Metadata->SourceStatsColumns.Contains(id) ? lineage.Find(id) : nullptr;
}

TOptimizerStatistics BuildOptimizerStatistics(IOperator& op, const TColumnLineage& lineage, bool withStatsAndCosts, const NYql::TTypeAnnotationContext& typeCtx) {
    auto& props = op.Props;
    const auto& outputIUs = op.GetOutputIUs();
    TVector<TString> keyColumnNames;

    for (const auto id : props.Metadata->KeyColumns.Items()) {
        keyColumnNames.push_back(ToString(id));
    }

    const double cost = props.Cost.has_value() ? *props.Cost : 0.0;

    // Build column statistics for the set of IUs
    // Use lineage table to obtain table and column names, look up table names in the
    // type annotation context and place them in the local map. If there are multiple tables - 
    // then its a result of join or set operation, don't create column statistics
    TString table;
    THashSet<TString> attributes;

    for (const auto id : outputIUs) {
        const auto* lineageEntry = lineage.Find(id);
        if (!lineageEntry) {
            continue;
        }
        // Columns without source statistics take an empty table name, as
        // computed columns (e.g. aggregate results) have no source table.
        const TString sourceTable = props.Metadata->SourceStatsColumns.Contains(id) ? lineageEntry->TableName : TString{};
        if (table != "" && table != sourceTable) {
            attributes.clear();
            break;
        }
        table = sourceTable;
        attributes.insert(lineageEntry->ColumnName);
    }

    TIntrusivePtr<TOptimizerStatistics::TColumnStatMap> ColumnStatistics;

    THashMap<TString, TColumnStatistics> columnStatsMap;
    THashMap<TString, TMultiColumnStatistics> multiColumnStatsMap;

    if (attributes.size() && typeCtx.ColumnStatisticsByTableName.contains(table)) {
        const auto& globalStats = *typeCtx.ColumnStatisticsByTableName.at(table);
        const auto& globalMap = globalStats.Data;

        // The statistics consumer sees the same decimal IDs as expression ASTs.
        // Multiple bindings of one storage field each receive its statistics.
        THashMap<TString, TString> idByColumnName;
        for (const auto id : outputIUs) {
            const auto* source = FindSourceStatistics(op, id, lineage);
            if (!source) {
                continue;
            }
            idByColumnName[source->ColumnName] = ToString(id);
            if (const auto it = globalMap.find(source->ColumnName); it != globalMap.end()) {
                columnStatsMap.emplace(ToString(id), it->second);
            }
        }

        for (const auto& [_, multiColumnStats] : globalStats.MultiData) {
            TVector<TString> translatedColumns;
            for (const auto& column : multiColumnStats.Columns) {
                const auto it = idByColumnName.find(column);
                if (it == idByColumnName.end()) {
                    translatedColumns.clear();
                    break;
                }
                translatedColumns.push_back(it->second);
            }

            if (translatedColumns.empty()) {
                continue;
            }

            TMultiColumnStatistics translated(multiColumnStats);
            translated.Columns = translatedColumns;
            multiColumnStatsMap[MakeMultiColumnKey(translatedColumns)] = std::move(translated);
        }

        if (columnStatsMap.size() || multiColumnStatsMap.size()) {
            ColumnStatistics = MakeIntrusive<TOptimizerStatistics::TColumnStatMap>(
                std::move(columnStatsMap), std::move(multiColumnStatsMap));
        }
    }

    TOptimizerStatistics stats(props.Metadata->Type,
        withStatsAndCosts ? props.Statistics->ERows : 0.0,
        props.Metadata->ColumnsCount,
        withStatsAndCosts ? props.Statistics->EBytes : 0.0,
        withStatsAndCosts ? cost : 0.0,
        TIntrusivePtr<TOptimizerStatistics::TKeyColumns>(
            new TOptimizerStatistics::TKeyColumns(keyColumnNames)),
        ColumnStatistics
        );

    if (withStatsAndCosts && props.Statistics.has_value()) {
        stats.Selectivity = props.Statistics->Selectivity;
    }

    return stats;
}

TString TRBOMetadata::ToString(ui32 printOptions, const TInfoUnitRegistry& registry) {
    TStringBuilder builder;

    if (printOptions & (EPrintPlanOptions::PrintBasicMetadata | EPrintPlanOptions::PrintFullMetadata)) {
        TString metadataType;

        switch (Type) {
            case EStatisticsType::BaseTable:
                metadataType = "BaseTable";
                break;
            case EStatisticsType::FilteredFactTable:
                metadataType = "FilteredFactTable";
                break;
            case EStatisticsType::ManyManyJoin:
                metadataType = "ManyManyJoin";
                break;
            case EStatisticsType::Constant:
                metadataType = "Constant";
                break;
        default:
            Y_ENSURE(false,"Unknown EStatisticsType");
        }

        builder << "Type: " << metadataType;

        TString storageType;

        switch(StorageType) {
            case EStorageType::RowStorage:
                storageType = "Row";
                break;
            case EStorageType::ColumnStorage:
                storageType = "Column";
                break;
            case EStorageType::NA:
                storageType = "N/A";
                break;
        }

        builder << ", ColumnsCount: " << ColumnsCount << ", Storage: " << storageType << ", KeyCols: [";

        for (size_t i = 0; i < KeyColumns.Items().size(); i++) {
            builder << registry.GetDebugName(KeyColumns.Items()[i]);
            if (i != KeyColumns.Items().size() - 1) {
                builder << ", ";
            }
        }

        builder << "]";
    }

    return builder;
}

TString TRBOStatistics::ToString(ui32 printOptions) {
    Y_UNUSED(printOptions);
    return TStringBuilder() << "N records: " << ERows << ", Size: " << EBytes << ", Selectivity: " << Selectivity;
}

}
}
