// Ported from PR #58587 at fbeb81657ef. Keep SQL and structural assertions;
// native type/DAG checks replace YQL lambda annotation at the same boundaries.
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>


namespace NKikimr::NKqp {
namespace {

// This fixture bypasses host fallback and imports the new AST directly. Its
// common checks validate IU definition/use, native expression ownership and
// PlanProps lifetime for every operator, not just the visible result columns.
struct TFixture : NTests::TIdTestContext {
    TString Sql;

    TIntrusivePtr<TOpRoot> Run(const TString& sql, bool orderedColumns = false) {
        Sql = sql;
        return Lower(orderedColumns);
    }

    TIntrusivePtr<TOpRoot> Lower(bool orderedColumns = false) {
        const auto nodes = ExprCtx.NodeAllocationCounter;
        const auto types = ExprCtx.TypeNodes.size();
        auto query = CompileNativeToRbo(std::string(orderedColumns ? "PRAGMA OrderedColumns; " : "") + Sql.c_str(),
            [](std::string_view, NFrontend::TSourcePosition) {
                using K = NFrontend::ETypeKind;
                NFrontend::TCatalogTable schema{"/Root/t", {{"id", {K::Int64}}, {"value", {K::String, true}}, {"a", {K::Int32}}},
                    NFrontend::TWriteSchema{{"id"}, {}, false}};
                return TNativeTable{std::move(schema), TRboTable{"/Root/t", "1:1", "", "1"}, NYql::EStorageType::RowStorage};
            }, ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(ExprCtx.TypeNodes.size(), types);
        auto root = query.Roots.front();
        NTests::AssertIdInvariants(*root, root->PlanProps);
        for (const auto& item : IterateSubtreeWithSubplans(root.Get(), root->PlanProps)) {
            for (const auto& expression : item.Current->GetExpressions()) {
                UNIT_ASSERT(expression.get().IsNative());
                UNIT_ASSERT_EQUAL(expression.get().PlanProps, &root->PlanProps);
            }
        }
        return root;
    }

    IGraphTransformer::TStatus Recompute(TOpRoot& root) {
        const auto nodes = ExprCtx.NodeAllocationCounter;
        const auto types = ExprCtx.TypeNodes.size();
        const auto status = root.ComputeTypes(RboCtx);
        UNIT_ASSERT_VALUES_EQUAL(ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(ExprCtx.TypeNodes.size(), types);
        return status;
    }

    TVector<IOperator*> Find(TOpRoot& root, EOperator kind) {
        TVector<IOperator*> result;
        for (const auto& item : root) {
            if (item.Current->Kind == kind) { result.push_back(item.Current); }
        }
        return result;
    }
};

} // namespace

Y_UNIT_TEST_SUITE(KqpNativeReferenceRboLowering) {
    Y_UNIT_TEST(DoesNotResetCallerCompilationStages) {
        TFixture f;
        f.ExprCtx.Step.Done(TExprStep::LoadTablesMetadata);
        f.ExprCtx.Step.Done(TExprStep::ExpandApplyForLambdas);
        f.ExprCtx.RepeatTransformCounter = 17;
        auto root = f.Run("SELECT id + 1, value ?? 'none' FROM t WHERE a > 1;");
        UNIT_ASSERT(f.Recompute(*root) == IGraphTransformer::TStatus::Ok);
        UNIT_ASSERT(f.ExprCtx.Step.IsDone(TExprStep::LoadTablesMetadata));
        UNIT_ASSERT(f.ExprCtx.Step.IsDone(TExprStep::ExpandApplyForLambdas));
        UNIT_ASSERT(!f.ExprCtx.Step.IsDone(TExprStep::Configure));
        UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.RepeatTransformCounter, 17);
    }

