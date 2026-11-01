#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_utils.h>
#include <ydb/core/kqp/provider/yql_kikimr_settings.h>

#include <yql/essentials/core/extract_predicate/extract_predicate.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_opt_utils.h>

#include <utility>

namespace NKikimr::NKqp {

namespace {

using namespace NYql::NNodes;
using namespace NKikimr;

bool IsValidForRange(const NYql::TExprNode::TPtr& node) {
    TExprBase expr(node);
    if (auto sqlin = expr.Maybe<TCoSqlIn>()) {
        auto collection = sqlin.Cast().Collection().Ptr();
        bool result = true;
        VisitExpr(collection, [&](const TExprNode::TPtr& node) {
            if (node->IsCallable() && (node->Content().StartsWith("Dq") || node->Content().StartsWith("Kql") || node->Content().StartsWith("Kqp"))) {
                result = false;
                return false;
            }
            return true;
        });
        return result;
    }
    return true;
}

TExprNode::TPtr TypeAnnotateLambda(TExprNode::TPtr lambda, const TStructExprType* structType, TRBOContext& ctx) {
    if (!UpdateLambdaAllArgumentsTypes(lambda, {structType}, ctx.ExprCtx)) {
        return nullptr;
    }

    ctx.TypeAnnTransformer.Rewind();
    IGraphTransformer::TStatus status(IGraphTransformer::TStatus::Ok);
    do {
        status = ctx.TypeAnnTransformer.Transform(lambda, lambda, ctx.ExprCtx);
    } while (status == IGraphTransformer::TStatus::Repeat);

    return status == IGraphTransformer::TStatus::Ok ? lambda : nullptr;
}

// The extractor builds a key range for Exists, which requires an optional key.
TExprNode::TPtr FoldExistsOverNonOptional(TExprNode::TPtr lambda, const TStructExprType* structType, TRBOContext& ctx) {
    TOptimizeExprSettings settings(&ctx.TypeCtx);
    // So we try to fold Exists.
    const auto status = OptimizeExpr(lambda, lambda, [&](const TExprNode::TPtr& node, TExprContext& exprCtx) -> TExprNode::TPtr {
        return node->IsCallable("Exists") ? OptimizeExists(node, exprCtx, ctx.TypeCtx) : node;
    }, ctx.ExprCtx, settings);
    if (status == IGraphTransformer::TStatus::Error) {
        return nullptr;
    }

    return TypeAnnotateLambda(lambda, structType, ctx);
}

// The range extractor expects a non optional predicate typed over the read row type.
TExprNode::TPtr GetLambdaForRangeExtractor(TExprNode::TPtr node, const TTypeAnnotationNode* inputType, TRBOContext& rboCtx) {
    if (!inputType) {
        return node;
    }

    auto& ctx = rboCtx.ExprCtx;
    auto structType = inputType->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();

    auto lambda = TypeAnnotateLambda(ctx.DeepCopyLambda(*node), structType, rboCtx);
    if (lambda) {
        lambda = FoldExistsOverNonOptional(lambda, structType, rboCtx);
    }
    if (!lambda || !lambda->GetTypeAnn()->IsOptionalOrNull()) {
        return lambda;
    }

    // Wrap over coalesce.
    // clang-format off
    auto newBody = Build<TCoCoalesce>(ctx, node->Pos())
        .Predicate(TCoLambda(lambda).Body())
        .Value<TCoBool>()
            .Literal().Build("false")
        .Build()
    .Done();
    // clang-format on

    return TypeAnnotateLambda(ctx.NewLambda(node->Pos(), lambda->HeadPtr(), newBody.Ptr()), structType, rboCtx);
}

bool IsSuitableToExtractAndPushRanges(IOperator* input, const NYql::EStorageType applicableTableType) {
    if (input->Kind != EOperator::Filter) {
        return false;
    }

    const auto filter = CastOperator<TOpFilter>(input);
    const auto maybeRead = filter->GetInput().Get();
    if (maybeRead->Kind != EOperator::Source) {
        return false;
    }

    const auto read = CastOperator<TOpRead>(maybeRead);
    const auto tableType = read->GetTableStorageType();
    return !read->GetRanges() && !(read->RangeInfo && read->RangeInfo->NativeRange) && (tableType == applicableTableType);
}

TPredicateExtractorSettings PrepareExtractorSettings(TKqpOptimizeContext& kqpCtx) {
    TPredicateExtractorSettings settings;
    settings.MergeAdjacentPointRanges = true;
    settings.HaveNextValueCallable = true;
    settings.BuildLiteralRange = true;
    settings.IsValidForRange = IsValidForRange;

    if (kqpCtx.Config->GetExtractPredicateRangesLimit() != 0) {
        settings.MaxRanges = kqpCtx.Config->GetExtractPredicateRangesLimit();
    } else {
        settings.MaxRanges = Nothing();
    }

    if (kqpCtx.QueryCtx->RuntimeParameterSizeLimitSatisfied && kqpCtx.QueryCtx->RuntimeParameterSizeLimit > 0) {
        settings.ExternalParameterMaxSize = kqpCtx.QueryCtx->RuntimeParameterSizeLimit;
    }
    return settings;
}

// Returns a bound value of a literal range in the form the read source takes as is: a literal, a null or a parameter
// of the key column type. The extractor converts the value to the key column type: it retypes an integer literal
// itself and wraps a Utf8 literal for a String key in ToString, which is folded here because the literal text stays
// the same. Null if the value has to be computed.
TExprNode::TPtr GetLiteralBoundValue(const TExprNode::TPtr& value, const TTypeAnnotationNode& keyType, TExprContext& ctx) {
    const TTypeAnnotationNode* type = value->GetTypeAnn();
    if (!type) {
        return nullptr;
    }

    if (value->IsCallable("Just")) {
        return GetLiteralBoundValue(value->HeadPtr(), keyType, ctx);
    }

    if (!IsSameAnnotation(*RemoveOptionalType(type), *RemoveOptionalType(&keyType))) {
        return nullptr;
    }

    if (TCoDataCtor::Match(value.Get()) || TCoPgConst::Match(value.Get()) || TCoNothing::Match(value.Get())
        || TCoParameter::Match(value.Get())) {
        return value;
    }

    if (!value->IsCallable("ToString") || !value->Head().IsCallable("Utf8")) {
        return nullptr;
    }

    auto folded = ctx.NewCallable(value->Pos(), "String", {TCoDataCtor(value->HeadPtr()).Literal().Ptr()});
    folded->SetTypeAnn(type);
    return folded;
}

// Returns the ranges as a KqlKeyRange, which the read source takes as is, when the extractor found them to be a single
// range known at compile time and every bound value is a literal, a null or a parameter. Null otherwise.
TExprNode::TPtr BuildLiteralRange(const IPredicateRangeExtractor::TBuildResult& result, const TVector<TString>& keyColumns,
                                  const TStructExprType& schemeType, TPositionHandle pos, TExprContext& ctx) {
    if (!result.LiteralRange) {
        return nullptr;
    }

    using TBound = IPredicateRangeExtractor::TBuildResult::TLiteralRange::TLiteralRangeBound;
    auto buildBound = [&](const TBound& bound) -> TMaybeNode<TKqlKeyTuple> {
        Y_ENSURE(bound.Columns.size() <= keyColumns.size());
        TExprNode::TListType values;
        for (size_t i = 0; i < bound.Columns.size(); ++i) {
            const auto* keyType = schemeType.FindItemType(keyColumns[i]);
            auto value = keyType ? GetLiteralBoundValue(bound.Columns[i], *keyType, ctx) : nullptr;
            if (!value) {
                return {};
            }
            values.push_back(std::move(value));
        }

        if (bound.Inclusive) {
            return Build<TKqlKeyInc>(ctx, pos).Add(values).Done().Cast<TKqlKeyTuple>();
        }
        return Build<TKqlKeyExc>(ctx, pos).Add(values).Done().Cast<TKqlKeyTuple>();
    };

    const auto from = buildBound(result.LiteralRange->Left);
    const auto to = buildBound(result.LiteralRange->Right);
    if (!from || !to) {
        return nullptr;
    }

    // clang-format off
    return Build<TKqlKeyRange>(ctx, pos)
        .From(from.Cast())
        .To(to.Cast())
    .Done().Ptr();
    // clang-format on
}

// The extractor needs the complete table schema, including keys absent from the Read.
// Selected columns use their ID atoms. Other schema entries are extractor-only labels,
// not plan bindings; a nonnumeric prefix keeps them disjoint from every ID.
THashMap<TString, TString> BuildExtractorNames(const TOpRead& read, const TStructExprType& schema,
                                             const TInfoUnitRegistry& registry, TExprContext& ctx) {
    THashMap<TString, TString> result;
    for (const auto* item : schema.GetItems()) {
        result[TString(item->GetName())] = TStringBuilder() << "column:" << item->GetName();
    }
    for (const auto id : read.GetColumns()) {
        result[registry.Get(id).GetColumnName()] = ctx.GetIndexAsString(id);
    }
    return result;
}

TVector<TString> ResolveExposedKeyColumns(const THashMap<TString, TString>& names, const TVector<TString>& physicalKeyColumns) {
    TVector<TString> keyColumns;
    keyColumns.reserve(physicalKeyColumns.size());
    for (const auto& key : physicalKeyColumns) {
        keyColumns.push_back(names.at(key));
    }
    return keyColumns;
}

const TStructExprType* PrepareSchemeType(const THashMap<TString, TString>& names, const TStructExprType* schemeType,
                                       const TOpRead& read, const TInfoUnitRegistry& registry, TExprContext& ctx) {
    TVector<const TItemExprType*> newItemTypes;
    for (const auto itemType : schemeType->GetItems()) {
        newItemTypes.push_back(ctx.MakeType<TItemExprType>(names.at(TString(itemType->GetName())), itemType->GetItemType()));
    }
    // Several Read IDs may fetch the same storage field. Key matching chooses
    // one, but every ID referenced by the predicate must still have a type.
    for (const auto id : read.GetColumns()) {
        const auto physical = registry.Get(id).GetColumnName();
        const auto name = ctx.GetIndexAsString(id);
        if (name != names.at(physical)) {
            newItemTypes.push_back(ctx.MakeType<TItemExprType>(name, schemeType->FindItemType(physical)));
        }
    }
    return ctx.MakeType<TStructExprType>(newItemTypes);
}

std::optional<TOpRead::TPointPrefix> ExtractPointPrefix(size_t pointPrefixLen, const TExprNode::TPtr& lambda,
                                                        const TStructExprType* schemeType, const THashSet<TString>& possibleKeys,
                                                        const TVector<TString>& exposedKeyColumns, const TKikimrTableMetadata& tableMeta,
                                                        const TPredicateExtractorSettings& baseSettings, TRBOContext& rboCtx) {
    const auto& physicalKeyColumns = tableMeta.KeyColumnNames;
    Y_ENSURE(exposedKeyColumns.size() == physicalKeyColumns.size());
    // A lookup join needs key columns after the prefix to look up by.
    if (pointPrefixLen == 0 || pointPrefixLen >= exposedKeyColumns.size()) {
        return std::nullopt;
    }

    auto& ctx = rboCtx.ExprCtx;

    auto settings = baseSettings;
    settings.MergeAdjacentPointRanges = false;
    settings.HaveNextValueCallable = false;
    settings.MaxRanges = Nothing();

    const TVector<TString> exposedPointColumns(exposedKeyColumns.begin(), exposedKeyColumns.begin() + pointPrefixLen);
    TVector<TString> physicalPointColumns(physicalKeyColumns.begin(), physicalKeyColumns.begin() + pointPrefixLen);

    THashSet<TString> keys = possibleKeys;
    auto extractor = MakePredicateRangeExtractor(settings);
    if (!extractor->Prepare(lambda, *schemeType, keys, ctx, rboCtx.TypeCtx)) {
        return std::nullopt;
    }

    const auto result = extractor->BuildComputeNode(exposedPointColumns, ctx, rboCtx.TypeCtx);
    if (!result.ComputeNode || result.PointPrefixLen != pointPrefixLen) {
        return std::nullopt;
    }

    TVector<const TItemExprType*> items;
    items.reserve(pointPrefixLen);
    for (size_t i = 0; i < pointPrefixLen; ++i) {
        const auto* columnType = schemeType->FindItemType(exposedPointColumns[i]);
        if (!columnType) {
            return std::nullopt;
        }
        items.push_back(ctx.MakeType<TItemExprType>(physicalPointColumns[i], columnType));
    }

    TOpRead::TPointPrefix prefix;
    prefix.Table = tableMeta.Name;
    prefix.Points = BuildPointsList(result, physicalPointColumns, ctx);
    prefix.PointsItemType = ctx.MakeType<TStructExprType>(items);
    prefix.Columns = std::move(physicalPointColumns);
    prefix.ExpectedMaxPoints = result.ExpectedMaxRanges ? TMaybe<size_t>(*result.ExpectedMaxRanges) : TMaybe<size_t>();

    YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Extracted points of " << prefix.Table << ": "
                                 << KqpExprToPrettyString(*prefix.Points, ctx);
    return prefix;
}

struct TIndexScore {
    bool SortMatchesAndNoResidual = false;
    bool PointCoversKey = false;
    size_t PointPrefixLen = 0;
    bool UsedCoversKey = false;
    size_t UsedPrefixLen = 0;
    bool SortMatches = false;

