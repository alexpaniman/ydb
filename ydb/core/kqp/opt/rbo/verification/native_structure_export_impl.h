// Private implementation included by semantic_snapshot.cpp after ExportCatalog.
// This is descriptive capture, separate from the semantic snapshot producer.
// New expression callables need no additions here: reviewed Python adapters can
// interpret the complete typed AST. Unhandled relational families stay explicit.

class TNativeStructureExporter {
public:
    TNativeStructureExporter(TOpRoot& root, TStringBuf cluster,
        const TSemanticSnapshotCatalogV1* catalog)
        : Root(root)
        , Cluster(cluster)
        , Catalog(catalog)
    {
    }

    TString Export() {
        auto result = JsonMap();
        result["format"] = "ydb-rbo-native-structure";
        result["version"] = 1;
        result["observation"] = "diagnostic_only";
        result["root"] = OperatorId(&Root);
        result["catalog"] = NJson::TJsonValue(NJson::JSON_NULL);
        result["catalog_complete"] = false;
        if (Catalog) {
            try {
                CheckCatalogBudget(*Catalog);
                result["catalog"] = ExportCatalog(*Catalog);
                result["catalog_complete"] = true;
            } catch (const std::exception& error) {
                result["catalog_error"] = error.what();
            }
        }
        result["pg_syntax"] = Root.PlanProps.PgSyntax;
        result["with_effects"] = Root.PlanProps.WithEffects;
        result["with_returning"] = Root.PlanProps.WithReturning;
        const auto& graph = Root.PlanProps.StageGraph;
        bool stageGraphPresent = !graph.StageIds.empty() || !graph.SourceStages.empty() ||
            !graph.SinkStages.empty() || !graph.StageInputs.empty() || !graph.StageOutputs.empty() ||
            !graph.Connections.empty() || !graph.StageOutputIndices.empty() || !graph.StageGUIDs.empty();
        result["stage_graph"] = StageGraph();
        auto subplans = JsonArray();
        for (const auto& [binding, entry] : Root.PlanProps.Subplans) {
            Charge();
            auto item = JsonMap();
            item["binding"] = ToString(binding);
            item["root"] = OperatorId(entry.Plan.Get());
            item["kind"] = static_cast<ui32>(entry.Type);
            item["tuple"] = Ids(entry.Tuple.Items());
            item["dependencies"] = Ids(entry.DependentIUs);
            item["result"] = entry.ResultIU
                ? NJson::TJsonValue(ToString(*entry.ResultIU))
                : NJson::TJsonValue(NJson::JSON_NULL);
            subplans.AppendValue(std::move(item));
        }
        result["subplans"] = std::move(subplans);

        auto registry = JsonArray();
        if (Root.PlanProps.InfoUnitRegistry.Size() > MaxNodes) {
            Unsupported("Native structure registry budget exceeded");
        }
        for (TInfoUnitId id = 0; id < Root.PlanProps.InfoUnitRegistry.Size(); ++id) {
            Charge();
            const auto& source = Root.PlanProps.InfoUnitRegistry.Get(id);
            auto item = JsonMap();
            item["id"] = ToString(id);
            item["alias_hex"] = Hex(source.GetAlias());
            item["column_hex"] = Hex(source.GetColumnName());
            registry.AppendValue(std::move(item));
        }
        result["registry"] = std::move(registry);

        bool complete = result["catalog_complete"].GetBoolean() &&
            !stageGraphPresent && Root.PlanProps.Subplans.Empty() &&
            !Root.PlanProps.PgSyntax && !Root.PlanProps.WithEffects && !Root.PlanProps.WithReturning;
        auto operators = JsonArray();
        for (size_t i = 0; i < Operators.size(); ++i) {
            auto item = Operator(*Operators[i]);
            stageGraphPresent = stageGraphPresent || Operators[i]->Props.StageId.has_value();
            complete = complete && item["complete"].GetBoolean();
            operators.AppendValue(std::move(item));
        }
        result["operators"] = std::move(operators);
        result["stage_graph_present"] = stageGraphPresent;
        auto expressions = JsonArray();
        for (size_t i = 0; i < Expressions.size(); ++i) {
            expressions.AppendValue(Expression(*Expressions[i]));
        }
        result["expressions"] = std::move(expressions);
        result["logical_complete"] = complete;
        NJsonWriter::TBuf writer;
        writer.WriteJsonValue(&result, true, PREC_NDIGITS, 17);
        if (writer.Str().size() > MaxBytes * 4) {
            Unsupported("Native structure serialized byte budget exceeded");
        }
        return writer.Str();
    }

private:
    static constexpr size_t MaxNodes = 65536;
    static constexpr size_t MaxBytes = 16 * 1024 * 1024;

