#include "kqp_rbo_statistics.h"
#include "kqp_operator.h"
#include <ydb/core/kqp/common/kqp_yql.h>

#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace NKikimr {
namespace NKqp {

namespace {

using namespace NFrontend;

// Statistics operate on the reachable typed DAG, not a lowered YQL lambda.
// Scalar inspection preserves the established selectivity policy. Native
// bounds probe typed storage statistics without generating YQL expressions.
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
    struct TMetadata {
        bool ColumnFree = true;
        bool HasParameters = false;
        TExprId Column = Missing, Literal = Missing;
    };
    struct TEstimate {
        double Value = 1.0;
        bool Leaf = false;
        ui64 Key = 0;
        TRange Range{Missing, {}, {}};
    };
    static constexpr ui64 EmptyAttribute = ui64{1} << 32;
    static constexpr ui64 NonConstant = EmptyAttribute + 1;
    static constexpr ui64 Parameter = EmptyAttribute + 2;
    static constexpr ui64 NoAttribute = EmptyAttribute + 3;
    static constexpr ui64 RegexAttribute = EmptyAttribute + 4;

public:
    TNativeSelectivity(const TExpressionArena& arena, const TOptimizerStatistics& stats, TNativePredicateStatistics* usage = nullptr)
        : Arena(arena), Stats(stats), Usage(usage) {}

    double Compute(TExprId root) {
        // Scalar metadata is computed once per reachable DAG node. Boolean
        // chains are flattened only when consumed, rather than copying a map
        // of column ranges through every intermediate binary AND/OR node.
        TVector<std::pair<TExprId, bool>> nodes{{root, false}};
        while (!nodes.empty()) {
            const auto [id, ready] = nodes.back(); nodes.pop_back();
            if (Metadata.contains(id)) continue;
            const auto& node = Arena[id];
            if (!ready) {
                nodes.emplace_back(id, true);
                for (const auto arg : node.Arguments) if (!Metadata.contains(arg)) nodes.emplace_back(arg, false);
                continue;
            }
            TMetadata result;
            for (const auto arg : node.Arguments) {
                result.ColumnFree &= Metadata.at(arg).ColumnFree;
                result.HasParameters |= Metadata.at(arg).HasParameters;
            }
            if (node.Op == EOpcode::Column) { result.Column = id; result.ColumnFree = false; }
            else if (node.Op == EOpcode::Literal) result.Literal = id;
            else if (node.Op == EOpcode::Parameter) result.HasParameters = true;
            else if (node.Op == EOpcode::Convert || node.Op == EOpcode::Cast || node.Op == EOpcode::Unwrap) {
                const auto& input = Metadata.at(node.Arguments[0]);
                // Attribute recognition follows the existing selectivity policy.
                // Literal probes still use the original storage width and reject
                // an unrepresentable boundary instead of truncating its bytes.
                result.Column = input.Column;
                if (node.Type.Required() == Arena[node.Arguments[0]].Type.Required() ||
                    (input.Literal != Missing && std::holds_alternative<std::monostate>(Arena[input.Literal].Value)))
                    result.Literal = input.Literal;
            }
            Metadata.emplace(id, result);
        }
        const auto key = Normalize(root, false);
        TVector<std::pair<ui64, bool>> work{{key, false}};
        while (!work.empty()) {
            const auto [current, ready] = work.back(); work.pop_back();
            if (Estimates.contains(current)) continue;
            const auto id = Node(current);
            const auto op = Arena[id].Op;
            if (op != EOpcode::And && op != EOpcode::Or) {
                Estimates.emplace(current, EstimateLeaf(id, Negated(current)));
                continue;
            }
            if (!ready) {
                auto operands = LogicalOperands(current);
                work.emplace_back(current, true);
                for (auto [child, count] : operands) {
                    Y_UNUSED(count);
                    if (!Estimates.contains(child)) work.emplace_back(child, false);
                }
                Operands.emplace(current, std::move(operands));
            } else {
                Estimates.emplace(current, EstimateLogical(current));
            }
        }
        if (Usage) CollectJoinKeys(root);
        const auto& result = Estimates.at(key);
        return result ? std::min(1.0, result->Value) : 1.0;
    }

private:
    static TExprId Node(ui64 key) { return key >> 1; }
    static bool Negated(ui64 key) { return key & 1; }