    std::tuple<bool, bool, size_t, bool, size_t> AsTuple() const {
        return std::make_tuple(SortMatchesAndNoResidual, PointCoversKey, PointPrefixLen, UsedCoversKey, UsedPrefixLen);
    }

    bool operator<(const TIndexScore& other) const { return AsTuple() < other.AsTuple(); }
};

bool HasNoResidualPredicate(const TExprNode::TPtr& prunedLambda) {
    if (!prunedLambda) {
        return false;
    }
    const auto body = TCoLambda(prunedLambda).Body();
    if (const auto cond = body.Maybe<TCoConditionalValueBase>()) {
        const auto boolLit = cond.Cast().Predicate().Maybe<TCoBool>();
        return boolLit.IsValid() && boolLit.Cast().Literal().Value() == "true" && cond.Cast().Value().Maybe<TCoArgument>().IsValid();
    }
    return body.Maybe<TCoArgument>().IsValid();
}

TIndexScore ScoreKeyOrder(const IPredicateRangeExtractor::TBuildResult& result, size_t keyLen, const TVector<TString>& sortColumns,
                          const TVector<TString>& keyColumns, bool covering) {
    TIndexScore score;
    score.PointCoversKey = keyLen != 0 && result.PointPrefixLen == keyLen;
    score.PointPrefixLen = score.PointCoversKey ? 0 : result.PointPrefixLen;
    score.UsedCoversKey = keyLen != 0 && result.UsedPrefixLen == keyLen;
    score.UsedPrefixLen = score.UsedCoversKey ? 0 : result.UsedPrefixLen;

    if (covering && !sortColumns.empty()) {
        const size_t pointPrefixLen =
            (result.ExpectedMaxRanges && *result.ExpectedMaxRanges == 1) ? std::min(result.PointPrefixLen, keyColumns.size()) : 0;
        score.SortMatches = SortMatchesKeyOrder(sortColumns, keyColumns, pointPrefixLen);
        score.SortMatchesAndNoResidual = score.SortMatches && HasNoResidualPredicate(result.PrunedLambda);
    }

    return score;
}

// Ties are broken deterministically, independently of the order indexes are declared
bool IsBetterCandidate(const TIndexScore& score, bool covering, const TString& name, const TIndexScore& bestScore, bool bestCovering,
                       const TString& bestName) {
    if (bestScore < score) {
        return true;
    }
    // Index never wins a tie against the main table
    if (score < bestScore || bestName.empty()) {
        return false;
    }
    // Covering index beats non-covering one
    if (covering != bestCovering) {
        return covering;
    }
    if (score.SortMatches != bestScore.SortMatches) {
        return score.SortMatches;
    }
    // Lexicographically smallest index name wins
    return name < bestName;
}

TVector<TString> FindConsumingTopSortColumns(const IOperator* op, TExprContext& ctx) {
    TSubstitutions copies;
    const IOperator* current = op;

    while (current && current->Parents.size() == 1) {
        IOperator* parent = current->Parents.front().first;
        if (!parent) {
            return {};
        }

        if (parent->Kind == EOperator::Map) {
            const auto& elements = CastOperator<TOpMap>(*parent).GetMapElements();
            for (const auto& [output, element] : elements.Items()) {
                if (element.IsColumnAccess()) {
                    copies.Add(output, Substitute(element.GetColumnAccess(), copies));
                }
            }
            current = parent;
            continue;
        }

        if (parent->Kind != EOperator::Sort) {
            return {};
        }

        const auto sort = CastOperator<TOpSort>(parent);
        if (!sort->LimitCond.has_value()) {
            return {};
        }

        TVector<TString> sortColumns;
        const auto& sortElements = sort->GetSortElements().Items();
        sortColumns.reserve(sortElements.size());

        const bool ascending = sortElements.empty() ? true : sortElements.front().second.Ascending;
        for (const auto& [id, order] : sortElements) {
            if (order.Ascending != ascending || order.NullsFirst != ascending) {
                return {};
            }
            sortColumns.push_back(TString(ctx.GetIndexAsString(Substitute(id, copies))));
        }
        return sortColumns;
    }

    return {};
}

bool IsSelectableIndex(const TIndexDescription& index) {
    return index.Type != TIndexDescription::EType::GlobalAsync
        && index.Type != TIndexDescription::EType::GlobalJson
        && index.Type != TIndexDescription::EType::GlobalJsonCompact
        && index.Type != TIndexDescription::EType::LocalMinMax
        && index.Type != TIndexDescription::EType::LocalBloomFilter
        && index.Type != TIndexDescription::EType::LocalBloomNgramFilter
        && index.State == TIndexDescription::EIndexState::Ready;
}

bool IsCovering(const TOpRead& read, const TKikimrTableMetadata& indexMeta, const TInfoUnitRegistry& registry) {
    for (const auto id : read.GetColumns()) {
        if (!indexMeta.Columns.contains(registry.Get(id).GetColumnName())) {
            return false;
        }
    }
    return true;
}

bool IsUselessIndex(const TVector<TString>& indexKeyColumns, const TVector<TString>& mainKeyColumns) {
    const size_t common = std::min(indexKeyColumns.size(), mainKeyColumns.size());
    for (size_t i = 0; i < common; ++i) {
        if (indexKeyColumns[i] != mainKeyColumns[i]) {
            return false;
        }
    }
    return true;
}

const TKikimrTableDescription* FindTable(const NOpt::TKqpOptimizeContext& kqpCtx, const TString& path) {
    const auto& tables = kqpCtx.Tables->GetTables();
    const auto it = tables.find(std::make_pair(kqpCtx.Cluster, path));
    return it != tables.end() ? &it->second : nullptr;
}

NYql::EStorageType GetStorageType(const TKikimrTableMetadata& meta) {
    switch (meta.Kind) {
        case EKikimrTableKind::Datashard:
            return NYql::EStorageType::RowStorage;
        case EKikimrTableKind::Olap:
            return NYql::EStorageType::ColumnStorage;
        default:
            return NYql::EStorageType::NA;
    }
}

TVector<TString> FilterPhysicalColumns(const TOpFilter& filter, const TOpRead& read, TPlanProps& props) {
    TVector<TString> result;
    THashSet<TString> seen;
    for (const auto& iu : filter.GetFilterIUs(props)) {
        Y_ENSURE(read.GetColumns().Contains(iu));
        const auto physical = props.InfoUnitRegistry.Get(iu).GetColumnName();
        if (seen.insert(physical).second) {
            result.push_back(physical);
        }
    }
    return result;
}

TVector<TString> BuildIndexReadColumns(const TVector<TString>& pkColumns, const TVector<TString>& filterPhysical) {
    TVector<TString> result;
    THashSet<TString> seen;
    for (const auto& c : pkColumns) {
        if (seen.insert(c).second) {
            result.push_back(c);
        }
    }
    for (const auto& c : filterPhysical) {
        if (seen.insert(c).second) {
            result.push_back(c);
        }
    }
    return result;
}

TExprNode::TPtr BuildTableCallable(const TKikimrTableMetadata& meta, TPositionHandle pos, TExprContext& ctx) {
    // clang-format off
    return Build<TKqpTable>(ctx, pos)
        .Path().Build(meta.Name)
        .PathId().Build(meta.PathId.ToString())
        .SysView().Build(meta.SysView)
        .Version().Build(meta.SchemaVersion)
    .Done().Ptr();
    // clang-format on
}

NFrontend::TExprId StripNativeOptionalLift(const NFrontend::TExpressionArena& arena, NFrontend::TExprId id) {
    using O = NFrontend::EOpcode;
    while (arena[id].Op == O::Convert && arena[id].Type.Required() == arena[arena[id].Arguments[0]].Type.Required()) {
        id = arena[id].Arguments[0];
    }
    return id;
}

// Storage bounds use the key's exact type. Integer comparisons in the native
// IR compare mathematical values across widths/signedness: never round them
// through Double or narrow an out-of-range value into the key domain.
template <typename T>
bool FitsNativeKey(const NFrontend::TScalar& value) {
    if (const auto* v = std::get_if<std::int64_t>(&value)) return std::in_range<T>(*v);
    if (const auto* v = std::get_if<std::uint64_t>(&value)) return std::in_range<T>(*v);
    return false;
}

std::optional<NFrontend::TExprId> NativeBoundValue(NFrontend::TExpressionArena& arena,
    NFrontend::TExprId id, NFrontend::TType keyType) {
    using namespace NFrontend;
    using K = ETypeKind;
    id = StripNativeOptionalLift(arena, id);
    const auto& node = arena[id];
    keyType = keyType.Required();
    if (node.Op == EOpcode::Parameter) {
        return node.Type == keyType ? std::optional{id} : std::nullopt;
    }
    if (node.Op != EOpcode::Literal || std::holds_alternative<std::monostate>(node.Value)) return {};
    if (node.Type == keyType) return id;
    if (node.Type.Required() == keyType) return arena.Literal(keyType, node.Value, node.Position);
    if (!node.Type.IsIntegral() || !keyType.IsIntegral()) return {};
    bool fits = false;
    switch (keyType.Kind) {
        case K::Int8: fits = FitsNativeKey<i8>(node.Value); break;
        case K::Int16: fits = FitsNativeKey<i16>(node.Value); break;
        case K::Int32: fits = FitsNativeKey<i32>(node.Value); break;
        case K::Int64: fits = FitsNativeKey<i64>(node.Value); break;
        case K::Uint8: fits = FitsNativeKey<ui8>(node.Value); break;
        case K::Uint16: fits = FitsNativeKey<ui16>(node.Value); break;
        case K::Uint32: fits = FitsNativeKey<ui32>(node.Value); break;
        case K::Uint64: fits = FitsNativeKey<ui64>(node.Value); break;
        default: break;
    }
    if (!fits) return {};
    if (keyType.IsSigned()) {
        const auto value = std::holds_alternative<std::int64_t>(node.Value) ? std::get<std::int64_t>(node.Value)
            : static_cast<std::int64_t>(std::get<std::uint64_t>(node.Value));
        return arena.Literal(keyType, value, node.Position);
    }
    const auto value = std::holds_alternative<std::uint64_t>(node.Value) ? std::get<std::uint64_t>(node.Value)
        : static_cast<std::uint64_t>(std::get<std::int64_t>(node.Value));
    return arena.Literal(keyType, value, node.Position);
}

TIntrusivePtr<IOperator> PushNativeRange(const TIntrusivePtr<IOperator>& input, const TKikimrTableMetadata& table,
    TRBOContext& ctx, TPlanProps& props) {
    using namespace NFrontend;
    using O = EOpcode;
    using K = ETypeKind;
    const auto filter = CastOperator<TOpFilter>(input);
    const auto read = CastOperator<TOpRead>(filter->GetInput());
    const auto& predicate = filter->GetFilterExpression();
    // A single literal/parameter range has a direct row-source interface.
    // Column-store computed ranges, disjunctions and index selection need their
    // own native representations; they must not enter the legacy extractor.
    if (read->StorageType != NYql::EStorageType::RowStorage || read->Limit || read->OlapProgram ||
        !predicate.GetRawInputIUs().IsSubsetOf(read->GetColumns())) return input;
    const auto arena = predicate.GetNativeArena();
    THashSet<TExprId> visited;
    TVector<TExprId> pending{predicate.GetNativeRoot()};
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!visited.insert(id).second) continue;
        const auto& node = (*arena)[id];
        // Discarding a row (including NULL) must not suppress a checked term
        // which the original lazy Boolean expression would have evaluated.
        if (node.Op == O::Ensure || node.Op == O::Unwrap) return input;
        pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
    }
    struct TCondition {
        size_t Term;
        TExprId Value;
        TType KeyType;
        bool Inclusive;
    };
    struct TConditions {
        std::optional<TCondition> Equal, Lower, Upper;
    };
    THashMap<TString, TConditions> keys;
    for (const auto& key : table.KeyColumnNames) keys.emplace(key, TConditions{});
    const auto terms = predicate.SplitConjunct();
    for (size_t i = 0; i < terms.size(); ++i) {
        const auto id = StripNativeOptionalLift(*arena, terms[i].GetNativeRoot());
        auto op = (*arena)[id].Op;
        if (op != O::Equal && op != O::Less && op != O::LessEqual && op != O::Greater && op != O::GreaterEqual) continue;
        auto column = StripNativeOptionalLift(*arena, (*arena)[id].Arguments[0]);
        auto value = (*arena)[id].Arguments[1];
        if ((*arena)[column].Op != O::Column) {
            std::swap(column, value);
            column = StripNativeOptionalLift(*arena, column);
            switch (op) {
                case O::Less: op = O::Greater; break;
                case O::LessEqual: op = O::GreaterEqual; break;
                case O::Greater: op = O::Less; break;
                case O::GreaterEqual: op = O::LessEqual; break;
                default: break;
            }
        }
        if ((*arena)[column].Op != O::Column) continue;
        const auto keyType = (*arena)[column].Type;
        // Floating/Decimal NaNs and value-changing column casts require an
        // explicit ordering proof. Identity/optional lifts are the only casts
        // stripped here; integer literals alone may change width exactly.
        if (!keyType.IsIntegral() && !keyType.IsString() && keyType.Kind != K::Bool && keyType.Kind != K::Date &&
            keyType.Kind != K::Datetime && keyType.Kind != K::Timestamp && keyType.Kind != K::Interval) continue;
        const auto it = keys.find(props.InfoUnitRegistry.Get((*arena)[column].ColumnId).GetColumnName());
        if (it == keys.end()) continue;
        const auto bound = NativeBoundValue(*arena, value, keyType);
        if (!bound) continue;
        auto& conditions = it->second;
        auto& slot = op == O::Equal ? conditions.Equal : (op == O::Greater || op == O::GreaterEqual ? conditions.Lower : conditions.Upper);
        if (!slot) slot = TCondition{i, *bound, keyType, op == O::Equal || op == O::GreaterEqual || op == O::LessEqual};
    }
    TOpRead::TRangeInfo info;
    info.KeyColumns = table.KeyColumnNames;
    info.MainTable = table.Name;
    info.ExpectedMaxRanges = 1;
    info.NativeRange.emplace();
    auto& range = *info.NativeRange;
    THashSet<size_t> consumed;
    const auto append = [&](TOpRead::TKeyBound& bound, const TCondition& condition) {
        bound.Values.push_back(TExpression::FromNative(arena, condition.Value, &ctx.ExprCtx, &props, read->Pos));
        bound.Inclusive = condition.Inclusive;
        consumed.insert(condition.Term);
    };
    for (const auto& key : table.KeyColumnNames) {
        const auto& conditions = keys.at(key);
        if (conditions.Equal) {
            append(range.Lower, *conditions.Equal);
            append(range.Upper, *conditions.Equal);
            ++info.UsedPrefixLen;
            ++info.PointPrefixLen;
            continue;
        }
        if (!conditions.Lower && !conditions.Upper) break;
        if (conditions.Lower) append(range.Lower, *conditions.Lower);
        if (conditions.Upper) {
            append(range.Upper, *conditions.Upper);
            // NULL sorts before every non-NULL storage value, but a SQL
            // comparison never accepts it. An upper-only interval must exclude
            // that NULL key, including all suffixes of a composite key.
            if (!conditions.Lower && conditions.Upper->KeyType.Nullable) {
                const auto null = arena->Literal(conditions.Upper->KeyType, {});
                range.Lower.Values.push_back(TExpression::FromNative(arena, null, &ctx.ExprCtx, &props, read->Pos));
                range.Lower.Inclusive = false;
            }
        }
        ++info.UsedPrefixLen;
        break;
    }
    if (!info.UsedPrefixLen) return input;
    auto newRead = MakeIntrusive<TOpRead>(read->Alias, read->GetColumns(), read->StorageType, read->TableCallable,
        read->OlapProgram, read->Limit, std::move(info), read->OriginalPredicate ? read->OriginalPredicate : std::optional{predicate},
        read->SortDir, read->Props, read->Pos);
    TVector<TExpression> remaining;
    for (size_t i = 0; i < terms.size(); ++i) if (!consumed.contains(i)) remaining.push_back(terms[i]);
    if (remaining.empty()) return newRead;
    return MakeIntrusive<TOpFilter>(newRead, filter->Pos, filter->Props, MakeConjunction(remaining), true);
}

} // anonymous namespace