    void Charge(size_t count = 1) {
        if (count > MaxNodes * 8 - Items) { Unsupported("Native structure item budget exceeded"); }
        Items += count;
    }

    void CountBytes(TStringBuf bytes) {
        if (bytes.size() > MaxBytes - Bytes) { Unsupported("Native structure content byte budget exceeded"); }
        Bytes += bytes.size();
    }

    void CheckCatalogBudget(const TSemanticSnapshotCatalogV1& catalog) {
        for (const auto& table : catalog.Tables) {
            Charge(); CountBytes(table.Name);
            for (const auto& column : table.Columns) {
                Charge(); CountBytes(column.Name); CountBytes(column.Type);
            }
            for (const auto& key : table.UniqueKeys) {
                Charge();
                for (const auto& name : key.Columns) { Charge(); CountBytes(name); }
            }
            if (table.PrimaryKey) {
                for (const auto& name : *table.PrimaryKey) { Charge(); CountBytes(name); }
            }
            if (table.DerivedFrom) { CountBytes(*table.DerivedFrom); }
        }
    }

    class TBoundedText final : public IOutputStream {
    public:
        explicit TBoundedText(size_t& bytes) : Bytes(bytes) {}
        const TString& Str() const { return Buffer; }
    private:
        void DoWrite(const void* data, size_t size) override {
            if (size > MaxBytes - Bytes) { Unsupported("Native structure printed text budget exceeded"); }
            Bytes += size;
            Buffer.append(static_cast<const char*>(data), size);
        }
        size_t& Bytes;
        TString Buffer;
    };

    class TTypeBudgetVisitor final : public TDefaultTypeAnnotationVisitor {
    public:
        explicit TTypeBudgetVisitor(size_t& work) : Work(work) {}
#define NATIVE_STRUCTURE_VISIT_TYPE(Name, Value) \
        void Visit(const T##Name##ExprType& type) override { \
            if (++Depth > 64 || ++Work > MaxNodes * 8) { \
                Unsupported("Native structure type rendering depth/work budget exceeded"); \
            } \
            TDefaultTypeAnnotationVisitor::Visit(type); \
            --Depth; \
        }
        YQL_TYPE_ANN_KIND_MAP(NATIVE_STRUCTURE_VISIT_TYPE)
#undef NATIVE_STRUCTURE_VISIT_TYPE
    private:
        size_t& Work;
        size_t Depth = 0;
    };

    TString Hex(TStringBuf bytes) {
        CountBytes(bytes);
        static constexpr char Digits[] = "0123456789abcdef";
        TString result;
        result.reserve(bytes.size() * 2);
        for (const unsigned char byte : bytes) {
            result.push_back(Digits[byte >> 4]);
            result.push_back(Digits[byte & 15]);
        }
        return result;
    }

    template <typename T>
    NJson::TJsonValue Ids(const T& values) {
        auto result = JsonArray();
        for (const auto id : values) { Charge(); result.AppendValue(ToString(id)); }
        return result;
    }