    ui64 Normalize(TExprId id, bool negative) const {
        for (;;) {
            const auto& node = Arena[id];
            if (node.Op == EOpcode::Not) { negative = !negative; id = node.Arguments[0]; }
            else if (node.Op == EOpcode::Convert && node.Type.Kind == ETypeKind::Bool &&
                node.Type.Required() == Arena[node.Arguments[0]].Type.Required()) id = node.Arguments[0];
            else if (node.Op == EOpcode::Coalesce && node.Type.Kind == ETypeKind::Bool) {
                // The legacy estimator consumes the predicate of COALESCE.
                // Constant NULL alternatives have already folded on real plans;
                // recognize them here as well for standalone scalar estimates.
                size_t at = 0;
                while (at + 1 < node.Arguments.size()) {
                    const auto literal = Metadata.at(node.Arguments[at]).Literal;
                    if (literal == Missing || !std::holds_alternative<std::monostate>(Arena[literal].Value)) break;
                    ++at;
                }
                id = node.Arguments[at];
            } else break;
        }
        return (ui64{id} << 1) | negative;
    }

    bool Conjunction(ui64 key) const { return (Arena[Node(key)].Op == EOpcode::And) != Negated(key); }

    TVector<std::pair<ui64, ui64>> LogicalOperands(ui64 root) const {
        // Parents have larger IDs than their operands. Propagate occurrence
        // counts in that order so shared Boolean DAGs remain bounded without
        // dropping repeated predicates from the established grouping policy.
        std::map<ui64, ui64, std::greater<ui64>> work{{root, 1}};
        TVector<std::pair<ui64, ui64>> result;
        const bool conjunction = Conjunction(root);
        while (!work.empty()) {
            const auto [key, count] = *work.begin(); work.erase(work.begin());
            const auto& node = Arena[Node(key)];
            if ((node.Op == EOpcode::And || node.Op == EOpcode::Or) && Conjunction(key) == conjunction) {
                for (auto arg : node.Arguments) {
                    auto& occurrences = work[Normalize(arg, Negated(key))];
                    occurrences += std::min(count, std::numeric_limits<ui64>::max() - occurrences);
                }
            } else result.emplace_back(key, count);
        }
        return result;
    }

