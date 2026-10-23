#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/opt/rbo/kqp_plan_conversion_utils.h>
#include <ydb/core/kqp/opt/kqp_opt.h>

#include "kqp_rbo_test_helpers.h"

#include <yql/essentials/public/langver/yql_langver.h>
#include <yql/essentials/providers/common/transform/yql_visit.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>
#include <optional>

namespace NKikimr::NKqp {

Y_UNIT_TEST_SUITE(KqpRboSetOperations) {
    Y_UNIT_TEST(ByNameAlignmentAndNullEqualSets) {
        NKikimrConfig::TAppConfig config;
        config.MutableTableServiceConfig()->SetEnableNewRBO(true);
        config.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
        config.MutableTableServiceConfig()->SetDefaultLangVer(NYql::GetMaxLangVersion());
        config.MutableTableServiceConfig()->SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
        TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
        auto tableSession = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto scheme = tableSession.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/SetValues` (
                id Int64 NOT NULL, v Int32, PRIMARY KEY (id)
            );
        )").GetValueSync();
        UNIT_ASSERT_C(scheme.IsSuccess(), scheme.GetIssues().ToString());
        NYdb::TValueBuilder rows;
        rows.BeginList();
        i64 id = 0;
        // Left: NULL, NULL, 1, 1, 2. Right: NULL, 1, 1, 3.
        for (const auto value : std::initializer_list<std::optional<i32>>{
                 std::nullopt, std::nullopt, 1, 1, 2, std::nullopt, 1, 1, 3}) {
            rows.AddListItem().BeginStruct()
                .AddMember("id").Int64(++id)
                .AddMember("v").OptionalInt32(value)
                .EndStruct();
        }
        rows.EndList();
        const auto upsert = kikimr.GetTableClient().BulkUpsert("/Root/SetValues", rows.Build()).GetValueSync();
        UNIT_ASSERT_C(upsert.IsSuccess(), upsert.GetIssues().ToString());

        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        const auto check = [&](const TString& query, const TString& expected,
                               std::initializer_list<NYdb::EPrimitiveType> types) {
            const auto result = session.ExecuteQuery("PRAGMA YqlSelect = 'force';\n" + query,
                NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            const auto& resultSet = result.GetResultSet(0);
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(resultSet), expected);
            UNIT_ASSERT_VALUES_EQUAL(resultSet.GetColumnsMeta().size(), types.size());
            size_t column = 0;
            for (const auto primitive : types) {
                NYdb::TTypeParser type(resultSet.GetColumnsMeta()[column++].Type);
                UNIT_ASSERT_VALUES_EQUAL(type.GetKind(), NYdb::TTypeParser::ETypeKind::Optional);
                type.OpenOptional();
                UNIT_ASSERT_VALUES_EQUAL(type.GetPrimitive(), primitive);
            }
        };
        const TString left = "SELECT v AS x FROM `/Root/SetValues` WHERE id <= 5";
        const TString right = "SELECT v AS x FROM `/Root/SetValues` WHERE id > 5";
        const auto int32 = NYdb::EPrimitiveType::Int32;

        // Column names, not positions, determine alignment and NULL padding.
        check(R"(
            SELECT a, b FROM (
                SELECT v AS a FROM `/Root/SetValues` WHERE id = 5
                UNION ALL
                SELECT v AS b FROM `/Root/SetValues` WHERE id = 9
            ) ORDER BY a, b;
        )", "[[#;[3]];[[2];#]]", {int32, int32});

        // Dots in public output names are not source aliases to discard.
        check(R"(
            SELECT q.`left.x` AS a, q.`right.x` AS b FROM (
                SELECT v AS `left.x` FROM `/Root/SetValues` WHERE id = 5
                UNION ALL
                SELECT v AS `right.x` FROM `/Root/SetValues` WHERE id = 9
            ) AS q ORDER BY a, b;
        )", "[[#;[3]];[[2];#]]", {int32, int32});

        // Both NULL and repeated non-NULL values compare as set elements.
        check("SELECT * FROM (" + left + " INTERSECT " + right + ") ORDER BY x;",
            "[[#];[[1]]]", {int32});
        check("SELECT * FROM (" + left + " EXCEPT " + right + ") ORDER BY x;",
            "[[[2]]]", {int32});
        check("SELECT * FROM (" + left + " UNION " + right + ") ORDER BY x;",
            "[[#];[[1]];[[2]];[[3]]]", {int32});

        // Non-null ordinary keys use semi/anti joins plus deduplication.
        const TString requiredLeft = "SELECT COALESCE(v, 0) AS x FROM `/Root/SetValues` WHERE id <= 5";
        const TString requiredRight = "SELECT COALESCE(v, 0) AS x FROM `/Root/SetValues` WHERE id > 5";
        check("SELECT IF(x >= 0, x, NULL) AS x FROM (" + requiredLeft + " INTERSECT " + requiredRight + ") ORDER BY x;",
            "[[[0]];[[1]]]", {int32});
        check("SELECT IF(x >= 0, x, NULL) AS x FROM (" + requiredLeft + " EXCEPT " + requiredRight + ") ORDER BY x;",
            "[[[2]]]", {int32});

        // Required Decimal is NOT an ordinary key: duplicate NaNs are peers.
        const TString nanRows = "SELECT Unwrap(CAST('nan' AS Decimal(15,4))) AS x FROM `/Root/SetValues` WHERE ";
        check("SELECT IF(c > 0, c, NULL) AS n FROM (SELECT COUNT(*) AS c FROM (" +
            nanRows + "id <= 2 INTERSECT " + nanRows + "id = 3));",
            "[[[1u]]]", {NYdb::EPrimitiveType::Uint64});

        // Only the padded all-NULL row is common when field names differ.
        check(R"(
            SELECT a, b FROM (
                SELECT v AS a FROM `/Root/SetValues` WHERE id <= 5
                INTERSECT
                SELECT v AS b FROM `/Root/SetValues` WHERE id > 5
            ) ORDER BY a, b;
        )", "[[#;#]]", {int32, int32});

        // Int64 (required) and Int32? share Optional<Int64>, without losing NULL.
        check(R"(
            SELECT * FROM (
                SELECT id AS x FROM `/Root/SetValues` WHERE id = 5
                UNION ALL
                SELECT v AS x FROM `/Root/SetValues` WHERE id IN (6, 9)
            ) ORDER BY x;
        )", "[[#];[[3]];[[5]]]", {NYdb::EPrimitiveType::Int64});
        // Equal-width inputs with reversed SELECT order still align by name.
        check(R"(
            SELECT a, b FROM (
                SELECT v AS b, v + 10 AS a FROM `/Root/SetValues` WHERE id = 5
                UNION ALL
                SELECT v + 20 AS a, v AS b FROM `/Root/SetValues` WHERE id = 9
            ) ORDER BY a, b;
        )", "[[[12];[2]];[[23];[3]]]", {int32, int32});

        // A nested producer adds a right-only name after two left columns.
        check(R"(
            SELECT a, b, c FROM (
                SELECT v AS c, v + 10 AS a FROM `/Root/SetValues` WHERE id = 5
                UNION ALL
                SELECT v AS b FROM `/Root/SetValues` WHERE id = 9
                UNION ALL
                SELECT v AS a FROM `/Root/SetValues` WHERE id = 6
            ) ORDER BY a, b, c;
        )", "[[#;#;#];[#;[3];#];[[12];#;[2]]]", {int32, int32, int32});

        // Same dotted public name survives both branches without alias stripping.
        check(R"(
            SELECT `a.b` AS x FROM (
                SELECT v AS `a.b` FROM `/Root/SetValues` WHERE id = 5
                UNION ALL
                SELECT v AS `a.b` FROM `/Root/SetValues` WHERE id = 9
            ) ORDER BY x;
        )", "[[[2]];[[3]]]", {int32});

        // Common optional Int64 conversion precedes set identity and dedup.
        const TString wideLeft = "SELECT CAST(v AS Int64) AS x FROM `/Root/SetValues` WHERE id <= 5";
        const TString wideRight = "SELECT CAST(v AS Int64) AS x FROM `/Root/SetValues` WHERE id > 5";
        check("SELECT * FROM (" + wideLeft + " UNION " + right + ") ORDER BY x;",
            "[[#];[[1]];[[2]];[[3]]]", {NYdb::EPrimitiveType::Int64});
        check("SELECT * FROM (" + left + " INTERSECT " + wideRight + ") ORDER BY x;",
            "[[#];[[1]]]", {NYdb::EPrimitiveType::Int64});
        check("SELECT * FROM (" + wideLeft + " EXCEPT " + right + ") ORDER BY x;",
            "[[[2]]]", {NYdb::EPrimitiveType::Int64});

        // A derived relation prefixes complete literal dotted output names.
        check(R"(
            SELECT q.`left.x` AS a, q.`right.x` AS b, q.x AS c FROM (
                SELECT v AS `left.x`, v + 10 AS `right.x`, v + 20 AS x
                FROM `/Root/SetValues` WHERE id = 5
            ) AS q;
        )", "[[[2];[12];[22]]]", {int32, int32, int32});
        // An actual input relation prefix is consumed by the SELECT boundary.
        // It must not leak into the public output name that q receives.
        check(R"(
            SELECT q.v AS x FROM (
                SELECT t.v FROM `/Root/SetValues` AS t WHERE t.id = 5
            ) AS q;
        )", "[[[2]]]", {int32});
        check(R"(
            SELECT z.`left.x` AS x FROM (
                SELECT q.`left.x` FROM (
                    SELECT v AS `left.x` FROM `/Root/SetValues` WHERE id = 5
                ) AS q
            ) AS z;
        )", "[[[2]]]", {int32});
        // Stripped names and stale source qualifiers must remain inaccessible.
        for (const auto& [query, column] : std::vector<std::pair<TString, TString>>{
            {R"(SELECT q.x FROM (SELECT v AS `left.x` FROM `/Root/SetValues`) AS q;)", "x"},
            {R"(SELECT q.`t.v` FROM (SELECT t.v FROM `/Root/SetValues` AS t) AS q;)", "t.v"},
        }) {
            const auto rejected = session.ExecuteQuery("PRAGMA YqlSelect='force';\n" + query,
                NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT_C(!rejected.IsSuccess(), query);
            const TString issues = rejected.GetIssues().ToString();
            UNIT_ASSERT_C(issues.Contains("No such column: " + column), issues);
        }

    }

    Y_UNIT_TEST(ReplaceAliasKeepsLiteralNamesAndEmptyAlias) {
        for (const TString alias : {"", "q"}) {
            NTests::TIdTestContext f;
            auto& ctx = f.ExprCtx;
            const std::vector<TString> names{"left.x", "right.x", "x"};
            const std::vector<const TTypeAnnotationNode*> types{
                ctx.MakeType<TDataExprType>(EDataSlot::Int32),
                ctx.MakeType<TDataExprType>(EDataSlot::Int64),
                ctx.MakeType<TDataExprType>(EDataSlot::Uint64)};
            auto empty = ctx.NewCallable(f.Pos, "KqpOpEmptySource", {});
            TExprNode::TListType elements;
            TVector<const TItemExprType*> inputFields, aliasFields;
            for (size_t i = 0; i < names.size(); ++i) {
                auto value = ctx.NewCallable(f.Pos, i == 0 ? "Int32" : i == 1 ? "Int64" : "Uint64", {ctx.NewAtom(f.Pos, "7")});
                value->SetTypeAnn(types[i]);
                auto lambda = ctx.NewLambda(f.Pos, ctx.NewArguments(f.Pos, {ctx.NewArgument(f.Pos, "row")}), std::move(value));
                elements.push_back(ctx.NewCallable(f.Pos, "KqpOpMapElementLambda", {
                    empty, ctx.NewAtom(f.Pos, names[i]), lambda, ctx.NewAtom(f.Pos, "False")}));
                inputFields.push_back(ctx.MakeType<TItemExprType>(names[i], types[i]));
                aliasFields.push_back(ctx.MakeType<TItemExprType>(alias.empty() ? names[i] : alias + "." + names[i], types[i]));
            }
            auto map = ctx.NewCallable(f.Pos, "KqpOpMap", {empty, ctx.NewList(f.Pos, std::move(elements)), ctx.NewAtom(f.Pos, "true")});
            map->SetTypeAnn(ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(inputFields)));
            auto aliased = ctx.NewCallable(f.Pos, "KqpOpReplaceAlias", {map, ctx.NewAtom(f.Pos, alias)});
            aliased->SetTypeAnn(ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(aliasFields)));
            auto renamed = ctx.NewCallable(f.Pos, "KqpOpReplaceColumns", {aliased,
                ctx.NewList(f.Pos, {ctx.NewAtom(f.Pos, "first"), ctx.NewAtom(f.Pos, "second"), ctx.NewAtom(f.Pos, "third")})});
            // Invoke the actual annotation handler: its GetSelectColumn path
            // must resolve the same names as the importer, even with alias="".
            auto transformer = NOpt::CreateKqpTypeAnnotationTransformer("ut", f.Tables, f.Config, f.TypeCtx);
            TExprNode::TPtr output;
            UNIT_ASSERT_C(transformer->Transform(renamed, output, ctx) == IGraphTransformer::TStatus::Ok,
                ctx.IssueManager.GetIssues().ToString());
            const auto* schema = renamed->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
            UNIT_ASSERT(schema->FindItemType("first") == types[0]);
            UNIT_ASSERT(schema->FindItemType("second") == types[1]);
            UNIT_ASSERT(schema->FindItemType("third") == types[2]);
            PlanConverter converter(f.TypeCtx, ctx);
            auto plan = converter.ExprNodeToOperator(renamed);
            const auto& projection = CastOperator<TOpMap>(*plan);
            const auto& rename = CastOperator<TOpMap>(*projection.GetInput());
            UNIT_ASSERT_VALUES_EQUAL(rename.GetMapElements().Items().size(), names.size());
            for (const auto& [id, expression] : rename.GetMapElements().Items()) {
                const auto original = converter.PlanProps.InfoUnitRegistry.Get(expression.GetColumnAccess()).GetFullName();
                const auto actual = converter.PlanProps.InfoUnitRegistry.Get(id).GetFullName();
                UNIT_ASSERT_VALUES_EQUAL(actual, alias.empty() ? original : alias + "." + original);
            }
            for (const auto& [id, expression] : projection.GetMapElements().Items()) {
                const auto name = converter.PlanProps.InfoUnitRegistry.Get(id).GetFullName();
                const auto index = name == "first" ? 0U : name == "second" ? 1U : 2U;
                const auto actual = converter.PlanProps.InfoUnitRegistry.Get(expression.GetColumnAccess()).GetFullName();
                UNIT_ASSERT_VALUES_EQUAL(actual, alias.empty() ? names[index] : alias + "." + names[index]);
            }
            NTests::AssertIdInvariants(*plan, converter.PlanProps);
        }
    }

    Y_UNIT_TEST(NestedByNameSetPositionalAnnotationMatchesImport) {
        for (const TString kind : {"union_all", "union", "intersect", "except"}) {
            for (const bool nestRight : {false, true}) {
                for (const TString alias : {"", "q"}) {
                    NTests::TIdTestContext f;
                    auto& ctx = f.ExprCtx;
                    auto transformer = NOpt::CreateKqpTypeAnnotationTransformer("ut", f.Tables, f.Config, f.TypeCtx);
                    const auto annotate = [&](const TExprNode::TPtr& node) {
                        TExprNode::TPtr output;
                        UNIT_ASSERT_C(transformer->Transform(node, output, ctx) == IGraphTransformer::TStatus::Ok,
                            ctx.IssueManager.GetIssues().ToString());
                        UNIT_ASSERT(output == node);
                    };
                    const std::vector<TString> names{"omega", "left.x", "right.x", "x"};
                    const std::vector<EDataSlot> slots{EDataSlot::Int32, EDataSlot::Int64, EDataSlot::Uint32, EDataSlot::Uint64};
                    const std::vector<TString> constructors{"Int32", "Int64", "Uint32", "Uint64"};
                    TVector<const TTypeAnnotationNode*> types;
                    for (const auto slot : slots) {
                        types.push_back(ctx.MakeType<TOptionalExprType>(ctx.MakeType<TDataExprType>(slot)));
                    }
                    const auto leaf = [&](std::initializer_list<size_t> indices) {
                        auto empty = ctx.NewCallable(f.Pos, "KqpOpEmptySource", {});
                        TExprNode::TListType elements;
                        TVector<const TItemExprType*> fields;
                        for (const auto index : indices) {
                            auto value = ctx.NewCallable(f.Pos, constructors[index], {ctx.NewAtom(f.Pos, "7")});
                            value->SetTypeAnn(types[index]->Cast<TOptionalExprType>()->GetItemType());
                            auto lambda = ctx.NewLambda(f.Pos, ctx.NewArguments(f.Pos, {ctx.NewArgument(f.Pos, "row")}), std::move(value));
                            elements.push_back(ctx.NewCallable(f.Pos, "KqpOpMapElementLambda", {
                                empty, ctx.NewAtom(f.Pos, names[index]), lambda, ctx.NewAtom(f.Pos, "True")}));
                            fields.push_back(ctx.MakeType<TItemExprType>(names[index], types[index]));
                        }
                        auto map = ctx.NewCallable(f.Pos, "KqpOpMap", {
                            empty, ctx.NewList(f.Pos, std::move(elements)), ctx.NewAtom(f.Pos, "true")});
                        map->SetTypeAnn(ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(fields)));
                        return map;
                    };
                    const auto makeSet = [&](const TExprNode::TPtr& left, const TExprNode::TPtr& right) {
                        auto set = ctx.NewCallable(f.Pos, "KqpOpSetOp", {left, right, ctx.NewAtom(f.Pos, kind)});
                        annotate(set);
                        return set;
                    };
                    auto a = leaf({0, 1}), b = leaf({2, 0}), c = leaf({3, 1});
                    auto set = nestRight ? makeSet(a, makeSet(b, c)) : makeSet(makeSet(a, b), c);
                    auto aliased = ctx.NewCallable(f.Pos, "KqpOpReplaceAlias", {set, ctx.NewAtom(f.Pos, alias)});
                    annotate(aliased);
                    auto count = ctx.NewCallable(f.Pos, "Uint64", {ctx.NewAtom(f.Pos, "9")});
                    count->SetTypeAnn(ctx.MakeType<TDataExprType>(EDataSlot::Uint64));
                    auto limited = ctx.NewCallable(f.Pos, "KqpOpLimit", {aliased, count});
                    annotate(limited);
                    auto sorted = ctx.NewCallable(f.Pos, "KqpOpSort", {limited, ctx.NewList(f.Pos, {})});
                    annotate(sorted);
                    auto renamed = ctx.NewCallable(f.Pos, "KqpOpReplaceColumns", {sorted,
                        ctx.NewList(f.Pos, {ctx.NewAtom(f.Pos, "first"), ctx.NewAtom(f.Pos, "second"),
                            ctx.NewAtom(f.Pos, "third"), ctx.NewAtom(f.Pos, "fourth")})});
                    annotate(renamed);
                    const auto* schema = renamed->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                    const std::vector<TString> renamedNames{"first", "second", "third", "fourth"};
                    for (size_t i = 0; i < names.size(); ++i) {
                        UNIT_ASSERT(schema->FindItemType(renamedNames[i]) == types[i]);
                    }
                    PlanConverter converter(f.TypeCtx, ctx);
                    auto plan = converter.ExprNodeToOperator(renamed);
                    const auto& projection = CastOperator<TOpMap>(*plan);
                    UNIT_ASSERT_VALUES_EQUAL(projection.GetMapElements().Items().size(), names.size());
                    for (const auto& [id, expression] : projection.GetMapElements().Items()) {
                        const auto output = converter.PlanProps.InfoUnitRegistry.Get(id).GetFullName();
                        const auto position = std::find(renamedNames.begin(), renamedNames.end(), output);
                        UNIT_ASSERT(position != renamedNames.end());
                        const auto index = std::distance(renamedNames.begin(), position);
                        const auto source = converter.PlanProps.InfoUnitRegistry.Get(expression.GetColumnAccess()).GetFullName();
                        UNIT_ASSERT_VALUES_EQUAL(source, alias.empty() ? names[index] : alias + "." + names[index]);
                        UNIT_ASSERT(projection.GetInput()->GetOutputIUs().Contains(expression.GetColumnAccess()));
                    }
                    NTests::AssertIdInvariants(*plan, converter.PlanProps);
                    auto wrongWidth = ctx.NewCallable(f.Pos, "KqpOpReplaceColumns", {sorted,
                        ctx.NewList(f.Pos, {ctx.NewAtom(f.Pos, "first")})});
                    TExprNode::TPtr output;
                    UNIT_ASSERT_EXCEPTION_CONTAINS(transformer->Transform(wrongWidth, output, ctx), yexception,
                        "Positional column count does not match the input schema");
                    auto repeated = ctx.NewCallable(f.Pos, "KqpOpMap", {a->HeadPtr(),
                        ctx.NewList(f.Pos, {a->Child(1)->ChildPtr(0), a->Child(1)->ChildPtr(0)}), ctx.NewAtom(f.Pos, "true")});
                    repeated->SetTypeAnn(a->GetTypeAnn());
                    auto repeatedRename = ctx.NewCallable(f.Pos, "KqpOpReplaceColumns", {repeated,
                        ctx.NewList(f.Pos, {ctx.NewAtom(f.Pos, "first"), ctx.NewAtom(f.Pos, "second")})});
                    UNIT_ASSERT_EXCEPTION_CONTAINS(transformer->Transform(repeatedRename, output, ctx), yexception,
                        "Duplicate SELECT projection column");
                }
            }
        }
        // Each layer references the same child twice. Keep the DAG small: this
        // is a sharing/name/type control, not a timeout-based speed benchmark.
        // Exercise annotation only: importing the multiset is a separate task.
        {
            NTests::TIdTestContext f;
            auto& ctx = f.ExprCtx;
            auto transformer = NOpt::CreateKqpTypeAnnotationTransformer("ut", f.Tables, f.Config, f.TypeCtx);
            const auto annotate = [&](const TExprNode::TPtr& node) {
                TExprNode::TPtr output;
                UNIT_ASSERT_C(transformer->Transform(node, output, ctx) == IGraphTransformer::TStatus::Ok,
                    ctx.IssueManager.GetIssues().ToString());
            };
            const auto* type = ctx.MakeType<TDataExprType>(EDataSlot::Int32);
            auto empty = ctx.NewCallable(f.Pos, "KqpOpEmptySource", {});
            auto value = ctx.NewCallable(f.Pos, "Int32", {ctx.NewAtom(f.Pos, "7")});
            value->SetTypeAnn(type);
            auto lambda = ctx.NewLambda(f.Pos, ctx.NewArguments(f.Pos, {ctx.NewArgument(f.Pos, "row")}), std::move(value));
            auto element = ctx.NewCallable(f.Pos, "KqpOpMapElementLambda", {
                empty, ctx.NewAtom(f.Pos, "public.x"), lambda, ctx.NewAtom(f.Pos, "False")});
            auto node = ctx.NewCallable(f.Pos, "KqpOpMap", {empty,
                ctx.NewList(f.Pos, {element}), ctx.NewAtom(f.Pos, "true")});
            node->SetTypeAnn(ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(
                TVector<const TItemExprType*>{ctx.MakeType<TItemExprType>("public.x", type)})));
            for (size_t i = 0; i < 12; ++i) {
                node = ctx.NewCallable(f.Pos, "KqpOpSetOp", {node, node, ctx.NewAtom(f.Pos, "union_all")});
                UNIT_ASSERT(node->Child(0) == node->Child(1));
                annotate(node);
            }
            auto aliased = ctx.NewCallable(f.Pos, "KqpOpReplaceAlias", {node, ctx.NewAtom(f.Pos, "q")});
            annotate(aliased);
            auto renamed = ctx.NewCallable(f.Pos, "KqpOpReplaceColumns", {aliased,
                ctx.NewList(f.Pos, {ctx.NewAtom(f.Pos, "result")})});
            annotate(renamed);
            const auto* schema = renamed->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
            UNIT_ASSERT_VALUES_EQUAL(schema->GetSize(), 1);
            UNIT_ASSERT(schema->FindItemType("result") == type);
        }
    }

    Y_UNIT_TEST(ByNameSetInputsRebindCompleteProjection) {
        for (const TString kind : {"union_all", "union", "intersect", "except"}) {
            NTests::TIdTestContext f;
            auto& ctx = f.ExprCtx;
            const auto* int32 = ctx.MakeType<TDataExprType>(EDataSlot::Int32);
            const auto* int64 = ctx.MakeType<TDataExprType>(EDataSlot::Int64);
            const auto* optional32 = ctx.MakeType<TOptionalExprType>(int32);
            const auto* optional64 = ctx.MakeType<TOptionalExprType>(int64);
            const auto row = [&](TVector<const TItemExprType*> items) {
                return ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(items));
            };
            const auto leaf = [&](bool right) {
                auto empty = ctx.NewCallable(f.Pos, "KqpOpEmptySource", {});
                const auto element = [&](TStringBuf name, bool nullable) {
                    auto value = ctx.NewCallable(f.Pos, right ? "Int64" : "Int32", {ctx.NewAtom(f.Pos, "7")});
                    value->SetTypeAnn(right ? int64 : int32);
                    auto lambda = ctx.NewLambda(f.Pos, ctx.NewArguments(f.Pos, {ctx.NewArgument(f.Pos, "row")}), std::move(value));
                    return ctx.NewCallable(f.Pos, "KqpOpMapElementLambda", {
                        empty, ctx.NewAtom(f.Pos, name), lambda, ctx.NewAtom(f.Pos, nullable ? "True" : "False")});
                };
                // Projection order intentionally differs from sorted field order.
                auto map = ctx.NewCallable(f.Pos, "KqpOpMap", {empty, ctx.NewList(f.Pos,
                    right ? TExprNode::TListType{element("right.x", true), element("omega", false)}
                          : TExprNode::TListType{element("omega", false), element("left.x", true)}), ctx.NewAtom(f.Pos, "true")});
                map->SetTypeAnn(right ? row({ctx.MakeType<TItemExprType>("omega", int64), ctx.MakeType<TItemExprType>("right.x", optional64)})
                                     : row({ctx.MakeType<TItemExprType>("left.x", optional32), ctx.MakeType<TItemExprType>("omega", int32)}));
                return map;
            };
            auto left = leaf(false), right = leaf(true);
            const TTypeAnnotationNode* leftType = left->GetTypeAnn();
            const TTypeAnnotationNode* rightType = right->GetTypeAnn();
            auto set = ctx.NewCallable(f.Pos, "KqpOpSetOp", {left, right, ctx.NewAtom(f.Pos, kind)});
            set->SetTypeAnn(row({ctx.MakeType<TItemExprType>("left.x", optional32),
                ctx.MakeType<TItemExprType>("omega", int64), ctx.MakeType<TItemExprType>("right.x", optional64)}));
            auto renamed = ctx.NewCallable(f.Pos, "KqpOpReplaceColumns", {set,
                ctx.NewList(f.Pos, {ctx.NewAtom(f.Pos, "first"), ctx.NewAtom(f.Pos, "second"), ctx.NewAtom(f.Pos, "third")})});
            PlanConverter converter(f.TypeCtx, ctx);
            auto plan = converter.ExprNodeToOperator(renamed);
            const auto& rename = CastOperator<TOpMap>(*plan);
            UNIT_ASSERT_VALUES_EQUAL(rename.GetMapElements().Items().size(), 3);
            for (const auto& [output, expression] : rename.GetMapElements().Items()) {
                const auto name = converter.PlanProps.InfoUnitRegistry.Get(output).GetFullName();
                const auto original = converter.PlanProps.InfoUnitRegistry.Get(expression.GetColumnAccess()).GetFullName();
                UNIT_ASSERT_VALUES_EQUAL_C(original, name == "first" ? "omega" : name == "second" ? "left.x" : "right.x", kind);
                UNIT_ASSERT(rename.GetInput()->GetOutputIUs().Contains(expression.GetColumnAccess()));
            }
            UNIT_ASSERT(left->GetTypeAnn() == leftType && right->GetTypeAnn() == rightType);
            NTests::AssertIdInvariants(*plan, converter.PlanProps);
        }
    }

    Y_UNIT_TEST(CommonTypeIdentityAndExactOutputAcrossBackends) {
        struct TCase {
            TString LeftType, RightType, OutputType;
            std::vector<std::optional<std::string>> Left, Right, Intersection, LeftDifference, RightDifference;
        };
        const std::vector<TCase> cases{
            // The two source Int64 values differ, but conversion to the common
            // Double type makes 2^53+1 equal to the right-side 2^53 value.
            {"Int64", "Double", "Double",
                {"9007199254740993", "9007199254740993", "9007199254740994"}, {"9007199254740992"},
                {"9007199254740992"}, {"9007199254740994"}, {}},
            {"Int64", "Uint32", "Int64",
                {"-1", "4294967295", "4294967295"}, {"4294967295", "7"},
                {"4294967295"}, {"-1"}, {"7"}},
            // No explicit output cast: assert the inferred Decimal(10,1)
            // payload and preserve NaN, infinities, fractional differences.
            {"Decimal(7,0)", "Decimal(10,1)", "Decimal(10,1)",
                {"nan", "nan", "7", "7", "10", "inf", "-inf"},
                {"nan", "7.0", "10.5", "1.0", "inf", "-inf"},
                {"nan", "7.0", "inf", "-inf"}, {"10.0"}, {"10.5", "1.0"}},
            // Raw Decimal codes 10 and 1.0 coincide before scale conversion.
            // NULL and NaN are peers, but 10 must not match 1.0.
            {"Decimal(7,0)?", "Decimal(10,1)?", "Decimal(10,1)?",
                {std::nullopt, "nan", "1", "1", "10"}, {std::nullopt, "nan", "1.0", "10.5"},
                {std::nullopt, "nan", "1.0"}, {"10.0"}, {"10.5"}},
        };
        const auto values = [](const TString& type, const auto& input) {
            NYdb::TValueBuilder result;
            result.BeginList();
            for (const auto& value : input) {
                result.AddListItem().BeginStruct().AddMember("x");
                if (type.StartsWith("Decimal")) {
                    const bool small = type.StartsWith("Decimal(7,");
                    const ui8 precision = small ? 7 : 10, scale = small ? 0 : 1;
                    const bool nullable = type.EndsWith("?");
                    if (!value) {
                        UNIT_ASSERT(nullable);
                        result.EmptyOptional(NYdb::TTypeBuilder().Decimal(NYdb::TDecimalType(precision, scale)).Build());
                    } else {
                        if (nullable) { result.BeginOptional(); }
                        result.Decimal(NYdb::TDecimalValue(*value, precision, scale));
                        if (nullable) { result.EndOptional(); }
                    }
                } else {
                    UNIT_ASSERT(value);
                    if (type == "Double") { result.Double(std::stod(*value)); }
                    else if (type == "Uint32") { result.Uint32(std::stoul(*value)); }
                    else { UNIT_ASSERT_VALUES_EQUAL(type, "Int64"); result.Int64(std::stoll(*value)); }
                }
                result.EndStruct();
            }
            return result.EndList().Build();
        };
        for (const TString mode : {"grace", "block", "map"}) {
            NKikimrConfig::TAppConfig config;
            auto* service = config.MutableTableServiceConfig();
            service->SetEnableNewRBO(true);
            service->SetEnableFallbackToYqlOptimizer(false);
            service->SetEnableFallbackOnDML(false);
            service->SetEnableFallbackOnMultipleStatements(false);
            service->SetDefaultLangVer(202603);
            service->SetDefaultCostBasedOptimizationLevel(0);
            service->SetUseBlockHashJoin(mode == "block");
            TKikimrRunner kikimr(TKikimrSettings(config).SetWithSampleTables(false));
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            const auto counters = TKqpCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters).GetKqpCounters();
            const auto successes = counters->GetCounter("Compilation/NewRBO/Success");
            const auto failures = counters->GetCounter("Compilation/NewRBO/Failed");
            bool observedNewRbo = false;
            for (const auto& item : cases) {
                for (const bool reverse : {false, true}) {
                    const auto params = NYdb::TParamsBuilder()
                        .AddParam("$lhs", values(reverse ? item.RightType : item.LeftType, reverse ? item.Right : item.Left))
                        .AddParam("$rhs", values(reverse ? item.LeftType : item.RightType, reverse ? item.Left : item.Right)).Build();
                    for (const bool difference : {false, true}) {
                        const TString query = TStringBuilder()
                            << "PRAGMA YqlSelect='force'; PRAGMA ydb.CostBasedOptimizationLevel='0'; "
                            << "PRAGMA ydb.HashJoinMode='" << (mode == "map" ? "map" : "grace") << "'; "
                            << "DECLARE $lhs AS List<Struct<x:" << (reverse ? item.RightType : item.LeftType) << ">>; "
                            << "DECLARE $rhs AS List<Struct<x:" << (reverse ? item.LeftType : item.RightType) << ">>; "
                            << "SELECT x FROM AS_TABLE($lhs) " << (difference ? "EXCEPT" : "INTERSECT")
                            << " SELECT x FROM AS_TABLE($rhs);";
                        const auto beforeSuccess = successes->Val(), beforeFailure = failures->Val();
                        const auto explain = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(), params,
                            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NYdb::NQuery::EExecMode::Explain)).GetValueSync();
                        UNIT_ASSERT_C(explain.IsSuccess(), query << "\n" << explain.GetIssues().ToString());
                        if (!observedNewRbo) { UNIT_ASSERT(successes->Val() > beforeSuccess); observedNewRbo = true; }
                        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
                        UNIT_ASSERT(explain.GetStats() && explain.GetStats()->GetAst());
                        const TString ast{*explain.GetStats()->GetAst()};
                        const TString core = mode == "block" ? "BlockHashJoinCore" : mode == "map" ? "MapJoinCore" : "GraceJoinCore";
                        UNIT_ASSERT_C(ast.Contains(core) && ast.Contains("(declare $lhs ") && ast.Contains("(declare $rhs "), ast);
                        if (item.OutputType.StartsWith("Decimal")) { UNIT_ASSERT_C(ast.Contains("StablePickle"), ast); }
                        Cerr << "[SET-COMMON-TYPE] mode=" << mode << " reverse=" << reverse << "\n" << query << "\n" << ast << Endl;
                        const auto result = session.ExecuteQuery(query,
                            NYdb::NQuery::TTxControl::BeginTx().CommitTx(), params).GetValueSync();
                        UNIT_ASSERT_C(result.IsSuccess(), query << "\n" << result.GetIssues().ToString());
                        UNIT_ASSERT_VALUES_EQUAL(failures->Val(), beforeFailure);
                        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                        const auto& rs = result.GetResultSet(0);
                        UNIT_ASSERT(!rs.Truncated());
                        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta().size(), 1);
                        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Name, "x");
                        UNIT_ASSERT_VALUES_EQUAL(rs.GetColumnsMeta()[0].Type.ToString(), item.OutputType);
                        std::vector<std::optional<std::string>> actual, expected;
                        NYdb::TResultSetParser parser(rs);
                        while (parser.TryNextRow()) {
                            if (item.OutputType == "Double") { actual.emplace_back(std::to_string(parser.ColumnParser(0).GetDouble())); }
                            else if (item.OutputType == "Int64") { actual.emplace_back(std::to_string(parser.ColumnParser(0).GetInt64())); }
                            else if (item.OutputType.EndsWith("?")) {
                                const auto value = parser.ColumnParser(0).GetOptionalDecimal();
                                actual.push_back(value ? std::optional<std::string>(value->ToString()) : std::nullopt);
                            } else { actual.emplace_back(parser.ColumnParser(0).GetDecimal().ToString()); }
                        }
                        const auto& expectedValues = !difference ? item.Intersection : reverse ? item.RightDifference : item.LeftDifference;
                        for (const auto& value : expectedValues) {
                            if (!value) { expected.emplace_back(std::nullopt); }
                            else if (item.OutputType == "Double") { expected.emplace_back(std::to_string(std::stod(*value))); }
                            else if (item.OutputType == "Int64") { expected.emplace_back(*value); }
                            else { expected.emplace_back(NYdb::TDecimalValue(*value, 10, 1).ToString()); }
                        }
                        std::sort(actual.begin(), actual.end());
                        std::sort(expected.begin(), expected.end());
                        UNIT_ASSERT_VALUES_EQUAL_C(actual, expected, query << "\n" << FormatResultSetYson(rs) << "\n" << ast);
                    }
                }
            }
        }
    }

}

} // namespace NKikimr::NKqp