    Y_UNIT_TEST(WriteDefaultsReachTheExistingEffectWithoutBackendExpressions) {
        NTests::TIdTestContext f;
        const auto nodes = f.ExprCtx.NodeAllocationCounter;
        const auto types = f.ExprCtx.TypeNodes.size();
        auto query = CompileNativeToRbo("INSERT INTO t (value) VALUES ('row') RETURNING id, a", [](auto, auto) {
            using namespace NFrontend;
            TCatalogTable schema{"/Root/t", {{"id", {ETypeKind::Int64}}, {"value", {ETypeKind::String, true}}, {"a", {ETypeKind::Int32, true}}},
                TWriteSchema{{"id"}, {{"id", EColumnDefault::Sequence}, {"a", EColumnDefault::Literal}}, false}};
            return TNativeTable{std::move(schema), TRboTable{"/Root/t", "1:1", "", "1"}, NYql::EStorageType::RowStorage};
        }, f.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 1);
        auto& root = *query.Roots.front();
        NTests::AssertIdInvariants(root, root.PlanProps);
        UNIT_ASSERT(root.ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
        UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
        const auto& effect = CastOperator<TOpTableEffect>(*root.GetInput());
        UNIT_ASSERT(effect.Options.DefaultColumns);
        UNIT_ASSERT(*effect.Options.DefaultColumns == (TVector<TString>{"id", "a"}));
        UNIT_ASSERT_VALUES_EQUAL(effect.GetColumns().Items().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(effect.GetReturningColumns().Items().size(), 2);
    }

    Y_UNIT_TEST(DeclarationsAndParameterTablesLowerDirectly) {
        for (const TString sql : {
            "DECLARE $x AS Decimal(22,9)?; DECLARE $s AS String; SELECT $x AS x, $s AS s;",
            "PRAGMA YqlSelectAllowUnnamedGroupByExpr; DECLARE $x AS Int64; SELECT $x AS k, SUM(id + $x) FROM t GROUP BY $x;",
            "DECLARE $items AS List<Struct<>>; SELECT COUNT(*) FROM AS_TABLE($items);",
            "SELECT CAST(RandomUuid(id) AS Utf8) AS uuid FROM t;",
            "SELECT RandomNumber(id, value) AS number FROM t;",
            "SELECT l.id, r.id, s.id FROM t AS l LEFT JOIN t AS r ON CAST(l.a AS Int64) = r.id "
                "LEFT JOIN t AS s ON CAST(l.a AS Int64) = s.id;",
            "SELECT MIN_BY(value, id), MAX_BY(id, value) FROM t;",
            "SELECT a, MAX_BY(value, id), MIN_BY(value, value) FROM t GROUP BY a;",
            "SELECT MAX_BY(id, NULL), MIN_BY(NULL, id) FROM t;",
            "SELECT a, MIN_BY(value, id) FROM t GROUP BY ROLLUP(a);",
            "DECLARE $items AS List<Struct<'id':Int64,'value':String?,'a':Int32>>; "
                "SELECT r.id, r.value FROM AS_TABLE($items) AS r WHERE r.a > 0;",
            "DECLARE $items AS List<Struct<'id':Int64,'value':String?,'a':Int32>>; "
                "INSERT INTO t SELECT * FROM AS_TABLE($items);",
            "DECLARE $id AS Int64; UPDATE t SET value = 'changed', a = a + 1 WHERE id = $id;",
            "UPDATE t ON SELECT id, a + 1 AS a FROM t WHERE id > 0;",
            "DECLARE $id AS Int64; DELETE FROM t WHERE id = $id;"}) {
            TFixture f;
            auto root = f.Run(sql);
            try {
                UNIT_ASSERT_C(f.Recompute(*root) == IGraphTransformer::TStatus::Ok,
                    sql << ": " << f.ExprCtx.IssueManager.GetIssues().ToString());
            } catch (const std::exception& e) {
                UNIT_FAIL(sql << ": " << e.what() << ": " << f.ExprCtx.IssueManager.GetIssues().ToString());
            }
            f.Lower();
        }
    }

    Y_UNIT_TEST(SetProjectionOrderSurvivesTypeRecomputation) {
        for (const TString op : {"UNION ALL", "UNION", "INTERSECT", "EXCEPT"}) {
            TFixture f;
            auto root = f.Run("SELECT DISTINCT value AS z, id AS a FROM t " + op
                + " SELECT value AS z, a AS a FROM t ORDER BY a + 1 DESC, z LIMIT 2 OFFSET 1;");
            UNIT_ASSERT_C(f.Recompute(*root) == IGraphTransformer::TStatus::Ok, f.ExprCtx.IssueManager.GetIssues().ToString());
            const auto& columns = root->GetColumns().Items();
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(columns[0].second, "z");
            UNIT_ASSERT_VALUES_EQUAL(columns[1].second, "a");
            // Look up fields by IU identity: the annotated struct does not
            // retain the order in root's explicit result-column contract.
            UNIT_ASSERT(root->GetInput()->GetNativeIUType(columns[0].first) == (NFrontend::TType{NFrontend::ETypeKind::String, true}));
            UNIT_ASSERT(root->GetInput()->GetNativeIUType(columns[1].first) == (NFrontend::TType{NFrontend::ETypeKind::Int64}));
            auto* sort = static_cast<TOpSort*>(f.Find(*root, EOperator::Sort).at(0));
            UNIT_ASSERT_VALUES_EQUAL(sort->GetSortElements().Items()[1].first, columns[0].first);
            UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Limit).size(), 1);
            f.Lower();
        }
    }