    NJson::TJsonValue Type(const TTypeAnnotationNode* type, size_t depth = 0) {
        if (!type) { return NJson::TJsonValue(NJson::JSON_NULL); }
        Charge();
        if (depth > 64 || ++TypeNodes > MaxNodes * 8) {
            Unsupported("Native structure type depth/node budget exceeded");
        }
        auto result = JsonMap();
        // Bound expansion before invoking the generic formatter, including
        // recursive kinds that this version keeps as diagnostic text only.
        TTypeBudgetVisitor visitor(TypeRenderWork);
        type->Accept(visitor);
        TBoundedText text(Bytes);
        text << FormatType(type);
        result["kind"] = static_cast<ui32>(type->GetKind());
        result["text"] = text.Str();
        result["flags"] = type->GetFlags();
        if (type->GetKind() == ETypeAnnotationKind::Optional) {
            result["item"] = Type(type->Cast<TOptionalExprType>()->GetItemType(), depth + 1);
        } else if (type->GetKind() == ETypeAnnotationKind::List) {
            result["item"] = Type(type->Cast<TListExprType>()->GetItemType(), depth + 1);
        } else if (type->GetKind() == ETypeAnnotationKind::Struct) {
            auto items = JsonArray();
            for (const auto* item : type->Cast<TStructExprType>()->GetItems()) {
                auto value = JsonMap();
                value["name_hex"] = Hex(item->GetName());
                value["type"] = Type(item->GetItemType(), depth + 1);
                items.AppendValue(std::move(value));
            }
            result["items"] = std::move(items);
        }
        return result;
    }

    TString OperatorId(const IOperator* op) {
        Charge();
        if (++References > MaxNodes * 8) { Unsupported("Native structure reference budget exceeded"); }
        if (!op) { Unsupported("Native structure contains a null operator"); }
        if (const auto* found = OperatorIds.FindPtr(op)) { return *found; }
        if (Operators.size() >= MaxNodes) { Unsupported("Native structure operator budget exceeded"); }
        const TString id = TStringBuilder() << "o" << Operators.size();
        Operators.push_back(op);
        OperatorIds.emplace(op, id);
        return id;
    }

    NJson::TJsonValue ExpressionId(const TExprNode* expression) {
        Charge();
        if (++References > MaxNodes * 8) { Unsupported("Native structure reference budget exceeded"); }
        if (!expression) { return NJson::TJsonValue(NJson::JSON_NULL); }
        if (const auto* found = ExpressionIds.FindPtr(expression)) { return NJson::TJsonValue(*found); }
        if (Expressions.size() >= MaxNodes) { Unsupported("Native structure expression budget exceeded"); }
        const TString id = TStringBuilder() << "e" << Expressions.size();
        Expressions.push_back(expression);
        ExpressionIds.emplace(expression, id);
        return NJson::TJsonValue(id);
    }

    NJson::TJsonValue Expression(const TExprNode& source) {
        auto result = JsonMap();
        result["id"] = ExpressionIds.at(&source);
        TString kind;
        switch (source.Type()) {
            case TExprNode::List: kind = "List"; break;
            case TExprNode::Atom: kind = "Atom"; break;
            case TExprNode::Callable: kind = "Callable"; break;
            case TExprNode::Lambda: kind = "Lambda"; break;
            case TExprNode::Argument: kind = "Argument"; break;
            case TExprNode::Arguments: kind = "Arguments"; break;
            case TExprNode::World: kind = "World"; break;
            default: Unsupported("Native structure expression kind is unknown");
        }
        result["kind"] = kind;
        result["content_hex"] = Hex(source.Content());
        result["flags"] = source.Flags();
        result["state"] = static_cast<ui32>(source.GetState());
        result["type"] = Type(source.GetTypeAnn());
        result["has_result"] = source.HasResult();
        result["result"] = source.HasResult() ? ExpressionId(&source.GetResult())
            : NJson::TJsonValue(NJson::JSON_NULL);
        result["position_aware"] = source.IsPosAware();
        result["has_side_effects"] = source.HasSideEffects();
        result["cse_safe"] = source.IsCseeSafe();
        result["unordered_children"] = (source.IsCallable() || source.IsList()) && source.UnorderedChildren();
        auto children = JsonArray();
        for (const auto& child : source.Children()) { children.AppendValue(ExpressionId(child.Get())); }
        result["children"] = std::move(children);
        result["dependency_scope"] = NJson::TJsonValue(NJson::JSON_NULL);
        if (const auto scope = source.GetDependencyScope()) {
            auto value = JsonMap();
            value["outer"] = ExpressionId(scope->first);
            value["inner"] = ExpressionId(scope->second);
            result["dependency_scope"] = std::move(value);
        }
        if (source.IsArgument()) {
            result["argument_index"] = source.GetArgIndex();
            result["used_in_depends_on"] = source.IsUsedInDependsOn();
        }
        auto constraints = JsonArray();
        const bool constraintsAvailable = source.GetState() >= TExprNode::EState::ConstrComplete;
        result["constraints_available"] = constraintsAvailable;
        if (constraintsAvailable) {
            for (const auto* constraint : source.GetAllConstraints()) {
                Charge();
                TBoundedText stream(Bytes);
                constraint->Out(stream);
                constraints.AppendValue(Hex(stream.Str()));
            }
        }
        result["constraints_text_hex"] = std::move(constraints);
        return result;
    }

