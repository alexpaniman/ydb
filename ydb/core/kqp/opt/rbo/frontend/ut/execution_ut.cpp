#include <ydb/core/kqp/opt/rbo/frontend/execution.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TExecutionPlan Prepare(std::string_view sql) {
    auto compiled = CompileNative(sql, [](const TTableAddress& address, TSourcePosition) {
        const std::string_view name = address.Name;
        return TCatalogTable{std::string(name), {{"id", {ETypeKind::Uint64}}}, TWriteSchema{{"id"}, {}, false}};
    });
    return PrepareExecution(std::move(*compiled.Program));
}
std::vector<const TExecutionRoot*> Materializations(const TExecutionPlan& plan) {
    std::vector<const TExecutionRoot*> result;
    for (const auto& root : plan.Roots) if (root.MaterializedInput) result.push_back(&root);
    return result;
}
}

Y_UNIT_TEST_SUITE(KqpNativeExecution) {
    Y_UNIT_TEST(OrdinaryRootsObservePrecedingWrites) {
        const auto plan = Prepare("INSERT INTO t (id) VALUES (1ul); SELECT id FROM t; DELETE FROM t WHERE id = 1ul");
        UNIT_ASSERT(Materializations(plan).empty());
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[0].Phase, 1);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[1].Phase, 2);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[2].Phase, 3);
    }

    Y_UNIT_TEST(ConsecutiveEffectsKeepTableDependencies) {
        const auto plan = Prepare("UPSERT INTO t (id) VALUES (1ul); INSERT INTO u SELECT id FROM t; "
            "DELETE FROM u WHERE id = 1ul; SELECT id FROM u");
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 4);
        for (std::size_t i = 0; i < plan.Roots.size(); ++i) UNIT_ASSERT_VALUES_EQUAL(plan.Roots[i].Phase, i + 1);
    }

    Y_UNIT_TEST(SharedProducerIsEvaluatedAtFirstUseAfterWrites) {
        const auto plan = Prepare("UPSERT INTO t (id) VALUES (1ul); $q = SELECT id FROM t; "
            "SELECT id FROM $q; SELECT id FROM $q");
        const auto materialized = Materializations(plan);
        UNIT_ASSERT_VALUES_EQUAL(materialized.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[0].Phase, 1);
        UNIT_ASSERT_VALUES_EQUAL(materialized[0]->Phase, 2);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[2].Phase, 3);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[3].Phase, 3);
    }

    Y_UNIT_TEST(PureQuerySharingWithinOneRootRemainsForRbo) {
        const auto plan = Prepare("$q = SELECT id FROM t; SELECT a.id FROM $q AS a JOIN $q AS b ON a.id = b.id");
        UNIT_ASSERT(Materializations(plan).empty());
        unsigned uses = 0;
        for (const auto& relation : plan.Program.Relations) uses += relation.Kind == ERelation::Reuse;
        UNIT_ASSERT_VALUES_EQUAL(uses, 2);
    }

    Y_UNIT_TEST(SharedIndexSourcesHaveAnExplicitExecutionInput) {
        for (const auto* sql : {
                "$q = SELECT id FROM t VIEW ById; SELECT a.id FROM $q a JOIN $q b ON a.id = b.id",
                "$inner = SELECT id FROM t VIEW ById; $outer = SELECT id + 1ul AS id FROM $inner; "
                "SELECT a.id FROM $outer a JOIN $outer b ON a.id = b.id"}) {
            const auto plan = Prepare(sql);
            UNIT_ASSERT_VALUES_EQUAL(Materializations(plan).size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots[0].Phase, 0);
            UNIT_ASSERT_VALUES_EQUAL(plan.Roots[1].Phase, 1);
        }
        const auto once = Prepare("$q = SELECT id FROM t VIEW ById; SELECT id FROM $q");
        UNIT_ASSERT(Materializations(once).empty());
    }

    Y_UNIT_TEST(SharedProducerBecomesOneInternalInputAcrossRoots) {
        const auto plan = Prepare("$q = SELECT id FROM t; SELECT id FROM $q; INSERT INTO u SELECT id FROM $q");
        const auto materialized = Materializations(plan);
        UNIT_ASSERT_VALUES_EQUAL(materialized.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[0].Phase, 0);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[1].Phase, 1);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots[2].Phase, 2);
        UNIT_ASSERT(plan.Program.Parameters.empty());
        std::vector<TColumnId> inputs;
        for (const auto& relation : plan.Program.Relations) if (relation.Kind == ERelation::Parameter) {
            UNIT_ASSERT_VALUES_EQUAL(relation.ParameterName, *materialized[0]->MaterializedInput);
            UNIT_ASSERT_VALUES_EQUAL(relation.Columns.size(), 1);
            inputs.push_back(relation.Columns[0]);
            UNIT_ASSERT_VALUES_EQUAL(plan.Program.Columns[relation.Columns[0]].Name, materialized[0]->Query.Columns[0].Name);
        }
        UNIT_ASSERT_VALUES_EQUAL(inputs.size(), 2);
        UNIT_ASSERT_VALUES_UNEQUAL(inputs[0], inputs[1]);
    }

    Y_UNIT_TEST(NestedProducerUsedOnlyBySharedOuterQueryIsNotMaterializedAgain) {
        const auto plan = Prepare("$inner = SELECT id FROM t; $outer = SELECT id + 1ul AS id FROM $inner; "
            "SELECT id FROM $outer; SELECT id FROM $outer");
        UNIT_ASSERT_VALUES_EQUAL(Materializations(plan).size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 3);
    }

    Y_UNIT_TEST(IndependentNestedConsumersRequireOrderedMaterializations) {
        const auto plan = Prepare("$inner = SELECT id FROM t; $outer = SELECT id + 1ul AS id FROM $inner; "
            "SELECT id FROM $outer; SELECT id FROM $outer; SELECT id FROM $inner");
        const auto materialized = Materializations(plan);
        UNIT_ASSERT_VALUES_EQUAL(materialized.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(materialized[0]->Phase, 0);
        UNIT_ASSERT_VALUES_EQUAL(materialized[1]->Phase, 1);
        for (const auto& root : plan.Roots) if (!root.MaterializedInput) UNIT_ASSERT_VALUES_EQUAL(root.Phase, 2);
    }

    Y_UNIT_TEST(ScalarSubqueryEdgesParticipateInExecutionSharing) {
        const auto plan = Prepare("$q = SELECT id FROM t; SELECT (SELECT MAX(id) FROM $q) AS m; SELECT id FROM $q");
        UNIT_ASSERT_VALUES_EQUAL(Materializations(plan).size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(plan.Program.Subplans.size(), 1);
    }

    Y_UNIT_TEST(VolatileNamedProducerHasOneEvaluationForAllConsumers) {
        for (const auto* sql : {
            "$q = SELECT RandomUuid(id) AS id FROM t; SELECT a.id = b.id FROM $q AS a CROSS JOIN $q AS b",
            "$q = SELECT RandomUuid(id) AS id FROM t; SELECT id FROM $q; SELECT id FROM $q",
        }) {
            const auto plan = Prepare(sql);
            UNIT_ASSERT_VALUES_EQUAL_C(Materializations(plan).size(), 1, sql);
        }
        const auto single = Prepare("$q = SELECT RandomUuid(id) AS id FROM t; SELECT id FROM $q");
        UNIT_ASSERT(Materializations(single).empty());
    }

    Y_UNIT_TEST(UnusedProducersDoNotCreateExecutionInputs) {
        const auto plan = Prepare("$q = SELECT RandomUuid(id) AS id FROM t; SELECT 1");
        UNIT_ASSERT(Materializations(plan).empty());
        UNIT_ASSERT_VALUES_EQUAL(plan.Roots.size(), 1);
    }
}