    // Recompute every operator type as well as validating IU ownership. This
    // catches mismatches between scalar annotations and relational result types.
    Y_UNIT_TEST(RelationalExpressionsLowerAndReannotate) {
        for (const TString sql : {
            "SELECT value, SUM(id) AS s, COUNT(DISTINCT a) FROM t GROUP BY value HAVING s > 0 ORDER BY s;",
            "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT a + 1 AS k, SUM(id) FROM t GROUP BY a + 1;",
            "SELECT a, GROUPING(a), SUM(id) FROM t GROUP BY ROLLUP(a);",
            "SELECT a, SUM(SUM(id)) OVER (), RANK() OVER (ORDER BY SUM(id)) FROM t GROUP BY a;",
            "SELECT id FROM t UNION ALL SELECT a FROM t;",
            "SELECT id, id AS widened, value FROM t UNION ALL SELECT id, a, value FROM t;",
            "SELECT id, value FROM t UNION ALL SELECT id, value FROM t UNION ALL SELECT id, value FROM t;",
            "SELECT value FROM t INTERSECT SELECT value FROM t;",
            "SELECT l.id, (SELECT MAX(r.id) FROM t AS r WHERE r.id = l.id) FROM t AS l;",
            "SELECT l.id + (SELECT MAX(r.id) FROM t AS r WHERE r.id = l.id) "
                "+ (SELECT MAX(s.id) FROM t AS s WHERE s.a = l.a), l.id FROM t AS l;",
            "SELECT l.id, (SELECT (SELECT MAX(z.id) FROM t AS z WHERE z.id = l.id) "
                "FROM t AS r LIMIT 1) FROM t AS l;",
            "SELECT l.id FROM t AS l WHERE EXISTS (SELECT r.id FROM t AS r WHERE r.id = l.id);",
            "SELECT COUNT(*) FROM t AS l WHERE EXISTS (SELECT * FROM t AS r WHERE r.id = l.id AND r.a != l.a) "
                "AND NOT EXISTS (SELECT * FROM t AS r WHERE r.id = l.id);",
            "SELECT id FROM t WHERE id IN (SELECT id FROM t);"}) {
            TFixture f;
            auto root = f.Run(sql);
            UNIT_ASSERT_C(f.Recompute(*root) == IGraphTransformer::TStatus::Ok,
                sql << ": " << f.ExprCtx.IssueManager.GetIssues().ToString());
            f.Lower();
        }
    }