    void AddExpression(NJson::TJsonValue& target, TStringBuf role, const TExprNode* expression) {
        Charge();
        auto item = JsonMap();
        item["role"] = TString(role);
        item["root"] = ExpressionId(expression);
        target.AppendValue(std::move(item));
    }

    NJson::TJsonValue Operator(const IOperator& source) {
        auto result = JsonMap();
        result["id"] = OperatorIds.at(&source);
        result["kind"] = source.GetExplainName();
        result["native_kind"] = static_cast<ui32>(source.Kind);
        result["type"] = Type(source.Type);
        result["stage_id"] = source.Props.StageId ? NJson::TJsonValue(*source.Props.StageId)
            : NJson::TJsonValue(NJson::JSON_NULL);
        auto children = JsonArray();
        for (size_t i = 0; i < source.GetChildCount(); ++i) {
            children.AppendValue(OperatorId(source.GetChild(i).Get()));
        }
        result["children"] = std::move(children);
        auto expressions = JsonArray();
        auto fields = JsonMap();
        const bool physical = source.Props.StageId || source.Props.Algorithm || source.Props.OrderEnforcer ||
            source.Props.JoinAlgo || source.Props.UseBlockHashJoin ||
            source.Props.LeftShuffleBy || source.Props.RightShuffleBy;
        result["physical_properties_present"] = physical;
        bool complete = !physical;
        switch (source.Kind) {
            case EOperator::Root: {
                result["kind"] = "Root";
                const auto& root = static_cast<const TOpRoot&>(source);
                auto columns = JsonArray();
                for (const auto& [id, label] : root.GetColumns().Items()) {
                    Charge();
                    auto column = JsonMap();
                    column["id"] = ToString(id);
                    column["label_hex"] = Hex(label);
                    columns.AppendValue(std::move(column));
                }
                fields["columns"] = std::move(columns);
                auto hints = JsonArray();
                for (const auto& hint : root.GetQueryColumns()) { Charge(); hints.AppendValue(Hex(hint)); }
                fields["query_columns_hex"] = std::move(hints);
                AddExpression(expressions, "source_root", root.Node.Get());
                break;
            }
            case EOperator::Source: {
                result["kind"] = "Read";
                const auto& read = static_cast<const TOpRead&>(source);
                fields["storage_type"] = static_cast<ui32>(read.StorageType);
                fields["sort_direction"] = static_cast<ui32>(read.SortDir);
                fields["table_identity"] = NJson::TJsonValue(NJson::JSON_NULL);
                try { fields["table_identity"] = TableReference(read, Cluster).Identity; }
                catch (const std::exception& error) { fields["table_error"] = error.what(); complete = false; }
                auto columns = JsonArray();
                for (const auto id : read.GetColumns()) {
                    Charge();
                    auto column = JsonMap();
                    column["id"] = ToString(id);
                    column["column_hex"] = Hex(Root.PlanProps.InfoUnitRegistry.Get(id).GetColumnName());
                    columns.AppendValue(std::move(column));
                }
                fields["columns"] = std::move(columns);
                AddExpression(expressions, "table", read.TableCallable.Get());
                AddExpression(expressions, "olap_filter", read.OlapFilterLambda.Get());
                AddExpression(expressions, "limit", read.Limit.Get());
                AddExpression(expressions, "original_predicate", read.OriginalPredicate ? read.OriginalPredicate->Node.Get() : nullptr);
                AddExpression(expressions, "ranges", read.GetRanges().Get());
                AddExpression(expressions, "literal_range", read.GetLiteralRange().Get());
                fields["has_range_info"] = read.RangeInfo.has_value();
                if (read.RangeInfo) {
                    fields["range_used_prefix_len"] = read.RangeInfo->UsedPrefixLen;
                    fields["range_point_prefix_len"] = read.RangeInfo->PointPrefixLen;
                    fields["range_expected_max_ranges"] = read.RangeInfo->ExpectedMaxRanges
                        ? NJson::TJsonValue(*read.RangeInfo->ExpectedMaxRanges)
                        : NJson::TJsonValue(NJson::JSON_NULL);
                    fields["range_main_table_hex"] = Hex(read.RangeInfo->MainTable);
                    auto keys = JsonArray();
                    for (const auto& key : read.RangeInfo->KeyColumns) { Charge(); keys.AppendValue(Hex(key)); }
                    fields["range_key_columns_hex"] = std::move(keys);
                    auto prefixes = JsonArray();
                    for (const auto& prefix : read.RangeInfo->PointPrefixes) {
                        Charge();
                        auto item = JsonMap();
                        item["table_hex"] = Hex(prefix.Table);
                        item["points"] = ExpressionId(prefix.Points.Get());
                        item["points_item_type"] = Type(prefix.PointsItemType);
                        item["expected_max_points"] = prefix.ExpectedMaxPoints
                            ? NJson::TJsonValue(*prefix.ExpectedMaxPoints)
                            : NJson::TJsonValue(NJson::JSON_NULL);
                        auto columns = JsonArray();
                        for (const auto& name : prefix.Columns) { Charge(); columns.AppendValue(Hex(name)); }
                        item["columns_hex"] = std::move(columns);
                        prefixes.AppendValue(std::move(item));
                    }
                    fields["range_point_prefixes"] = std::move(prefixes);
                }
                complete = complete && !read.OlapFilterLambda && !read.Limit && !read.OriginalPredicate &&
                    !read.RangeInfo && read.SortDir == ESortDir::None &&
                    (read.StorageType == NYql::EStorageType::RowStorage ||
                     read.StorageType == NYql::EStorageType::ColumnStorage);
                break;
            }
            case EOperator::Filter: {
                const auto& filter = static_cast<const TOpFilter&>(source);
                fields["partially_pushed_down"] = filter.PartiallyPushedDown;
                AddExpression(expressions, "predicate", filter.GetFilterExpression().Node.Get());
                break;
            }
            case EOperator::Map: {
                const auto& map = static_cast<const TOpMap&>(source);
                auto definitions = JsonArray();
                for (const auto& [output, element] : map.GetMapElements().Items()) {
                    Charge();
                    auto definition = JsonMap();
                    definition["output"] = ToString(output);
                    definition["expression"] = ExpressionId(element.GetExpression().Node.Get());
                    definitions.AppendValue(std::move(definition));
                }
                fields["definitions"] = std::move(definitions);
                fields["need_to_push"] = map.NeedToPush;
                break;
            }
            case EOperator::Limit: {
                const auto& limit = static_cast<const TOpLimit&>(source);
                fields["phase"] = ToStringPhase(limit.GetLimitPhase());
                AddExpression(expressions, "count", limit.GetLimitCond().Node.Get());
                AddExpression(expressions, "offset", limit.HasOffset() ? limit.GetOffsetCond()->Node.Get() : nullptr);
                break;
            }
            case EOperator::Join: {
                const auto& join = static_cast<const TOpJoin&>(source);
                fields["join_kind"] = join.JoinKind;
                fields["preserve_input_order"] = join.PreserveInputOrder;
                auto keys = JsonArray();
                for (const auto& key : join.JoinKeys.Items()) {
                    Charge();
                    auto item = JsonMap();
                    item["left"] = ToString(key.first);
                    item["right"] = ToString(key.second);
                    item["equal_nulls"] = key.EqualNulls;
                    keys.AppendValue(std::move(item));
                }
                fields["keys"] = std::move(keys);
                auto predicates = JsonArray();
                for (const auto& predicate : join.JoinFilters) { predicates.AppendValue(ExpressionId(predicate.Node.Get())); }
                fields["predicates"] = std::move(predicates);
                break;
            }
            default: {
                // GetExpressions is useful evidence, not a complete contract:
                // some native operators also store expressions elsewhere.
                complete = false;
                size_t i = 0;
                for (const auto& expression : source.GetExpressions()) {
                    AddExpression(expressions, TStringBuilder() << "generic_" << i++, expression.get().Node.Get());
                }
                if (source.Kind == EOperator::EmptySource) {
                    AddExpression(expressions, "input", static_cast<const TOpEmptySource&>(source).Input.Get());
                }
                break;
            }
        }
        result["fields"] = std::move(fields);
        result["expressions"] = std::move(expressions);
        result["complete"] = complete;
        return result;
    }