bool TPushRangesRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter &&
        input->GetChildren().front()->Kind == EOperator::Source;
}

TIntrusivePtr<IOperator> TPushRangesRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& rboCtx, TPlanProps& props) {
    auto& kqpCtx = rboCtx.KqpCtx;
    auto& ctx = rboCtx.ExprCtx;
    auto& typeCtx = rboCtx.TypeCtx;

    auto predicateExtractSetting = kqpCtx.Config->GetOptPredicateExtract();
    if (predicateExtractSetting == EOptionalFlag::Disabled) {
        return input;
    }

    if (!IsSuitableToExtractAndPushRanges(input.get(), ApplicableTableType)) {
        return input;
    }

    const auto filter = CastOperator<TOpFilter>(input);
    const auto read = CastOperator<TOpRead>(filter->GetInput().Get());
    const auto tablePath = TExprBase(read->GetTable()).Cast<TKqpTable>().Path().StringValue();

    // Check for table.
    const auto tableDesc = kqpCtx.Tables->EnsureTableExists(kqpCtx.Cluster, tablePath, read->Pos, ctx);
    if (!tableDesc || !tableDesc->Metadata) {
        return input;
    }

    const auto tableKind = tableDesc->Metadata->Kind;
    if (tableKind != EKikimrTableKind::Olap && tableKind != EKikimrTableKind::Datashard) {
        return input;
    }

    if (filter->GetFilterExpression().IsNative()) return PushNativeRange(input, *tableDesc->Metadata, rboCtx, props);
    Y_ENSURE(!props.NativeExpressions, "Legacy range extraction in a native plan");

    const auto extractorLambda = GetLambdaForRangeExtractor(filter->GetFilterExpression().Node, read->Type, rboCtx);
    if (!extractorLambda) {
        return input;
    }

    auto lambda = TCoLambda(extractorLambda);
    auto originalLambda = ctx.DeepCopyLambda(*lambda.Ptr());
    // Predicate extract lib requires constraints.
    auto arg = lambda.Args().Arg(0).Ptr();
    arg->AddConstraint(ctx.MakeConstraint<TEmptyConstraintNode>());

    THashSet<TString> possibleKeys;
    auto settings = PrepareExtractorSettings(kqpCtx);
    auto extractor = MakePredicateRangeExtractor(settings);
    const auto extractorNames = BuildExtractorNames(*read, *tableDesc->SchemeNode, props.InfoUnitRegistry, ctx);
    const auto schemeType = PrepareSchemeType(extractorNames, tableDesc->SchemeNode, *read, props.InfoUnitRegistry, ctx);
    const bool prepareSuccess = extractor->Prepare(lambda.Ptr(), *schemeType, possibleKeys, ctx, typeCtx);
    YQL_ENSURE(prepareSuccess);

    const auto& mainMeta = *tableDesc->Metadata;
    const auto mainKeyColumns = ResolveExposedKeyColumns(extractorNames, mainMeta.KeyColumnNames);
    const auto mainResult = extractor->BuildComputeNode(mainKeyColumns, ctx, typeCtx);
    const auto sortColumns =
        read->GetTableStorageType() == NYql::EStorageType::RowStorage ? FindConsumingTopSortColumns(input.get(), ctx) : TVector<TString>();

    TIntrusivePtr<TKikimrTableMetadata> chosenIndexMeta;
    IPredicateRangeExtractor::TBuildResult winnerResult;
    TVector<TString> winnerKeyColumns;

    TIntrusivePtr<TKikimrTableMetadata> lookupIndexMeta;
    IPredicateRangeExtractor::TBuildResult lookupResult;
    TVector<TString> lookupKeyColumns;
    TVector<TString> lookupReadColumns;

    // Point prefixes are computed for every table the read can be redirected to, so a lookup join can
    // choose the one which fits its keys best. Otherwise only the table chosen for the read gets one.
    // Point lookups are only applicable to row storage tables.
    const bool allPointPrefixes = kqpCtx.Config->GetEnableLookupJoinPointPrefixes();
    TVector<TOpRead::TPointPrefix> pointPrefixes;
    auto addPointPrefix = [&](const TKikimrTableMetadata& tableMeta, const IPredicateRangeExtractor::TBuildResult& result,
                              const TVector<TString>& exposedKeyColumns) {
        if (read->GetTableStorageType() != NYql::EStorageType::RowStorage) {
            return;
        }
        if (auto prefix = ExtractPointPrefix(result.PointPrefixLen, lambda.Ptr(), schemeType, possibleKeys, exposedKeyColumns,
                                             tableMeta, settings, rboCtx)) {
            pointPrefixes.push_back(std::move(*prefix));
        }
    };
    if (allPointPrefixes) {
        addPointPrefix(mainMeta, mainResult, mainKeyColumns);
    }

    auto bestScore = ScoreKeyOrder(mainResult, mainKeyColumns.size(), sortColumns, mainKeyColumns, true);
    TString bestIndexName;
    bool bestCovering = false;

    if (!kqpCtx.Config->IsAutoIndexSelectionDisabled() && !bestScore.PointCoversKey) {
        const auto filterPhysical = FilterPhysicalColumns(*filter, *read, props);
        for (const auto& index : mainMeta.Indexes) {
            if (!IsSelectableIndex(index)) {
                continue;
            }

            const auto indexMeta = mainMeta.GetIndexMetadata(index.Name).first;
            if (!indexMeta || IsUselessIndex(indexMeta->KeyColumnNames, mainMeta.KeyColumnNames)) {
                continue;
            }

            if (!FindTable(kqpCtx, indexMeta->Name)) {
                continue;
            }

            auto indexKeyColumns = ResolveExposedKeyColumns(extractorNames, indexMeta->KeyColumnNames);
            auto indexResult = extractor->BuildComputeNode(indexKeyColumns, ctx, typeCtx);
            if (!indexResult.ComputeNode) {
                continue;
            }

            // A lookup join fetches the rows it finds in a non-covering index from the main table and applies
            // the whole predicate to them, so the index needs neither to cover the read nor the predicate.
            if (allPointPrefixes) {
                addPointPrefix(*indexMeta, indexResult, indexKeyColumns);
            }

            const bool covering = IsCovering(*read, *indexMeta, props.InfoUnitRegistry);
            if (!covering) {
                if (read->Limit) {
                    continue;
                }
                const bool evaluable = std::all_of(filterPhysical.begin(), filterPhysical.end(),
                                                   [&](const TString& col) { return indexMeta->Columns.contains(col); });
                if (!evaluable) {
                    continue;
                }
            }

            const auto score = ScoreKeyOrder(indexResult, indexKeyColumns.size(), sortColumns, indexKeyColumns, covering);
            if (!IsBetterCandidate(score, covering, index.Name, bestScore, bestCovering, bestIndexName)) {
                continue;
            }

            bestScore = score;
            bestIndexName = index.Name;
            bestCovering = covering;
            if (covering) {
                chosenIndexMeta = indexMeta;
                winnerResult = std::move(indexResult);
                winnerKeyColumns = std::move(indexKeyColumns);
                lookupIndexMeta.Reset();
            } else {
                lookupIndexMeta = indexMeta;
                lookupResult = std::move(indexResult);
                lookupKeyColumns = std::move(indexKeyColumns);
                lookupReadColumns = BuildIndexReadColumns(mainMeta.KeyColumnNames, filterPhysical);
                chosenIndexMeta.Reset();
            }
        }
    }

    if (lookupIndexMeta) {
        YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Selected non-covering index " << lookupIndexMeta->Name
                                     << " for a read of " << tablePath;

        TOpRead::TRangeInfo rangeInfo{
            .ComputeNode = lookupResult.ComputeNode,
            .KeyColumns = lookupIndexMeta->KeyColumnNames,
            .UsedPrefixLen = lookupResult.UsedPrefixLen,
            .PointPrefixLen = lookupResult.PointPrefixLen,
            .ExpectedMaxRanges = lookupResult.ExpectedMaxRanges ? TMaybe<size_t>(*lookupResult.ExpectedMaxRanges) : TMaybe<size_t>(),
        };
        if (GetStorageType(*lookupIndexMeta) == NYql::EStorageType::RowStorage) {
            rangeInfo.LiteralRange = BuildLiteralRange(lookupResult, lookupKeyColumns, *schemeType, read->Pos, ctx);
        }

        TUnorderedIUs indexColumns;
        THashMap<TString, TInfoUnitId> indexIds;
        for (const auto& col : lookupReadColumns) {
            const auto id = props.InfoUnitRegistry.Add(TInfoUnit(read->Alias, col));
            indexColumns.Add(id);
            indexIds.emplace(col, id);
        }

        auto indexRead = MakeIntrusive<TOpRead>(read->Alias, std::move(indexColumns), GetStorageType(*lookupIndexMeta),
                                                BuildTableCallable(*lookupIndexMeta, read->Pos, ctx), nullptr, std::nullopt,
                                                std::move(rangeInfo), std::nullopt, ESortDir::None, read->Props, read->Pos);

        TSubstitutions substitutions;
        for (const auto id : filter->GetFilterIUs(props)) {
            substitutions.Add(id, indexIds.at(props.InfoUnitRegistry.Get(id).GetColumnName()));
        }
        auto indexPredicate = TExpression(lookupResult.PrunedLambda, &ctx, &props).ApplyRenames(substitutions);
        auto indexFilter = MakeIntrusive<TOpFilter>(std::move(indexRead), filter->Pos, filter->Props, std::move(indexPredicate), true);

        TOpTableLookup::TLookupKeys lookupKeys;
        for (const auto& pk : mainMeta.KeyColumnNames) {
            lookupKeys.Append(indexIds.at(pk), pk);
        }

        auto lookup = MakeIntrusive<TOpTableLookup>(std::move(indexFilter), read->Pos, read->GetTable(), read->GetColumns(),
                                                    std::move(lookupKeys));
        // Keep the read predicate and the point prefixes, so a lookup join can still probe the main table
        // or an index instead of this subtree.
        if (allPointPrefixes && read->GetTableStorageType() == NYql::EStorageType::RowStorage) {
            lookup->SourceRead = TOpTableLookup::TSourceRead{
                .Predicate = TExpression(originalLambda, &ctx, &props),
                .PointPrefixes = std::move(pointPrefixes),
            };
        }
        return lookup;
    }

    const auto& chosen = chosenIndexMeta ? winnerResult : mainResult;
    if (!chosen.ComputeNode) {
        return input;
    }
    const auto& chosenKeyColumns = chosenIndexMeta ? winnerKeyColumns : mainKeyColumns;

    if (chosenIndexMeta) {
        YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Selected index " << chosenIndexMeta->Name << " for a read of " << tablePath;
    }
    YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Extracted ranges: " << KqpExprToPrettyString(*chosen.ComputeNode, ctx);
    YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Pruned lambda: " << KqpExprToPrettyString(*chosen.PrunedLambda, ctx);

    if (!allPointPrefixes) {
        addPointPrefix(chosenIndexMeta ? *chosenIndexMeta : mainMeta, chosen, chosenKeyColumns);
    }

    TOpRead::TRangeInfo rangeInfo{
        .ComputeNode = chosen.ComputeNode,
        .KeyColumns = chosenIndexMeta ? chosenIndexMeta->KeyColumnNames : mainMeta.KeyColumnNames,
        .UsedPrefixLen = chosen.UsedPrefixLen,
        .PointPrefixLen = chosen.PointPrefixLen,
        .ExpectedMaxRanges = chosen.ExpectedMaxRanges ? TMaybe<size_t>(*chosen.ExpectedMaxRanges) : TMaybe<size_t>(),
        .PointPrefixes = std::move(pointPrefixes),
        .MainTable = mainMeta.Name,
    };
    const auto storageType = chosenIndexMeta ? GetStorageType(*chosenIndexMeta) : read->GetTableStorageType();

    // Only the row storage read source takes a literal range.
    if (storageType == NYql::EStorageType::RowStorage) {
        rangeInfo.LiteralRange = BuildLiteralRange(chosen, chosenKeyColumns, *schemeType, read->Pos, ctx);
        if (rangeInfo.LiteralRange) {
            YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Literal range: " << KqpExprToPrettyString(*rangeInfo.LiteralRange, ctx);
        }
    }

    const auto tableCallable = chosenIndexMeta ? BuildTableCallable(*chosenIndexMeta, read->Pos, ctx) : read->TableCallable;
    const auto sortDir = chosenIndexMeta ? ESortDir::None : read->SortDir;
    auto newRead = MakeIntrusive<TOpRead>(read->Alias, read->GetColumns(), storageType, tableCallable, read->OlapProgram,
                                          read->Limit, std::move(rangeInfo), TExpression(originalLambda, &ctx, &props), sortDir, read->Props, read->Pos);
    return MakeIntrusive<TOpFilter>(newRead, filter->Pos, filter->Props, TExpression(chosen.PrunedLambda, &ctx, &props), true);
}
} // namespace NKikimr::NKqp