    // ORDER BY uses an unprojected computed value; the map must retain its input
    // and count literals must already be visible to later pushdown rules.
    Y_UNIT_TEST(ProjectionSortAndLimitKeepHiddenDependencies) {
        TFixture f;
        auto root = f.Run("PRAGMA YqlSelect = 'force'; SELECT id + 1 AS next FROM t WHERE id > 0 ORDER BY a + 2 DESC LIMIT 5 OFFSET 1;");
        UNIT_ASSERT_VALUES_EQUAL(root->GetColumns().Items().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(root->GetColumns().Items()[0].second, "next");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Filter).size(), 1);
        auto* sort = static_cast<TOpSort*>(f.Find(*root, EOperator::Sort).at(0));
        UNIT_ASSERT(!sort->GetSortElements().Unordered().HasAny(root->GetOutputIUs()));
        auto* limit = static_cast<TOpLimit*>(f.Find(*root, EOperator::Limit).at(0));
        UNIT_ASSERT(limit->HasOffset());
        UNIT_ASSERT(limit->GetLimitCond().GetRawInputIUs().Empty());
        const auto& countExpression = limit->GetLimitCond();
        const auto& count = (*countExpression.GetNativeArena())[countExpression.GetNativeRoot()];
        UNIT_ASSERT(count.Op == NFrontend::EOpcode::Literal);
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(count.Value), 5);
        const auto& offsetExpression = *limit->GetOffsetCond();
        const auto& offset = (*offsetExpression.GetNativeArena())[offsetExpression.GetNativeRoot()];
        UNIT_ASSERT(offset.Op == NFrontend::EOpcode::Literal);
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(offset.Value), 1);
    }

    // Check both RBO type recomputation and a second import from the same inputs
    // to detect consumed AST nodes or mutated annotation lambdas.
    Y_UNIT_TEST(SelectWithoutSourceCanBeReannotated) {
        TFixture f;
        auto root = f.Run("SELECT [1, 2][0u] AS v, CAST(3 AS Int64) + 2 AS n LIMIT 1;");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::EmptySource).size(), 1);
        UNIT_ASSERT_C(f.Recompute(*root) == IGraphTransformer::TStatus::Ok, f.ExprCtx.IssueManager.GetIssues().ToString());
        auto again = f.Lower();
        UNIT_ASSERT_VALUES_EQUAL(again->GetColumns().Items().size(), 2);

    }

    // Reversed equality operands still form a key, while the remaining outer-join
    // condition must stay on the join rather than disappear or move to WHERE.
    Y_UNIT_TEST(SelfJoinExtractsKeysAndRetainsResidual) {
        TFixture f;
        auto root = f.Run("SELECT l.id, r.value FROM t AS l LEFT JOIN t AS r ON r.id = l.id AND r.a > 1;");
        auto* join = static_cast<TOpJoin*>(f.Find(*root, EOperator::Join).at(0));
        UNIT_ASSERT_VALUES_EQUAL(join->JoinKind, "Left");
        UNIT_ASSERT_VALUES_EQUAL(join->JoinKeys.Items().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(join->JoinFilters.size(), 1);
        UNIT_ASSERT(!join->GetLeftInput()->GetOutputIUs().HasAny(join->GetRightInput()->GetOutputIUs()));
    }

    // Shared computation and independent consumer identity are separate invariants:
    // one read should feed two ports with disjoint IU sets.
    Y_UNIT_TEST(CteReferencesShareProducerAndHaveDisjointIds) {
        TFixture f;
        auto root = f.Run("WITH c(k) AS (SELECT id + 1 FROM t) "
            "SELECT l.k AS a, r.k AS b FROM c AS l JOIN c AS r ON l.k = r.k;");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Source).size(), 1);
        auto ports = f.Find(*root, EOperator::Replicate);
        UNIT_ASSERT_VALUES_EQUAL(ports.size(), 2);
        UNIT_ASSERT(!ports[0]->GetOutputIUs().HasAny(ports[1]->GetOutputIUs()));
        auto* a = static_cast<TOpReplicate*>(ports[0]);
        auto* b = static_cast<TOpReplicate*>(ports[1]);
        UNIT_ASSERT_EQUAL(&a->GetReplicate(), &b->GetReplicate());
    }

    // Visible renames must not replace the stored column names used by the read.
    Y_UNIT_TEST(DerivedAndPhysicalColumnAliases) {
        TFixture f;
        auto root = f.Run("SELECT d.key FROM (SELECT key FROM t AS x(key, label, count)) AS d;");
        auto* read = static_cast<TOpRead*>(f.Find(*root, EOperator::Source).at(0));
        UNIT_ASSERT_VALUES_EQUAL(read->GetColumns().Size(), 3);
        TVector<TString> storedNames;
        for (const auto id : read->GetColumns()) {
            storedNames.push_back(root->PlanProps.InfoUnitRegistry.Get(id).GetColumnName());
        }
        UNIT_ASSERT_VALUES_EQUAL(storedNames, (TVector<TString>{"id", "value", "a"}));
    }

    // Presentation ordering is separate from catalog order; DISTINCT must emit
    // only its projected key IUs and exclude hidden input dependencies.
    Y_UNIT_TEST(StarPresentationAndDistinctKeys) {
        TFixture f;
        auto root = f.Run("SELECT * FROM t;");
        UNIT_ASSERT_VALUES_EQUAL(root->GetColumns().Items()[0].second, "id");
        auto ordered = f.Lower(true);
        UNIT_ASSERT_VALUES_EQUAL(ordered->GetColumns().Items()[0].second, "id");
        auto unordered = f.Run("PRAGMA DisableOrderedColumns; SELECT * FROM t;");
        UNIT_ASSERT_VALUES_EQUAL(unordered->GetColumns().Items()[0].second, "a");
        auto distinct = f.Run("SELECT DISTINCT value AS v FROM t ORDER BY v;");
        auto* agg = static_cast<TOpAggregate*>(f.Find(*distinct, EOperator::Aggregate).at(0));
        UNIT_ASSERT(agg->GetOutputIUs() == distinct->GetOutputIUs());
    }

    // Exercise differing output-side rules through the same ID/argument invariants.
    Y_UNIT_TEST(SemiCrossAndOuterJoinKinds) {
        for (const TString kind : {"LEFT SEMI", "RIGHT ONLY", "FULL", "EXCLUSION", "CROSS"}) {
            TFixture f;
            const auto projection = kind == "RIGHT ONLY" ? "r.id" : "l.id";
            auto root = f.Run(TStringBuilder() << "SELECT " << projection << " FROM t AS l " << kind
                << " JOIN t AS r" << (kind == "CROSS" ? ";" : " ON l.id = r.id;"));
            UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Join).size(), 1);
        }
    }

    // Reject unsupported modifiers and unavailable DISTINCT sort dependencies
    // without producing either a partial plan or a spurious type-error issue.
    Y_UNIT_TEST(UnsupportedSemanticsDoNotSilentlyDisappear) {
        for (const TString sql : {"SELECT l.id FROM t AS l JOIN ANY t AS r ON l.id = r.id;",
            "SELECT DISTINCT id FROM t ORDER BY value;", "SELECT id FROM t ASSUME ORDER BY id;"}) {
            TFixture f;
            UNIT_ASSERT_EXCEPTION(f.Run(sql), NFrontend::TFrontendError);
            UNIT_ASSERT(f.ExprCtx.IssueManager.GetIssues().Empty());
        }
    }

    // Named query aliases must share a producer while their self-join ports
    // define disjoint IUs. Reimport also checks annotation/AST immutability.
    Y_UNIT_TEST(PreambleLambdasAndNamedQueriesLowerDirectly) {
        TFixture f;
        auto root = f.Run("$offset = 1; $inc = ($x) -> ($x + $offset); "
            "$q = (SELECT $inc(id) AS k FROM t); $alias = $q; "
            "$offset = 100; SELECT l.k, r.k FROM $q AS l JOIN $alias AS r ON l.k = r.k ORDER BY l.k LIMIT 2;");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Source).size(), 1);
        const auto ports = f.Find(*root, EOperator::Replicate);
        UNIT_ASSERT_VALUES_EQUAL(ports.size(), 2);
        UNIT_ASSERT(!ports[0]->GetOutputIUs().HasAny(ports[1]->GetOutputIUs()));
        UNIT_ASSERT_C(f.Recompute(*root) == IGraphTransformer::TStatus::Ok, f.ExprCtx.IssueManager.GetIssues().ToString());
        auto again = f.Lower();
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*again, EOperator::Source).size(), 1);
    }

    Y_UNIT_TEST(PreambleQueriesComposeWithWithCtesAndDerivedTables) {
        TFixture f;
        auto root = f.Run("$q = SELECT id FROM t; $r = (SELECT id + 1 AS k FROM $q); "
            "WITH c AS (SELECT k FROM $r) SELECT d.k FROM (SELECT k FROM c) AS d WHERE d.k > 0;");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Source).size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(root->GetColumns().Items()[0].second, "k");
        UNIT_ASSERT_VALUES_EQUAL(f.Find(*root, EOperator::Filter).size(), 1);
    }

}

} // namespace NKikimr::NKqp