    NJson::TJsonValue StageGraph() {
        // Preserve routing descriptors for future reviewed Python adapters.
        // This diagnostic schema deliberately does not promise enough evidence
        // to reinterpret physical programs; logical_complete rejects them.
        auto result = JsonMap();
        result["complete"] = false;
        auto ids = JsonArray();
        for (const auto id : Root.PlanProps.StageGraph.StageIds) { Charge(); ids.AppendValue(id); }
        result["stage_ids"] = std::move(ids);
        auto connections = JsonArray();
        TMap<std::pair<ui32, ui32>, TVector<TIntrusivePtr<TConnection>>> ordered;
        for (const auto& [key, value] : Root.PlanProps.StageGraph.Connections) {
            Charge(1 + value.size());
            ordered.emplace(key, value);
        }
        for (const auto& [key, values] : ordered) {
            for (const auto& connection : values) {
                if (!connection) { Unsupported("Native structure contains a null stage connection"); }
                auto item = JsonMap();
                item["from"] = key.first;
                item["to"] = key.second;
                item["kind"] = connection->Type;
                item["output_index"] = connection->OutputIndex;
                item["used_ids"] = Ids(connection->GetUsedIUs());
                if (const auto* shuffle = dynamic_cast<const TShuffleConnection*>(connection.Get())) {
                    item["keys"] = Ids(shuffle->Keys.Items());
                    item["hash_function"] = shuffle->HashFuncType
                        ? NJson::TJsonValue(static_cast<ui32>(*shuffle->HashFuncType))
                        : NJson::TJsonValue(NJson::JSON_NULL);
                    item["use_spilling"] = shuffle->UseSpilling;
                }
                if (const auto* lookup = dynamic_cast<const TStreamLookupConnection*>(connection.Get())) {
                    item["table"] = ExpressionId(lookup->Table.Get());
                    item["columns"] = ExpressionId(lookup->Columns.Get());
                    item["input_type"] = ExpressionId(lookup->InputType.Get());
                    item["settings"] = ExpressionId(lookup->Settings.Get());
                }
                connections.AppendValue(std::move(item));
            }
        }
        result["connections"] = std::move(connections);
        return result;
    }

    TOpRoot& Root;
    TString Cluster;
    const TSemanticSnapshotCatalogV1* Catalog;
    size_t Bytes = 0;
    size_t References = 0;
    size_t TypeNodes = 0;
    size_t TypeRenderWork = 0;
    size_t Items = 0;
    THashMap<const IOperator*, TString> OperatorIds;
    TVector<const IOperator*> Operators;
    THashMap<const TExprNode*, TString> ExpressionIds;
    TVector<const TExprNode*> Expressions;
};
