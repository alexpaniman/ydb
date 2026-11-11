#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;
using namespace NYdb::NTable;

TKikimrSettings Settings(bool native) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(native);
    service->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                                        : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetEnableFallbackOnMultipleStatements(false);
    service->SetEnableNewRBOPhysicalStagePeephole(false);
    return TKikimrSettings(config).SetWithSampleTables(false);
}

template <class TResult>
void Success(const TResult& result) {
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos,
        result.GetIssues().ToString());
}

void CreateTables(TKikimrRunner& kikimr, TSession& session) {
    const auto schema = session.ExecuteSchemeQuery(R"(
        CREATE TABLE t (id Uint64 NOT NULL, PRIMARY KEY(id));
        CREATE TABLE u (id Uint64 NOT NULL, value String, PRIMARY KEY(id));
    )").GetValueSync();
    UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
    TValueBuilder rows;
    rows.BeginList();
    for (ui64 id : {1ul, 2ul, 3ul}) rows.AddListItem().BeginStruct().AddMember("id").Uint64(id).EndStruct();
    rows.EndList();
    const auto inserted = kikimr.GetTableClient().BulkUpsert("/Root/t", rows.Build()).GetValueSync();
    UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
}
}

Y_UNIT_TEST_SUITE(KqpNativeExecutionRuntime) {
    Y_UNIT_TEST(NestedAggregateStatesUseLexicographicOrder) {
        const TVector<TString> values{
            "(0, AsList(id, id + 1))",
            "(0, AsList(CAST(NULL AS Uint64?), id))",
            "(0, IF(id = 1ul, AsList(1ul), AsList(1ul, id)))",
            "(0, IF(id = 1ul, CAST(AsList() AS List<Uint64>), AsList(id)))",
            "(0, IF(id = 1ul, NULL, AsList(id)))",
            "(0, ((id, id + 1), AsList(id)))",
            "(0, AsList(AsList(id), AsList(id + 1)))",
        };
        TVector<TString> baseline(values.size());
        TString failures;
        for (const bool native : {false, true}) {
            TKikimrRunner kikimr(Settings(native));
            auto table = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            CreateTables(kikimr, table);
            for (size_t i = 0; i < values.size(); ++i) {
                const TString sql = "SELECT MIN(" + values[i] + "), MAX(" + values[i] + ") FROM t";
                const auto result = table.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
                if (!result.IsSuccess()) {
                    failures += TStringBuilder() << "native=" << native << ": " << sql << "\n" << result.GetIssues().ToString() << "\n";
                    table = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
                    continue;
                }
                UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, sql);
                TString actual = FormatResultSetYson(result.GetResultSet(0));
                for (const auto& column : result.GetResultSet(0).GetColumnsMeta()) actual += "|" + column.Type.ToString();
                if (!native) baseline[i] = actual;
                else if (actual != baseline[i]) failures += sql + "\nnative: " + actual + "\nlegacy: " + baseline[i] + "\n";
            }
        }
        UNIT_ASSERT_C(failures.empty(), failures);
    }

    Y_UNIT_TEST(BooleanFilterPreparationMatchesLegacy) {
        const TVector<TString> predicates{"Just(r)", "Just(r = true)", "r IS NOT NULL", "r IS NULL",
            "b AND true", "b OR false", "IF(b, true, false)", "Just(b IS NOT NULL)"};
        TParamsBuilder parameters;
        auto& rows = parameters.AddParam("$rows").BeginList();
        ui32 id = 0;
        for (const auto b : {std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true}})
            rows.AddListItem().BeginStruct().AddMember("id").Uint32(id++)
                .AddMember("b").OptionalBool(b).AddMember("r").Bool(b.value_or(false)).EndStruct();
        rows.EndList().Build();
        const auto params = parameters.Build();
        TVector<TString> expected;
        for (const bool native : {false, true}) {
            TKikimrRunner kikimr(Settings(native));
            for (std::size_t i = 0; i < predicates.size(); ++i) {
                const TString sql = TStringBuilder()
                    << "DECLARE $rows AS List<Struct<id:Uint32,b:Bool?,r:Bool>>; "
                    << "SELECT id, b AND true AS a, b OR false AS o, true AND b AS l, r AND Just(true) AS n "
                    << "FROM AS_TABLE($rows) WHERE " << predicates[i] << " ORDER BY id;";
                const auto result = kikimr.GetQueryClient().ExecuteQuery(sql, NQuery::TTxControl::NoTx(), params).GetValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), sql << ": " << result.GetIssues().ToString());
                UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos, sql);
                const auto& resultRows = result.GetResultSet(0);
                TString signature = FormatResultSetYson(resultRows);
                for (const auto& column : resultRows.GetColumnsMeta()) signature += "|" + column.Name + ":" + column.Type.ToString();
                if (!native) expected.push_back(signature);
                else UNIT_ASSERT_VALUES_EQUAL_C(signature, expected[i], sql);
            }
        }
    }


    Y_UNIT_TEST(ScalarFunctionNamesAndMetadataMatchLegacy) {
        const TVector<TString> queries{
            "SELECT aBs(-1), Length('abc'), ListLength([1, 2]), ListHasItems([1]), Starts_With('abc', 'a'), "
                "MaxOf(1, 2l), Coalesce(Just(1), 2), IF(Just(TRUE), 1, 2), IF(NULL, 1), Substring('abc', 1u);",
            "SELECT ListLength(NULL), ListLength([]), ListHasItems([]), "
                "ListLength(ListFromRange(CAST(NULL AS Int32?), 5)), ListHasItems(ListFromRange(CAST(NULL AS Int32?), 5));",
            "SELECT MinOf(1, 2l), Greatest(1, 2l, Just(3)), Least(1, NULL, 3), MaxOf('abc', 'ab'), MinOf(TRUE, FALSE);",
            "SELECT MaxOf(Decimal('nan', 12, 2), Decimal('2.50', 12, 2)), "
                "MinOf(Decimal('2.50', 12, 2), Decimal('nan', 12, 2));",
            "SELECT MaxOf(Double('nan'), 2.0), MinOf(2.0, Double('nan')), MaxOf(Double('nan'), Double('nan'));",
            "SELECT MinOf(-1l, 18446744073709551615ul), MaxOf(-1l, 18446744073709551615ul), "
                "MaxOf(Decimal('1.25', 9, 2), Decimal('2.5', 10, 1)), MinOf('a'u, 'b');",
            "DECLARE $start AS Int32?; DECLARE $stop AS Int32; "
                "SELECT ListLength(ListFromRange($start, $stop)), ListHasItems(ListFromRange($start, $stop)), "
                "MinOf($start, $stop), MaxOf($start, $stop), MinOf($start, NULL, $stop);",
        };
        TVector<TString> baseline;
        for (const bool native : {false, true}) {
            TKikimrRunner kikimr(Settings(native));
            auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            size_t index = 0;
            for (const auto& query : queries) for (const bool present : {false, true}) {
                if (present && !query.StartsWith("DECLARE")) continue;
                const auto params = TParamsBuilder().AddParam("$start")
                    .OptionalInt32(present ? std::optional<i32>{2} : std::nullopt).Build()
                    .AddParam("$stop").Int32(5).Build().Build();
                const auto result = query.StartsWith("DECLARE")
                    ? session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx(), params).GetValueSync()
                    : session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync();
                Success(result);
                const auto& rows = result.GetResultSet(0);
                TString signature = FormatResultSetYson(rows);
                for (const auto& column : rows.GetColumnsMeta()) signature += "|" + column.Type.ToString();
                if (!native) baseline.push_back(signature);
                else UNIT_ASSERT_VALUES_EQUAL_C(signature, baseline.at(index), query << ", present=" << present);
                ++index;
            }
        }
    }

    Y_UNIT_TEST_TWIN(ListOperationsMatchLegacyValuesAndNullability, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const std::pair<TString, std::optional<TString>> cases[]{
            {"ListConcat(CAST(ListFromRange(1, 10) AS List<String>), '..')", "1..2..3..4..5..6..7..8..9"},
            {"ListConcat(CAST(ListFromRange(5, -1, -2) AS List<String>), ',')", "5,3,1"},
            {"ListConcat(CAST(ListFromRange(1u, 6u, 2) AS List<String>))", "135"},
            {"ListConcat(CAST(ListFromRange(1, 1) AS List<String>))", std::nullopt},
            {"ListConcat(CAST(ListFromRange(1, 8, 0) AS List<String>))", std::nullopt},
            {"ListConcat(CAST(ListFromRange(Just(1), 4) AS List<String>), ',')", "1,2,3"},
            {"ListConcat(CAST(ListFromRange(CAST(NULL AS Int32?), 4) AS List<String>))", std::nullopt},
            {"ListConcat(AsList('a', NULL, 'b', NULL), ',')", "a,,b,"},
            {"ListConcat(AsList(NULL, 'a', NULL), ',')", "a,"},
            {"ListConcat(AsList('a', 'b'), CAST(NULL AS String?))", "ab"},
            {"ListConcat(CAST(CAST(AsList('1', 'bad', '2') AS List<Int32?>) AS List<String?>), ',')", "1,,2"},
            {"ListConcat(CAST(AsList() AS List<String>))", std::nullopt},
        };
        for (const auto& [expression, expected] : cases) {
            const auto sql = "SELECT " + expression + " AS value";
            const auto result = session.ExecuteDataQuery(sql, TTxControl::BeginTx().CommitTx()).GetValueSync();
            Success(result);
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetResultSet(0).GetColumnsMeta()[0].Type.ToString(), "String?", sql);
            TResultSetParser rows(result.GetResultSet(0));
            UNIT_ASSERT(rows.TryNextRow());
            const auto actual = rows.ColumnParser(0).GetOptionalString();
            UNIT_ASSERT_VALUES_EQUAL_C(bool(actual), bool(expected), sql);
            if (expected) UNIT_ASSERT_VALUES_EQUAL_C(*actual, *expected, sql);
        }
        const auto params = TParamsBuilder()
            .AddParam("$start").OptionalInt32(2).Build()
            .AddParam("$stop").Int32(7).Build()
            .AddParam("$step").Int32(2).Build()
            .AddParam("$separator").OptionalString(",").Build()
            .Build();
        const auto dynamic = session.ExecuteDataQuery(R"(
            DECLARE $start AS Int32?; DECLARE $stop AS Int32; DECLARE $step AS Int32; DECLARE $separator AS String?;
            SELECT ListConcat(CAST(ListFromRange($start, $stop, $step) AS List<String>), $separator) AS value;
        )", TTxControl::BeginTx().CommitTx(), params).GetValueSync();
        Success(dynamic);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(dynamic.GetResultSet(0)), "[[[\"2,4,6\"]]]");
        const auto items = TParamsBuilder().AddParam("$items").BeginList()
            .AddListItem().String("1").AddListItem().String("bad").AddListItem().String("2")
            .EndList().Build().Build();
        const auto cast = session.ExecuteDataQuery(R"(
            DECLARE $items AS List<String>;
            SELECT CAST($items AS List<Int32>) IS NULL AS failed,
                CAST($items AS List<Int32>?) IS NULL AS optional_failed,
                ListConcat(CAST(CAST($items AS List<Int32>) AS List<String>), ',') AS filtered,
                ListConcat(CAST(CAST($items AS List<Int32?>) AS List<String?>), ',') AS retained;
        )", TTxControl::BeginTx().CommitTx(), items).GetValueSync();
        Success(cast);
        TResultSetParser castRows(cast.GetResultSet(0));
        UNIT_ASSERT(castRows.TryNextRow());
        UNIT_ASSERT(!castRows.ColumnParser("failed").GetBool());
        UNIT_ASSERT(!castRows.ColumnParser("optional_failed").GetBool());
        UNIT_ASSERT_VALUES_EQUAL(*castRows.ColumnParser("filtered").GetOptionalString(), "1,2");
        UNIT_ASSERT_VALUES_EQUAL(*castRows.ColumnParser("retained").GetOptionalString(), "1,,2");
        const auto unicode = session.ExecuteDataQuery("SELECT ListConcat(AsList('a'u, NULL, 'é'u), ','u) AS value",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(unicode);
        UNIT_ASSERT_VALUES_EQUAL(unicode.GetResultSet(0).GetColumnsMeta()[0].Type.ToString(), "Utf8?");
        TResultSetParser unicodeRows(unicode.GetResultSet(0));
        UNIT_ASSERT(unicodeRows.TryNextRow());
        UNIT_ASSERT_VALUES_EQUAL(*unicodeRows.ColumnParser(0).GetOptionalUtf8(), "a,,é");
    }

    Y_UNIT_TEST_TWIN(LambdaClosuresCaptureRowsAndStaticCallables, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = session.ExecuteDataQuery(R"(
            $n = 1ul;
            $f = ($x) -> { $n = $n + $x; RETURN $n; };
            $n = 100ul;
            $make = ($x) -> { $n = $x + 1ul; RETURN ($y) -> ($n + $y); };
            $apply = ($fn, $x) -> ($fn($x));
            $capture = ($x) -> { $g = () -> ($x); $h = ($x) -> ($g()); RETURN $h(900ul); };
            $invoke = ($fn) -> ($fn('abc', 'a'));
            SELECT id, $f(id) AS captured, $n AS global_value, $make(id)(2ul) AS closed,
                $apply(($x) -> ($x * 2ul), id) AS doubled, $capture(id) AS shadowed,
                $invoke(($text, $needle) -> (Udf(String::Contains)($text, $needle))) AS found FROM t ORDER BY id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), 3);
        TResultSetParser rows(result.GetResultSet(0));
        for (ui64 id : {1ul, 2ul, 3ul}) {
            UNIT_ASSERT(rows.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser("id").GetUint64(), id);
            UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser("captured").GetUint64(), id + 1);
            UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser("global_value").GetUint64(), 100);
            UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser("closed").GetUint64(), id + 3);
            UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser("doubled").GetUint64(), id * 2);
            UNIT_ASSERT_VALUES_EQUAL(rows.ColumnParser("shadowed").GetUint64(), id);
            UNIT_ASSERT(rows.ColumnParser("found").GetBool());
        }
    }

    Y_UNIT_TEST_TWIN(LambdaClosureResultsFlowThroughWrites, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        Success(session.ExecuteDataQuery(R"(
            $make = ($x) -> { $n = $x + 1ul; RETURN ($y) -> ($n + $y); };
            INSERT INTO u SELECT id, CAST($make(id)(2ul) AS String) AS value FROM t;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync());
        Success(session.ExecuteDataQuery(R"(
            $prefix = ($x) -> { $value = 'result:' || $x; RETURN $value; };
            UPDATE u SET value = $prefix(value) WHERE id > 1ul;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync());
        const auto result = session.ExecuteDataQuery("SELECT id, value FROM u ORDER BY id",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)),
            "[[1u;[\"4\"]];[2u;[\"result:5\"]];[3u;[\"result:6\"]]]");
    }

    Y_UNIT_TEST_TWIN(LambdaClosuresKeepOneCapturedVolatileValue, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = session.ExecuteDataQuery(R"(
            $same = ($x) -> {
                $value = RandomUuid($x);
                $read = () -> ($value);
                RETURN CAST($read() AS String) = CAST($read() AS String);
            };
            SELECT $same(id) AS equal FROM t ORDER BY id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[%true];[%true];[%true]]");
    }

    // Exact shared-binding cases from the newer reference frontend. Native
    // execution must preserve sharing without its legacy fallback.
    Y_UNIT_TEST_TWIN(SharedVolatileAliasesPreserveOneEvaluation, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto queries = session.ExecuteDataQuery(R"(
            $q = SELECT id, CAST(RandomUuid(id) AS Utf8) AS value FROM t;
            $alias = $q;
            SELECT * FROM $q ORDER BY id;
            SELECT * FROM $alias ORDER BY id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(queries);
        UNIT_ASSERT_VALUES_EQUAL(queries.GetResultSets().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(queries.GetResultSet(0).RowsCount(), 3);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(queries.GetResultSet(0)), FormatResultSetYson(queries.GetResultSet(1)));

        const auto scalar = session.ExecuteDataQuery(R"(
            $x = RandomUuid(1);
            SELECT CAST($x AS Utf8) AS a, CAST($x AS Utf8) AS b;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(scalar);
        UNIT_ASSERT_VALUES_EQUAL(scalar.GetResultSets().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(scalar.GetResultSet(0).RowsCount(), 1);
        TResultSetParser row(scalar.GetResultSet(0));
        UNIT_ASSERT(row.TryNextRow());
        UNIT_ASSERT_VALUES_EQUAL(row.ColumnParser(0).GetUtf8(), row.ColumnParser(1).GetUtf8());
    }

    Y_UNIT_TEST_TWIN(NamedVolatileRowsAreSharedAcrossResultsAndWrites, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = session.ExecuteDataQuery(R"(
            $q = SELECT id, CAST(RandomUuid(id) AS String) AS value FROM t;
            SELECT id, value FROM $q ORDER BY id;
            INSERT INTO u SELECT id, value FROM $q;
            SELECT id, value FROM $q ORDER BY id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), 3);
        const auto expected = FormatResultSetYson(result.GetResultSet(0));
        UNIT_ASSERT_VALUES_EQUAL(expected, FormatResultSetYson(result.GetResultSet(1)));
        const auto written = session.ExecuteDataQuery("SELECT id, Unwrap(value) AS value FROM u ORDER BY id",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(written);
        // The classic frontend expands the write independently. Native named
        // producers retain their identity across public execution roots.
        if constexpr (Native) UNIT_ASSERT_VALUES_EQUAL(expected, FormatResultSetYson(written.GetResultSet(0)));
        else UNIT_ASSERT_VALUES_UNEQUAL(expected, FormatResultSetYson(written.GetResultSet(0)));
    }

    Y_UNIT_TEST_TWIN(NamedVolatileRowsAreSharedWithinOneResult, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = session.ExecuteDataQuery(R"(
            $q = SELECT id, RandomUuid(id) AS value FROM t;
            SELECT l.value = r.value AS equal FROM $q AS l JOIN $q AS r ON l.id = r.id ORDER BY l.id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        // Pre-migration new RBO also shares this table-backed projection; the
        // classic optimizer evaluates the two join inputs independently.
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)),
            Native ? "[[%true];[%true];[%true]]" : "[[%false];[%false];[%false]]");
    }

    Y_UNIT_TEST_TWIN(GlobalVolatileBindingsHaveOneValueAcrossStatements, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto result = session.ExecuteDataQuery(R"(
            $value = YQL::RandomUuid(); $copy = $value;
            SELECT CAST($value AS String) AS value;
            SELECT CAST($copy AS String) AS value;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), FormatResultSetYson(result.GetResultSet(1)));
    }

    Y_UNIT_TEST_TWIN(ZeroArgumentUuidIsOneInternalInputPerExecution, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto prepared = session.PrepareDataQuery("SELECT CAST(YQL::RandomUuid() AS String) AS a, CAST(YQL::RandomUuid() AS String) AS b").GetValueSync();
        UNIT_ASSERT_C(prepared.IsSuccess(), prepared.GetIssues().ToString());
        UNIT_ASSERT(prepared.GetQuery().GetParameterTypes().empty());
        std::optional<TString> previous;
        for (unsigned i = 0; i < 2; ++i) {
            const auto result = prepared.GetQuery().Execute(TTxControl::BeginTx().CommitTx()).GetValueSync();
            Success(result);
            TResultSetParser rows(result.GetResultSet(0));
            UNIT_ASSERT(rows.TryNextRow());
            const TString value(rows.ColumnParser(0).GetString());
            UNIT_ASSERT_VALUES_EQUAL(value, rows.ColumnParser(1).GetString());
            if (previous) UNIT_ASSERT_VALUES_UNEQUAL(value, *previous);
            previous = value;
        }
    }

    Y_UNIT_TEST_TWIN(ZeroArgumentNumberIsOneInternalInputPerExecution, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto prepared = session.PrepareDataQuery(R"(
            SELECT YQL::RandomNumber() AS a, YQL::RandomNumber() AS b;
            SELECT YQL::RandomNumber() AS c;
        )").GetValueSync();
        UNIT_ASSERT_C(prepared.IsSuccess(), prepared.GetIssues().ToString());
        UNIT_ASSERT(prepared.GetQuery().GetParameterTypes().empty());
        std::optional<ui64> previous;
        for (unsigned i = 0; i < 2; ++i) {
            const auto result = prepared.GetQuery().Execute(TTxControl::BeginTx().CommitTx()).GetValueSync();
            Success(result);
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
            TResultSetParser first(result.GetResultSet(0)), second(result.GetResultSet(1));
            UNIT_ASSERT(first.TryNextRow()); UNIT_ASSERT(second.TryNextRow());
            const auto value = first.ColumnParser(0).GetUint64();
            UNIT_ASSERT_VALUES_EQUAL(value, first.ColumnParser(1).GetUint64());
            UNIT_ASSERT_VALUES_EQUAL(value, second.ColumnParser(0).GetUint64());
            if (previous) UNIT_ASSERT_VALUES_UNEQUAL(value, *previous);
            previous = value;
        }
    }

    // Retain the established generic-query behavior tested by
    // NamedExpressionPureNondeterministicWriteAndResult in the legacy suite.
    Y_UNIT_TEST_TWIN(DependencyBearingScalarIsNotQueryCached, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = kikimr.GetQueryClient().ExecuteQuery(R"(
            $value = CAST(RandomUuid(1u) AS String);
            UPSERT INTO u (id, value) VALUES (1ul, $value);
            SELECT $value AS value;
        )", NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        const auto stored = session.ExecuteDataQuery("SELECT value FROM u WHERE id = 1ul",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(stored);
        TResultSetParser selected(result.GetResultSet(0)), written(stored.GetResultSet(0));
        UNIT_ASSERT(selected.TryNextRow()); UNIT_ASSERT(written.TryNextRow());
        UNIT_ASSERT_VALUES_UNEQUAL(selected.ColumnParser(0).GetString(), *written.ColumnParser(0).GetOptionalString());
    }

    Y_UNIT_TEST_TWIN(UnchosenGlobalAssertionStaysLazy, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto params = TParamsBuilder().AddParam("$chosen").Bool(false).Build().Build();
        const auto result = session.ExecuteDataQuery(R"(
            DECLARE $chosen AS Bool;
            $value = Ensure(YQL::RandomUuid(), false, 'unchosen global assertion');
            SELECT IF($chosen, CAST($value AS String), 'safe') AS value;
            SELECT IF($chosen, CAST($value AS String), 'safe') AS value;
        )", TTxControl::BeginTx().CommitTx(), params).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        for (const auto& rows : result.GetResultSets()) UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(rows), "[[\"safe\"]]");
    }
}
}