    void CollectJoinKeys(TExprId root) {
        TVector<TExprId> work{root};
        THashSet<TExprId> visited;
        const auto column = [&](TExprId id) {
            while ((Arena[id].Op == EOpcode::Convert || Arena[id].Op == EOpcode::Cast || Arena[id].Op == EOpcode::Unwrap) &&
                Arena[id].Type.Required() == Arena[Arena[id].Arguments[0]].Type.Required()) id = Arena[id].Arguments[0];
            return Arena[id].Op == EOpcode::Column ? id : Missing;
        };
        while (!work.empty()) {
            const auto id = work.back(); work.pop_back();
            if (!visited.insert(id).second) continue;
            const auto& node = Arena[id];
            if (node.Op == EOpcode::And) work.insert(work.end(), node.Arguments.begin(), node.Arguments.end());
            else if (node.Op == EOpcode::Coalesce) {
                const bool falseFallback = std::all_of(node.Arguments.begin() + 1, node.Arguments.end(), [&](TExprId arg) {
                    const auto literal = Metadata.at(arg).Literal;
                    if (literal == Missing) return false;
                    const auto& value = Arena[literal].Value;
                    return std::holds_alternative<std::monostate>(value) ||
                        (std::holds_alternative<bool>(value) && !std::get<bool>(value));
                });
                if (falseFallback) work.push_back(node.Arguments[0]);
            } else if ((node.Op == EOpcode::Convert || node.Op == EOpcode::Cast || node.Op == EOpcode::Unwrap) &&
                node.Type.Required() == Arena[node.Arguments[0]].Type.Required()) work.push_back(node.Arguments[0]);
            else if (node.Op == EOpcode::Equal) {
                const auto left = column(node.Arguments[0]), right = column(node.Arguments[1]);
                if (left != Missing && right != Missing) Usage->JoinKeys.emplace_back(Arena[left].ColumnId, Arena[right].ColumnId);
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

    double RangeEstimate(const TRange& range, bool recomputed = false) const {
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
            if (recomputed && (!Stats.ColumnStatistics || !Stats.Nrows)) {
                if constexpr (!std::is_same_v<T, std::string_view>)
                    if (Usage) Usage->HistogramColumns.Add(Arena[range.Column].ColumnId);
                return EqualityDefault(range.Column);
            }
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
                if (cmp > 0 || (cmp == 0 && (!lower.Inclusive || !upper.Inclusive))) return std::nullopt;
            }
        }
        return TRange{a.Column, bound(a.Lower, b.Lower, true, conjunction), bound(a.Upper, b.Upper, false, conjunction)};
    }

    double EstimateGroup(TVector<TEstimate> group, bool conjunction) const {
        if (group.size() == 1) return group.front().Value;
        if (conjunction) {
            auto range = group.front().Range;
            for (size_t i = 1; i < group.size(); ++i) {
                if (range.Column == Missing) range.Column = group[i].Range.Column;
                auto other = group[i].Range;
                if (other.Column == Missing) other.Column = range.Column;
                if (range.Column == Missing) continue;
                const auto merged = MergeRanges(range, other, true);
                Y_ENSURE(merged, "Conjuncts grouped by one source column must have compatible ranges");
                range = *merged;
            }
            return range.Column == Missing ? 1.0 : RangeEstimate(range, true);
        }
        // The existing policy unions only direct leaf ranges for one column.
        // Nested conjunctions contribute their independent estimates instead of
        // being promoted into an interval and merged through an enclosing OR.
        std::sort(group.begin(), group.end(), [&](const TEstimate& a, const TEstimate& b) {
            const auto left = a.Range.Lower, right = b.Range.Lower;
            if (left.Literal == Missing) return right.Literal != Missing;
            if (right.Literal == Missing) return false;
            const auto cmp = *Compare(a.Range.Column, left.Literal, right.Literal);
            return cmp ? cmp < 0 : left.Inclusive && !right.Inclusive;
        });
        double value = 0;
        auto range = group.front().Range;
        for (size_t i = 1; i < group.size(); ++i) {
            if (range.Column == Missing) range.Column = group[i].Range.Column;
            auto other = group[i].Range;
            if (other.Column == Missing) other.Column = range.Column;
            if (range.Column == Missing) continue;
            if (auto merged = MergeRanges(range, other, false)) range = *merged;
            else { value += RangeEstimate(range, true); range = other; }
        }
        return std::min(1.0, value + (range.Column == Missing ? 1.0 : RangeEstimate(range, true)));
    }

    TEstimate EstimateLogical(ui64 key) const {
        const bool conjunction = Conjunction(key);
        double value = conjunction ? 1.0 : 0.0;
        THashMap<ui64, TVector<TEstimate>> groups;
        const auto combine = [&](double item) { if (conjunction) value *= item; else value += item; };
        for (auto [child, count] : Operands.at(key)) {
            const auto& estimate = Estimates.at(child);
            if (!estimate) continue;
            if (estimate->Leaf) {
                auto& group = groups[estimate->Key];
                group.push_back(*estimate);
                // Two identical ranges already trigger recomputation; more
                // occurrences do not change their intersection or union.
                if (count > 1) group.push_back(*estimate);
            } else combine(conjunction ? std::pow(estimate->Value, count) : estimate->Value * count);
        }
        for (auto& [column, group] : groups) { Y_UNUSED(column); combine(EstimateGroup(std::move(group), conjunction)); }
        return {conjunction ? value : std::min(1.0, value)};
    }

    static EOpcode SwapComparison(EOpcode op) {
        using O = EOpcode;
        switch (op) {
            case O::Less: return O::Greater;
            case O::LessEqual: return O::GreaterEqual;
            case O::Greater: return O::Less;
            case O::GreaterEqual: return O::LessEqual;
            default: return op;
        }
    }

    static EOpcode NegateComparison(EOpcode op) {
        using O = EOpcode;
        switch (op) {
            case O::Equal: return O::NotEqual;
            case O::NotEqual: return O::Equal;
            case O::Less: return O::GreaterEqual;
            case O::LessEqual: return O::Greater;
            case O::Greater: return O::LessEqual;
            case O::GreaterEqual: return O::Less;
            default: Y_UNREACHABLE();
        }
    }

    std::optional<TEstimate> EstimateLeaf(TExprId id, bool negative) const {
        using O = EOpcode;
        const auto& node = Arena[id];
        const auto child = [&](size_t i) -> const TMetadata& { return Metadata.at(node.Arguments[i]); };
        const auto constant = [&](bool value) { return TEstimate{double(value != negative)}; };
        const auto leaf = [&](ui64 key, double value, TExprId column = Missing) {
            return TEstimate{value, true, key, {column, {}, {}}};
        };
        if (node.Op == O::Literal) {
            if (std::holds_alternative<std::monostate>(node.Value)) return TEstimate{0.0};
            if (const auto* value = std::get_if<bool>(&node.Value)) return constant(*value);
        }
        if (node.Op == O::IsNull || node.Op == O::IsNotNull) {
            if (child(0).Literal != Missing) {
                const bool absent = std::holds_alternative<std::monostate>(Arena[child(0).Literal].Value);
                return constant(absent == (node.Op == O::IsNull));
            }
            const auto type = Arena[node.Arguments[0]].Type;
            if (type.Kind == ETypeKind::Null) return constant(node.Op == O::IsNull);
            if (!type.OptionalDepth) return constant(node.Op == O::IsNotNull);
            return std::nullopt;
        }
        if (node.Op == O::Equal || node.Op == O::NotEqual || node.Op == O::Less || node.Op == O::LessEqual ||
            node.Op == O::Greater || node.Op == O::GreaterEqual) {
            auto left = child(0), right = child(1);
            auto op = node.Op;
            const auto null = [&](const TMetadata& value) {
                return value.Literal != Missing && std::holds_alternative<std::monostate>(Arena[value.Literal].Value);
            };
            if (null(left) || null(right)) return TEstimate{0.0};
            if (left.Column == Missing && right.Column != Missing) { std::swap(left, right); op = SwapComparison(op); }
            if (left.Column == Missing) return leaf(EmptyAttribute, 1.0);
            const auto column = left.Column;
            const auto key = Arena[column].ColumnId;
            if (right.Column != Missing) return leaf(key, 0.3, column);
            if (!right.ColumnFree) return leaf(NonConstant, 1.0);
            // Parameter comparisons use the existing equality default for every
            // comparison sign, including under NOT. They are not literal ranges.
            if (right.HasParameters) return leaf(Parameter, EqualityDefault(column));
            if (negative) op = NegateComparison(op);
            const bool literal = right.Literal != Missing && Compare(column, right.Literal, right.Literal).has_value();
            const auto comparison = [&](O comparison) {
                auto result = leaf(key, comparison == O::Equal ? EqualityDefault(column) : 0.4, column);
                if (literal) {
                    if (comparison == O::Equal || comparison == O::Greater || comparison == O::GreaterEqual)
                        result.Range.Lower = {right.Literal, comparison == O::Equal || comparison == O::GreaterEqual};
                    if (comparison == O::Equal || comparison == O::Less || comparison == O::LessEqual)
                        result.Range.Upper = {right.Literal, comparison == O::Equal || comparison == O::LessEqual};
                    result.Value = RangeEstimate(result.Range);
                }
                return result;
            };
            if (op == O::NotEqual) return TEstimate{EstimateGroup({comparison(O::Less), comparison(O::Greater)}, false)};
            return comparison(op);
        }
        if (node.Op == O::Contains || node.Op == O::StartsWith || node.Op == O::EndsWith || node.Op == O::Find) {
            auto left = child(0), right = child(1);
            const auto originalColumn = left.Column;
            if (left.Column == Missing && left.ColumnFree && right.Column != Missing) std::swap(left, right);
            double value = 1.0;
            if (left.Column != Missing) {
                if (right.Column != Missing) value = 0.3;
                else if (right.ColumnFree) value = (node.Op == O::Contains || node.Op == O::Find) && !right.HasParameters ? 0.1 : 0.5;
            }
            return leaf(originalColumn == Missing ? NoAttribute : Arena[originalColumn].ColumnId,
                negative ? 1 - value : value, originalColumn);
        }
        if (node.Op == O::Like || node.Op == O::ILike || node.Op == O::RegexMatch || node.Op == O::RegexGrep) {
            // The existing estimator groups runtime regex predicates under
            // one synthetic attribute, with 0.5 selectivity in either polarity.
            return leaf(RegexAttribute, 0.5);
        }
        // Unsupported predicates are omitted from logical groups, exactly as
        // by the existing estimator, rather than inventing a 50% selectivity.
        return std::nullopt;
    }

    const TExpressionArena& Arena;
    const TOptimizerStatistics& Stats;
    TNativePredicateStatistics* Usage;
    THashMap<TExprId, TMetadata> Metadata;
    THashMap<ui64, std::optional<TEstimate>> Estimates;
    THashMap<ui64, TVector<std::pair<ui64, ui64>>> Operands;
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
