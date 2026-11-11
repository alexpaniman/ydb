// Syntax boundary cases from PR #58587 at fbeb81657ef, adapted to the native
// owning syntax arena. These assertions do not imply binder/runtime admission.
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TParsedQuery Ast(std::string text, bool grammar) {
    TCompilerSettings settings;
    settings.Parser = grammar ? EParser::Grammar : EParser::Fast;
    return ParseNative(text, settings);
}

const TSyntaxExpr& Projection(const TParsedQuery& syntax, std::size_t index) {
    return syntax.Expressions.at(syntax.Statements.at(0).Query->Projection.at(index).Expression);
}

const TSyntaxExpr& Child(const TParsedQuery& syntax, const TSyntaxExpr& expression, std::size_t index) {
    return syntax.Expressions.at(expression.Arguments.at(index));
}
}

Y_UNIT_TEST_SUITE(KqpNativeReferenceAst) {
    Y_UNIT_TEST(SchemaAndWatermarkHints) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT * FROM t WITH (
                SCHEMA = Struct<id:Int64, name:String?>, COLUMNS = $columns,
                WATERMARK = ts - 1);
                SELECT * FROM t WITH SCHEMA (id Int64 NOT NULL, name String NULL, Int32 AS old, raw $type);
                SELECT * FROM t WITH SCHEMA ();)", grammar);
            const auto& hints = syntax.Statements[0].Query->Source->Options->Hints;
            const auto& schemaHint = std::get<TSchemaHint>(hints[0].Value);
            UNIT_ASSERT(!schemaHint.Columns);
            const auto& schema = std::get<TSyntaxType>(schemaHint.Value);
            UNIT_ASSERT_VALUES_EQUAL(schema.Name, "Struct");
            UNIT_ASSERT_VALUES_EQUAL(schema.Members.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(schema.Members[0].Name->Text, "id");
            UNIT_ASSERT(schema.Members[0].Name->Kind == EHintValueKind::Identifier);
            UNIT_ASSERT_VALUES_EQUAL(schema.Members[1].Type.OptionalDepth, 1);
            const auto& columns = std::get<TSchemaHint>(hints[1].Value);
            UNIT_ASSERT(columns.Columns);
            UNIT_ASSERT(std::get<TSyntaxType>(columns.Value).Binding);
            UNIT_ASSERT_VALUES_EQUAL(std::get<TSyntaxType>(columns.Value).Name, "columns");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[std::get<TWatermarkHint>(hints[2].Value).Expression].Text, "-");
            const auto& positional = std::get<std::vector<TSchemaColumn>>(
                std::get<TSchemaHint>(syntax.Statements[1].Query->Source->Options->Hints[0].Value).Value);
            UNIT_ASSERT_VALUES_EQUAL(positional.size(), 4);
            UNIT_ASSERT(positional[0].Nullability == EColumnNullability::NotNull);
            UNIT_ASSERT(positional[1].Nullability == EColumnNullability::Nullable);
            UNIT_ASSERT(positional[2].Nullability == EColumnNullability::Default);
            UNIT_ASSERT_VALUES_EQUAL(positional[2].Name.Text, "old");
            UNIT_ASSERT(positional[3].Type.Binding);
            UNIT_ASSERT(std::get<std::vector<TSchemaColumn>>(
                std::get<TSchemaHint>(syntax.Statements[2].Query->Source->Options->Hints[0].Value).Value).empty());
        }
    }

    Y_UNIT_TEST(NestedSchemaTypes) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT * FROM t WITH SCHEMA Struct<
                a:Variant<Int32, String>, b:Variant<x:Int32, y:$type>,
                c:Tagged<List<Int32>, 'tag'>, d:Enum<red, $blue>, e:Resource<'handle'>>;)", grammar);
            const auto& members = std::get<TSyntaxType>(
                std::get<TSchemaHint>(syntax.Statements[0].Query->Source->Options->Hints[0].Value).Value).Members;
            UNIT_ASSERT_VALUES_EQUAL(members[0].Type.Members.size(), 2);
            UNIT_ASSERT(!members[0].Type.Members[0].Name);
            UNIT_ASSERT_VALUES_EQUAL(members[1].Type.Members[0].Name->Text, "x");
            UNIT_ASSERT(members[1].Type.Members[1].Type.Binding);
            UNIT_ASSERT_VALUES_EQUAL(members[2].Type.Arguments[0].Arguments[0].Name, "Int32");
            UNIT_ASSERT(members[2].Type.Tags[0].Kind == EHintValueKind::String);
            UNIT_ASSERT_VALUES_EQUAL(members[2].Type.Tags[0].Text, "tag");
            UNIT_ASSERT(members[3].Type.Tags[1].Kind == EHintValueKind::Binding);
            UNIT_ASSERT_VALUES_EQUAL(members[3].Type.Tags[1].Text, "blue");
            UNIT_ASSERT_VALUES_EQUAL(members[4].Type.Tags[0].Text, "handle");
            UNIT_ASSERT_EXCEPTION(CollectTables(syntax), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(StructuredHintsRetainTypeIdentityAndRequireAdmission) {
        for (const bool grammar : {false, true}) {
            for (const auto* sql : {"SELECT * FROM t WITH SCHEMA Struct<a:Int32>",
                "SELECT * FROM t WITH COLUMNS = $columns", "SELECT * FROM t WITH WATERMARK = ts - 1"}) {
                const auto syntax = Ast(sql, grammar);
                UNIT_ASSERT_EXCEPTION_C(CollectTables(syntax), TUnsupportedFeature, sql);
                UNIT_ASSERT_EXCEPTION_C(BindNative(syntax, [](std::string_view, TSourcePosition) -> TCatalogTable {
                    UNIT_FAIL("unsupported schema semantics reached catalog I/O"); return {};
                }), TUnsupportedFeature, sql);
            }
            const auto fingerprint = [&](const char* sql) { return SyntaxFingerprint(Ast(sql, grammar)); };
            UNIT_ASSERT(fingerprint("SELECT * FROM t WITH SCHEMA Struct<a:Int32>") != fingerprint("SELECT * FROM t WITH SCHEMA Struct<a:Int64>"));
            UNIT_ASSERT(fingerprint("SELECT * FROM t WITH SCHEMA (a Int32)") != fingerprint("SELECT * FROM t WITH SCHEMA (a Int32 NOT NULL)"));
            UNIT_ASSERT(fingerprint("SELECT * FROM t WITH WATERMARK = ts - 1") != fingerprint("SELECT * FROM t WITH WATERMARK = ts - 2"));
            const auto ansi = Ast("--!ansi_lexer\nSELECT * FROM t WITH SCHEMA Struct<\"a\"\"b\":Decimal(20,3)?" "?>;", grammar);
            const auto& member = std::get<TSyntaxType>(std::get<TSchemaHint>(ansi.Statements[0].Query->Source->Options->Hints[0].Value).Value).Members[0];
            UNIT_ASSERT_VALUES_EQUAL(member.Name->Text, "a\"b");
            UNIT_ASSERT_VALUES_EQUAL(member.Type.Parameters[0], "20");
            UNIT_ASSERT_VALUES_EQUAL(member.Type.Parameters[1], "3");
            UNIT_ASSERT_VALUES_EQUAL(member.Type.OptionalDepth, 2);
        }
    }

    Y_UNIT_TEST(InlineWindowWithFilterAndFrame) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT SUM(x) FILTER (WHERE x > 0) OVER (
                PARTITION COMPACT BY department AS dep ORDER BY ts DESC, id ASC
                ROWS BETWEEN $n PRECEDING AND 1 FOLLOWING EXCLUDE TIES) FROM t;)", grammar);
            const auto& call = Projection(syntax, 0);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[call.Filter].Text, ">");
            UNIT_ASSERT(call.NullTreatment == ENullTreatment::Default);
            const auto& window = *call.Window;
            UNIT_ASSERT(window.Name.empty() && !window.ReferenceOnly && window.CompactPartition);
            UNIT_ASSERT_VALUES_EQUAL(window.Partition[0].Alias, "dep");
            UNIT_ASSERT_VALUES_EQUAL(window.Order.size(), 2);
            UNIT_ASSERT(window.Order[0].Descending && !window.Order[1].Descending);
            UNIT_ASSERT(window.Frame.Type == EWindowFrame::Rows);
            UNIT_ASSERT(window.Frame.Begin == EWindowBound::Preceding);
            UNIT_ASSERT(syntax.Expressions[window.BeginOffset].Kind == ESyntaxKind::Variable);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[window.BeginOffset].Text, "n");
            UNIT_ASSERT(window.ExplicitFrameEnd && window.Frame.End == EWindowBound::Following);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[window.EndOffset].Text, "1");
            UNIT_ASSERT(window.Exclusion == EWindowExclusion::Ties);
        }
    }

    Y_UNIT_TEST(NamedWindowsInheritanceAndNullTreatment) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT FIRST_VALUE(x) IGNORE NULLS OVER w,
                LAST_VALUE(x) RESPECT NULLS OVER (w), ROW_NUMBER() OVER (),
                COUNT(*) FILTER (WHERE x > 0)
                FROM t WINDOW base AS (PARTITION BY department),
                w AS (base ORDER BY ts RANGE UNBOUNDED PRECEDING);)", grammar);
            const auto& first = Projection(syntax, 0);
            UNIT_ASSERT(first.NullTreatment == ENullTreatment::Ignore);
            UNIT_ASSERT(first.Window->ReferenceOnly);
            UNIT_ASSERT_VALUES_EQUAL(first.Window->Name, "w");
            const auto& last = Projection(syntax, 1);
            UNIT_ASSERT(last.NullTreatment == ENullTreatment::Respect);
            UNIT_ASSERT(!last.Window->ReferenceOnly);
            UNIT_ASSERT_VALUES_EQUAL(last.Window->Name, "w");
            const auto& empty = *Projection(syntax, 2).Window;
            UNIT_ASSERT(empty.Frame.Type == EWindowFrame::Default && empty.Name.empty() && empty.Partition.empty() && empty.Order.empty());
            UNIT_ASSERT(!Projection(syntax, 3).Window && Projection(syntax, 3).Filter != NoSyntax);
            const auto& windows = syntax.Statements[0].Query->Windows;
            UNIT_ASSERT_VALUES_EQUAL(windows.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(windows[0].Name, "base");
            UNIT_ASSERT_VALUES_EQUAL(windows[1].Name, "w");
            const auto& inherited = *windows[1].Specification;
            UNIT_ASSERT_VALUES_EQUAL(inherited.Name, "base");
            UNIT_ASSERT(inherited.Frame.Type == EWindowFrame::Range);
            UNIT_ASSERT(inherited.Frame.Begin == EWindowBound::UnboundedPreceding);
            UNIT_ASSERT(!inherited.ExplicitFrameEnd && inherited.BeginOffset == NoSyntax);
            UNIT_ASSERT(inherited.Exclusion == EWindowExclusion::Default);
        }
    }

    Y_UNIT_TEST(WindowFrameExclusionsAndBounds) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT
                SUM(x) OVER (GROUPS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING EXCLUDE GROUP),
                SUM(x) OVER (ROWS CURRENT ROW EXCLUDE CURRENT ROW),
                SUM(x) OVER (RANGE BETWEEN 2 PRECEDING AND CURRENT ROW EXCLUDE NO OTHERS);)", grammar);
            const auto& groups = *Projection(syntax, 0).Window;
            UNIT_ASSERT(groups.Frame.Type == EWindowFrame::Groups);
            UNIT_ASSERT(groups.Frame.Begin == EWindowBound::CurrentRow);
            UNIT_ASSERT(groups.ExplicitFrameEnd && groups.Frame.End == EWindowBound::UnboundedFollowing);
            UNIT_ASSERT(groups.BeginOffset == NoSyntax && groups.EndOffset == NoSyntax);
            UNIT_ASSERT(groups.Exclusion == EWindowExclusion::Group);
            const auto& current = *Projection(syntax, 1).Window;
            UNIT_ASSERT(current.Exclusion == EWindowExclusion::CurrentRow && !current.ExplicitFrameEnd);
            const auto& range = *Projection(syntax, 2).Window;
            UNIT_ASSERT(range.Exclusion == EWindowExclusion::NoOthers);
            UNIT_ASSERT(range.ExplicitFrameEnd && range.Frame.End == EWindowBound::CurrentRow);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[range.BeginOffset].Text, "2");
        }
    }

    Y_UNIT_TEST(AnsiWindowsHintsAndPragmas) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(--!ansi_lexer
                PRAGMA "ns"."name" = 'a''b';
                SELECT SUM(x) OVER "w" FROM t WITH ("hint" = 'a''b')
                WINDOW "w" AS (ORDER BY x);)", grammar);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements[0].Name, "ns.name");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements[0].PragmaValues[0].Text, "a'b");
            const auto& select = *syntax.Statements[1].Query;
            UNIT_ASSERT_VALUES_EQUAL(select.Windows[0].Name, "w");
            const auto& reference = *syntax.Expressions[select.Projection[0].Expression].Window;
            UNIT_ASSERT(reference.ReferenceOnly);
            UNIT_ASSERT_VALUES_EQUAL(reference.Name, "w");
            const auto& hint = std::get<TGenericSourceHint>(select.Source->Options->Hints[0].Value);
            UNIT_ASSERT_VALUES_EQUAL(hint.Name, "hint");
            UNIT_ASSERT_VALUES_EQUAL(hint.Values[0].Text, "a'b");
        }
    }

    Y_UNIT_TEST(WindowNamesAndModifiersAreAdmittedAtBinding) {
        for (const bool grammar : {false, true}) {
            // Name lookup must not alter parsing, even for unresolved or duplicate names.
            for (const auto* sql : {"SELECT SUM(x) OVER missing FROM t",
                "SELECT SUM(x) OVER w FROM t WINDOW w AS (), w AS ()"}) {
                const auto syntax = Ast(sql, grammar);
                UNIT_ASSERT_EXCEPTION_C(CollectTables(syntax), TFrontendError, sql);
            }
            for (const auto* sql : {"SELECT SUM(x) FILTER (WHERE x > 0) FROM t",
                "SELECT FIRST_VALUE(x) IGNORE NULLS OVER () FROM t",
                "SELECT SUM(x) OVER (PARTITION COMPACT BY x) FROM t",
                "SELECT SUM(x) OVER (PARTITION BY x AS k) FROM t",
                "SELECT SUM(x) OVER (ROWS CURRENT ROW EXCLUDE TIES) FROM t",
                "SELECT SUM(x) OVER (w) FROM t WINDOW w AS ()"}) {
                const auto syntax = Ast(sql, grammar);
                UNIT_ASSERT_EXCEPTION_C(CollectTables(syntax), TUnsupportedFeature, sql);
                UNIT_ASSERT_EXCEPTION_C(BindNative(syntax, [](std::string_view, TSourcePosition) -> TCatalogTable {
                    UNIT_FAIL("unsupported window semantics reached catalog I/O"); return {};
                }), TUnsupportedFeature, sql);
            }
        }
        UNIT_ASSERT(SyntaxFingerprint(Ast("SELECT SUM(x) OVER w FROM t WINDOW w AS ()", true)) !=
            SyntaxFingerprint(Ast("SELECT SUM(x) OVER (w) FROM t WINDOW w AS ()", true)));
        UNIT_ASSERT(SyntaxFingerprint(Ast("SELECT SUM(x) OVER (ROWS CURRENT ROW)", true)) !=
            SyntaxFingerprint(Ast("SELECT SUM(x) OVER (ROWS BETWEEN CURRENT ROW AND CURRENT ROW)", true)));
        UNIT_ASSERT(SyntaxFingerprint(Ast("SELECT SUM(x) FILTER(WHERE x > 0)", true)) !=
            SyntaxFingerprint(Ast("SELECT SUM(x) FILTER(WHERE x > 1)", true)));
    }

    Y_UNIT_TEST(AnsiSpellingsAndMultipleStatements) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("--!ansi_lexer\nEXPLAIN QUERY PLAN SELECT 'a''b' AS \"a\"\"b\"; SELECT 42ul;", grammar);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements.size(), 2);
            UNIT_ASSERT(syntax.Statements[0].Explain == EExplainMode::QueryPlan);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements[0].Position.Line, 2);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements[0].Position.Column, 1);
            UNIT_ASSERT(syntax.Statements[1].Explain == EExplainMode::None);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 0).Text, "a'b");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements[0].Query->Projection[0].Alias, "a\"b");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[syntax.Statements[1].Query->Projection[0].Expression].Text, "42ul");
            UNIT_ASSERT(Ast("EXPLAIN SELECT 1", grammar).Statements[0].Explain == EExplainMode::Explain);
        }
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(Parse("EXPLAIN QUERY PLAN SELECT 1; SELECT 2")),
            SyntaxFingerprint(ParseGrammar("EXPLAIN QUERY PLAN SELECT 1; SELECT 2")));
        UNIT_ASSERT(SyntaxFingerprint(Parse("EXPLAIN SELECT 1")) != SyntaxFingerprint(Parse("SELECT 1")));
    }

    Y_UNIT_TEST(AnyUsingAndQualifiedStar) {
        const auto* sql = "SELECT l.*, r.v FROM ANY t AS l INNER JOIN ANY u AS r USING(id);";
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)));
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(sql, grammar);
            const auto& select = *syntax.Statements[0].Query;
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Star);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 0).Text, "l");
            UNIT_ASSERT(select.Source->Any && select.Joins[0].Source.Any);
            UNIT_ASSERT_VALUES_EQUAL(select.Source->Position.Offset, std::string_view(sql).find("ANY"));
            UNIT_ASSERT_VALUES_EQUAL(select.Joins[0].Source.Position.Offset, std::string_view(sql).rfind("ANY"));
            UNIT_ASSERT_VALUES_EQUAL(select.Joins[0].Using[0], "id");
            UNIT_ASSERT(select.Joins[0].Predicate == NoSyntax);
        }
    }

    Y_UNIT_TEST(AnyJoinInputsAreIndependent) {
        for (const bool grammar : {false, true}) for (const bool ansi : {false, true})
        for (const bool leftAny : {false, true}) for (const bool rightAny : {false, true}) {
            const std::string sql = std::string(ansi ? "--!ansi_lexer\n" : "") + "SELECT * FROM "
                + (leftAny ? "ANY " : "") + "t AS l LEFT JOIN " + (rightAny ? "ANY " : "") + "u AS r ON l.id = r.id;";
            const auto syntax = Ast(sql, grammar);
            const auto& select = *syntax.Statements[0].Query;
            const auto& join = select.Joins[0];
            UNIT_ASSERT(join.Kind == EJoinKind::Left);
            UNIT_ASSERT_VALUES_EQUAL(select.Source->Any, leftAny);
            UNIT_ASSERT_VALUES_EQUAL(join.Source.Any, rightAny);
            UNIT_ASSERT_VALUES_EQUAL(select.Source->Alias, "l");
            UNIT_ASSERT_VALUES_EQUAL(join.Source.Alias, "r");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[join.Predicate].Text, "=");
        }
    }

    Y_UNIT_TEST(AnyDoesNotLeakAcrossJoinChainOrSubquery) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT * FROM ANY t AS l
                JOIN ANY (SELECT * FROM u JOIN w ON u.id = w.id) WITH infer AS r SAMPLE 0.5 ON l.id = r.id
                JOIN v AS s ON r.id = s.id;)", grammar);
            const auto& select = *syntax.Statements[0].Query;
            UNIT_ASSERT_VALUES_EQUAL(select.Joins.size(), 2);
            UNIT_ASSERT(select.Source->Any && select.Joins[0].Source.Any);
            UNIT_ASSERT(!select.Joins[1].Source.Any);
            const auto& derived = select.Joins[0].Source;
            UNIT_ASSERT_VALUES_EQUAL(derived.Alias, "r");
            UNIT_ASSERT_VALUES_EQUAL(derived.Options->Hints.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(std::get<TGenericSourceHint>(derived.Options->Hints[0].Value).Name, "infer");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[derived.Options->Sampling->Rate].Text, "0.5");
            UNIT_ASSERT(!derived.Query->Source->Any && !derived.Query->Joins[0].Source.Any);
            UNIT_ASSERT_VALUES_EQUAL(select.Joins[1].Source.Alias, "s");
            UNIT_ASSERT(!select.Joins[1].Source.Options);
        }
    }

    Y_UNIT_TEST(SamplingModesAndSourcePlacement) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT * FROM t AS l SAMPLE $fraction
                JOIN (SELECT * FROM u) AS r TABLESAMPLE BERNOULLI(10 + 5) REPEATABLE($seed)
                ON l.id = r.id; SELECT * FROM t TABLESAMPLE SYSTEM(25);)", grammar);
            const auto& select = *syntax.Statements[0].Query;
            const auto& left = *select.Source->Options->Sampling;
            UNIT_ASSERT(left.Mode == ESamplingMode::Sample && left.Seed == NoSyntax);
            UNIT_ASSERT(syntax.Expressions[left.Rate].Kind == ESyntaxKind::Variable);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[left.Rate].Text, "fraction");
            const auto& derived = select.Joins[0].Source;
            UNIT_ASSERT(derived.Query && !derived.Query->Source->Options);
            UNIT_ASSERT_VALUES_EQUAL(derived.Alias, "r");
            const auto& right = *derived.Options->Sampling;
            UNIT_ASSERT(right.Mode == ESamplingMode::Bernoulli);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[right.Rate].Text, "+");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[right.Seed].Text, "seed");
            const auto& system = *syntax.Statements[1].Query->Source->Options->Sampling;
            UNIT_ASSERT(system.Mode == ESamplingMode::System && system.Seed == NoSyntax);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[system.Rate].Text, "25");
        }
    }

    Y_UNIT_TEST(GenericHintsPreserveValuesAndOrder) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(SELECT * FROM t WITH (infer, format = 'csv',
                flags = (foo, 'bar', $baz), format = json, one = (foo)) AS src SAMPLE 0.5;
                SELECT * FROM u WITH truncate;)", grammar);
            const auto& source = *syntax.Statements[0].Query->Source;
            const auto& hints = source.Options->Hints;
            const auto hint = [&](std::size_t i) -> const TGenericSourceHint& { return std::get<TGenericSourceHint>(hints[i].Value); };
            UNIT_ASSERT_VALUES_EQUAL(hints.size(), 5);
            UNIT_ASSERT_VALUES_EQUAL(hint(0).Name, "infer");
            UNIT_ASSERT(hint(0).Form == EHintValueForm::None && hint(0).Values.empty());
            UNIT_ASSERT(hint(1).Form == EHintValueForm::Scalar);
            UNIT_ASSERT(hint(1).Values[0].Kind == EHintValueKind::String);
            UNIT_ASSERT_VALUES_EQUAL(hint(1).Values[0].Text, "csv");
            UNIT_ASSERT(hint(2).Form == EHintValueForm::List);
            UNIT_ASSERT_VALUES_EQUAL(hint(2).Values.size(), 3);
            UNIT_ASSERT(hint(2).Values[0].Kind == EHintValueKind::Identifier);
            UNIT_ASSERT_VALUES_EQUAL(hint(2).Values[0].Text, "foo");
            UNIT_ASSERT(hint(2).Values[1].Kind == EHintValueKind::String);
            UNIT_ASSERT_VALUES_EQUAL(hint(2).Values[1].Text, "bar");
            UNIT_ASSERT(hint(2).Values[2].Kind == EHintValueKind::Binding);
            UNIT_ASSERT_VALUES_EQUAL(hint(2).Values[2].Text, "baz");
            UNIT_ASSERT_VALUES_EQUAL(hint(3).Name, "format");
            UNIT_ASSERT(hint(3).Values[0].Kind == EHintValueKind::Identifier);
            UNIT_ASSERT(hint(4).Form == EHintValueForm::List && hint(4).Values.size() == 1);
            UNIT_ASSERT_VALUES_EQUAL(source.Alias, "src");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[source.Options->Sampling->Rate].Text, "0.5");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements[1].Query->Source->Options->Hints.size(), 1);
            const auto quoted = Ast("--!ansi_lexer\nSELECT * FROM t WITH (\"hint\" = 'a''b');", grammar);
            const auto& quotedHint = std::get<TGenericSourceHint>(quoted.Statements[0].Query->Source->Options->Hints[0].Value);
            UNIT_ASSERT_VALUES_EQUAL(quotedHint.Name, "hint");
            UNIT_ASSERT_VALUES_EQUAL(quotedHint.Values[0].Text, "a'b");
        }
    }

    Y_UNIT_TEST(SourceModifiersRequireExplicitExecutionAdmission) {
        for (const bool grammar : {false, true}) for (const auto* sql : {
            "SELECT l.id FROM ANY t AS l JOIN u AS r ON l.id = r.id",
            "SELECT * FROM t WITH infer", "SELECT * FROM t SAMPLE 0.5",
            "EXPLAIN SELECT * FROM t", "EXPLAIN QUERY PLAN SELECT * FROM t"}) {
            const auto syntax = Ast(sql, grammar);
            UNIT_ASSERT_EXCEPTION_C(CollectTables(syntax), TUnsupportedFeature, sql);
            UNIT_ASSERT_EXCEPTION_C(BindNative(syntax, [](std::string_view, TSourcePosition) -> TCatalogTable {
                UNIT_FAIL("unsupported source semantics reached catalog I/O");
                return {};
            }), TUnsupportedFeature, sql);
        }
        UNIT_ASSERT(SyntaxFingerprint(Ast("SELECT * FROM t WITH format = csv", true)) !=
            SyntaxFingerprint(Ast("SELECT * FROM t WITH format = json", true)));
        UNIT_ASSERT(SyntaxFingerprint(Ast("SELECT * FROM t SAMPLE 0.5", true)) !=
            SyntaxFingerprint(Ast("SELECT * FROM t SAMPLE 0.6", true)));
    }

    Y_UNIT_TEST(PragmasPreserveOrderAndValueForms) {
        const auto* sql = R"(PRAGMA OrderedColumns;
            PRAGMA yt.Pool = 'batch'; SELECT 1;
            PRAGMA Custom(-42, +1.5, true, foo, $value, 'a\n');
            PRAGMA yt.Pool = default; SELECT 2;)";
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)));
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(sql, grammar);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Statements.size(), 6);
            const auto& bare = syntax.Statements[0];
            UNIT_ASSERT_VALUES_EQUAL(bare.Name, "OrderedColumns");
            UNIT_ASSERT(bare.PragmaForm == EPragmaValueForm::None && bare.PragmaValues.empty());
            const auto& pool = syntax.Statements[1];
            UNIT_ASSERT_VALUES_EQUAL(pool.Name, "yt.Pool");
            UNIT_ASSERT(pool.PragmaForm == EPragmaValueForm::Assignment);
            UNIT_ASSERT(pool.PragmaValues[0].Kind == EPragmaValueKind::String);
            UNIT_ASSERT_VALUES_EQUAL(pool.PragmaValues[0].Text, "batch");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[syntax.Statements[2].Query->Projection[0].Expression].Text, "1");
            const auto& custom = syntax.Statements[3];
            UNIT_ASSERT(custom.PragmaForm == EPragmaValueForm::List);
            UNIT_ASSERT_VALUES_EQUAL(custom.PragmaValues.size(), 6);
            using K = EPragmaValueKind;
            const K kinds[]{K::Integer, K::Real, K::Boolean, K::Identifier, K::Binding, K::String};
            const char* values[]{"-42", "+1.5", "TRUE", "foo", "$value", "a\n"};
            for (unsigned i = 0; i < std::size(kinds); ++i) {
                UNIT_ASSERT(custom.PragmaValues[i].Kind == kinds[i]);
                UNIT_ASSERT_VALUES_EQUAL(custom.PragmaValues[i].Text, values[i]);
            }
            const auto& reset = syntax.Statements[4];
            UNIT_ASSERT(reset.PragmaForm == EPragmaValueForm::Assignment);
            UNIT_ASSERT(reset.PragmaValues[0].Kind == K::Identifier);
            UNIT_ASSERT_VALUES_EQUAL(reset.PragmaValues[0].Text, "default");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions[syntax.Statements[5].Query->Projection[0].Expression].Text, "2");
        }
    }

    Y_UNIT_TEST(Predicates) {
        const auto* sql = R"(SELECT x NOT IN (1, 2), x NOT BETWEEN SYMMETRIC 1 AND 3,
            x IS NOT NULL, x IS NOT DISTINCT FROM y, name NOT LIKE 'a%' ESCAPE '!';)";
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)));
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(sql, grammar);
            const auto& in = Projection(syntax, 0);
            UNIT_ASSERT(in.Kind == ESyntaxKind::Unary && in.Text == "NOT");
            UNIT_ASSERT(Child(syntax, in, 0).Kind == ESyntaxKind::In);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, in, 0).Arguments.size(), 3);
            const auto& between = Projection(syntax, 1);
            UNIT_ASSERT(between.Kind == ESyntaxKind::Between && between.Text == "NOT BETWEEN SYMMETRIC");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, between, 1).Text, "1");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, between, 2).Text, "3");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 2).Text, "IS NOT NULL");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "IS NOT DISTINCT FROM");
            const auto& like = Projection(syntax, 4);
            UNIT_ASSERT(like.Kind == ESyntaxKind::Unary && like.Text == "NOT");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, like, 0).Text, "LIKE");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Child(syntax, like, 0), 2).Text, "!");
        }
    }

    Y_UNIT_TEST(NamedPreambleAndLambdaBodies) {
        const auto* sql = "$f = ($x, $y) -> { $n = $x + 1; return $n + $y; }; "
            "$g = () -> (42); $q = SELECT $f(id, 2) AS k FROM t; "
            "$r = (WITH c AS (SELECT k FROM $q) SELECT * FROM c); SELECT * FROM $r;";
        const auto fast = Parse(sql), grammar = ParseGrammar(sql);
        UNIT_ASSERT_VALUES_EQUAL(SyntaxFingerprint(fast), SyntaxFingerprint(grammar));
        for (const auto* parsed : {&fast, &grammar}) {
            UNIT_ASSERT_VALUES_EQUAL(parsed->Statements.size(), 5);
            const auto& f = parsed->Expressions[parsed->Statements[0].Expression];
            UNIT_ASSERT(f.Kind == ESyntaxKind::Lambda);
            UNIT_ASSERT(f.Lambda->Parameters == (std::vector<std::string>{"x", "y"}));
            UNIT_ASSERT(f.Lambda->Locals == (std::vector<std::string>{"n"}));
            UNIT_ASSERT_VALUES_EQUAL(f.Arguments.size(), 2);
            UNIT_ASSERT(parsed->Expressions[f.Arguments.back()].Kind == ESyntaxKind::Binary);
            UNIT_ASSERT(parsed->Expressions[parsed->Statements[1].Expression].Lambda->Parameters.empty());
            UNIT_ASSERT(parsed->Statements[2].Query);
            UNIT_ASSERT_VALUES_EQUAL(parsed->Statements[3].Query->Ctes.size(), 1);
        }
    }

    Y_UNIT_TEST(SetOperationRejectsMisplacedTrailingClauses) {
        for (const bool grammar : {false, true}) {
            for (const auto* sql : {
                "SELECT 1 AS n ORDER BY n UNION ALL SELECT 2 AS n;",
                "SELECT 1 AS n LIMIT 1 INTERSECT SELECT 2 AS n;",
                "SELECT 1 AS n UNION ALL SELECT 2 AS n ORDER BY n EXCEPT SELECT 3 AS n;",
                "SELECT 1 AS n UNION ALL SELECT 2 AS n ASSUME ORDER BY n;"}) {
                UNIT_ASSERT_EXCEPTION_C(Ast(sql, grammar), TFrontendError, sql);
            }
        }
    }

    Y_UNIT_TEST(SetOperationTrailingClauses) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT 3 AS n UNION ALL SELECT 2 AS n INTERSECT SELECT 1 AS n ORDER BY n DESC LIMIT 2 OFFSET 1;", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            UNIT_ASSERT_VALUES_EQUAL(query.OrderBy.size(), 1);
            UNIT_ASSERT(query.OrderBy[0].Descending);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Limit).Text, "2");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Offset).Text, "1");
            UNIT_ASSERT_VALUES_EQUAL(query.SetInputs.size(), 3);
            for (const auto& input : query.SetInputs) {
                UNIT_ASSERT(input->OrderBy.empty());
                UNIT_ASSERT(input->Limit == NoSyntax);
            }
            const auto local = Ast("(SELECT 3 AS n ORDER BY n LIMIT 1) UNION ALL (SELECT 2 AS n ORDER BY n DESC LIMIT 1);", grammar);
            const auto& compound = *local.Statements.at(0).Query;
            UNIT_ASSERT(compound.OrderBy.empty());
            UNIT_ASSERT(compound.Limit == NoSyntax);
            UNIT_ASSERT_VALUES_EQUAL(compound.SetInputs.size(), 2);
            for (const auto& input : compound.SetInputs) {
                UNIT_ASSERT_VALUES_EQUAL(input->OrderBy.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(local.Expressions.at(input->Limit).Text, "1");
            }
        }
    }

    Y_UNIT_TEST(PrecedenceAndAssociativity) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT 10 - 3 - 2 * 4, a OR b AND c, x ?? y ?? z, -x * y;", grammar);
            const auto& subtract = Projection(syntax, 0);
            UNIT_ASSERT(subtract.Kind == ESyntaxKind::Binary);
            UNIT_ASSERT_VALUES_EQUAL(subtract.Text, "-");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, subtract, 0).Text, "-");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, subtract, 1).Text, "*");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Child(syntax, subtract, 0), 0).Text, "10");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Child(syntax, subtract, 1), 1).Text, "4");
            const auto& logical = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(logical.Text, "OR");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, logical, 1).Text, "AND");
            const auto& coalesce = Projection(syntax, 2);
            UNIT_ASSERT_VALUES_EQUAL(coalesce.Text, "??");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, coalesce, 1).Text, "??");
            const auto& negate = Child(syntax, Projection(syntax, 3), 0);
            UNIT_ASSERT(negate.Kind == ESyntaxKind::Unary);
            UNIT_ASSERT_VALUES_EQUAL(negate.Text, "-");
        }
    }

    Y_UNIT_TEST(SelectClausesAndJoin) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast(R"(
                SELECT DISTINCT t.id AS id, COUNT(DISTINCT u.value) AS n
                FROM `/Root/t` AS t LEFT JOIN `/Root/u` AS u ON t.id = u.id
                WHERE t.id > $min GROUP BY t.id HAVING COUNT(*) > 1
                ORDER BY n DESC, id ASC LIMIT 10 OFFSET 2;
            )", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            UNIT_ASSERT(query.Distinct);
            UNIT_ASSERT_VALUES_EQUAL(query.Projection.at(0).Alias, "id");
            UNIT_ASSERT(Projection(syntax, 1).Distinct);
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Name, "/Root/t");
            UNIT_ASSERT_VALUES_EQUAL(query.Source->Alias, "t");
            const auto& join = query.Joins.at(0);
            UNIT_ASSERT(join.Kind == EJoinKind::Left);
            UNIT_ASSERT_VALUES_EQUAL(join.Source.Name, "/Root/u");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(join.Predicate).Text, "=");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, syntax.Expressions.at(query.Where), 1).Text, "min");
            UNIT_ASSERT_VALUES_EQUAL(query.GroupBy.size(), 1);
            const auto& count = Child(syntax, syntax.Expressions.at(query.Having), 0);
            UNIT_ASSERT(Child(syntax, count, 0).Kind == ESyntaxKind::Star);
            UNIT_ASSERT_VALUES_EQUAL(query.OrderBy.size(), 2);
            UNIT_ASSERT(query.OrderBy[0].Descending);
            UNIT_ASSERT(!query.OrderBy[1].Descending);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Limit).Text, "10");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(query.Offset).Text, "2");
            // The reference's cluster qualifier is outside this native catalog
            // contract. It must be rejected, never stripped from the table name.
            UNIT_ASSERT_EXCEPTION(Ast("SELECT * FROM plato.`/Root/t`;", grammar), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(DerivedTableAndCommaLimit) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT d.id FROM (SELECT id FROM t LIMIT 2, 5) AS d;", grammar);
            const auto& source = *syntax.Statements.at(0).Query->Source;
            UNIT_ASSERT_VALUES_EQUAL(source.Alias, "d");
            UNIT_ASSERT(source.Query);
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(source.Query->Limit).Text, "5");
            UNIT_ASSERT_VALUES_EQUAL(syntax.Expressions.at(source.Query->Offset).Text, "2");
        }
    }

    Y_UNIT_TEST(InSingletonCollectionsPreserveTheirOuterTuple) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT x IN (1), x IN ((1, 2)), x IN $items, x IN (SELECT id FROM t), (1), x IN ((SELECT id FROM t)), "
                "x IN ((SELECT id FROM t),), x IN (((SELECT id FROM t)));", grammar);
            const auto& singleton = Projection(syntax, 0);
            UNIT_ASSERT(singleton.Kind == ESyntaxKind::In);
            UNIT_ASSERT_VALUES_EQUAL(singleton.Arguments.size(), 2);
            UNIT_ASSERT(Child(syntax, singleton, 1).Kind == ESyntaxKind::Integer);
            const auto& tuples = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(tuples.Arguments.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, tuples, 1).Arguments.size(), 2);
            UNIT_ASSERT(Child(syntax, Projection(syntax, 2), 1).Kind == ESyntaxKind::Variable);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, Projection(syntax, 2), 1).Text, "items");
            UNIT_ASSERT(Projection(syntax, 3).Query);
            UNIT_ASSERT(Projection(syntax, 4).Kind == ESyntaxKind::Integer);
            UNIT_ASSERT(Projection(syntax, 5).Query);
            UNIT_ASSERT(!Projection(syntax, 6).Query);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 6).Arguments.size(), 2);
            UNIT_ASSERT(Child(syntax, Projection(syntax, 6), 1).Kind == ESyntaxKind::ScalarSubquery);
            UNIT_ASSERT(Projection(syntax, 7).Query);
        }
    }

    Y_UNIT_TEST(ParenthesesCollectionsAndSubqueries) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT (1 + 2), (1,), (), [1, 2], EXISTS (SELECT 1 FROM t), (SELECT MAX(x) FROM u);", grammar);
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Binary);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 0).Text, "+");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 1).Text, "AsTuple");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 1).Arguments.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 2).Text, "AsTuple");
            UNIT_ASSERT(Projection(syntax, 2).Arguments.empty());
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "AsList");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Arguments.size(), 2);
            UNIT_ASSERT(Projection(syntax, 4).Kind == ESyntaxKind::Exists && Projection(syntax, 4).Query);
            UNIT_ASSERT(Projection(syntax, 5).Kind == ESyntaxKind::ScalarSubquery && Projection(syntax, 5).Query);
        }
    }

    Y_UNIT_TEST(CaseAndCast) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT CASE x WHEN 1 THEN 'one' ELSE 'many' END, CASE WHEN x > 0 THEN 1 END, "
                "CAST($x AS Int64?), CAST(1 AS Decimal(22, 9)), CAST($items AS List<Int32>);", grammar);
            const auto& simple = Projection(syntax, 0);
            UNIT_ASSERT(simple.Kind == ESyntaxKind::Case);
            UNIT_ASSERT_VALUES_EQUAL(simple.Arguments.size(), 3);
            const auto& condition = Child(syntax, simple, 0);
            UNIT_ASSERT_VALUES_EQUAL(condition.Text, "==");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, condition, 0).Text, "x");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, condition, 1).Text, "1");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, simple, 2).Text, "many");
            const auto& searched = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(searched.Text, "implicit_else");
            UNIT_ASSERT(Child(syntax, searched, 2).Kind == ESyntaxKind::Null);
            UNIT_ASSERT(Projection(syntax, 2).Kind == ESyntaxKind::Cast);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 2).Text, "Int64?");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "Decimal(22,9)");
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 4).Text, "List<Int32>");
        }
    }

    Y_UNIT_TEST(CteAndSetOperationPrecedence) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("WITH c(id) AS (SELECT 1) SELECT id FROM c UNION ALL SELECT 2 INTERSECT SELECT 3 EXCEPT SELECT 4;", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            UNIT_ASSERT_VALUES_EQUAL(query.Ctes.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(query.Ctes[0].Name, "c");
            UNIT_ASSERT_VALUES_EQUAL(query.Ctes[0].Columns.at(0), "id");
            const std::vector<ESetOperation> expected{ESetOperation::Push, ESetOperation::Push, ESetOperation::Push,
                ESetOperation::Intersect, ESetOperation::UnionAll, ESetOperation::Push, ESetOperation::Except};
            UNIT_ASSERT(query.SetOperations == expected);
        }
    }

    Y_UNIT_TEST(GroupingSets) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT a, COUNT(*) FROM t GROUP BY GROUPING SETS ((), ROLLUP(a, b), CUBE(a, b));", grammar);
            const auto& query = *syntax.Statements.at(0).Query;
            // Native syntax expands bounded grouping combinations once. Retain
            // duplicate sets: they produce duplicate aggregate result groups.
            std::vector<std::vector<std::string>> sets;
            for (const auto& set : query.GroupingSets) {
                std::vector<std::string> keys;
                for (auto key : set) keys.push_back(syntax.Expressions.at(query.GroupBy.at(key).Expression).Text);
                sets.push_back(std::move(keys));
            }
            std::vector<std::vector<std::string>> expected{{}, {"a", "b"}, {"a"}, {}, {}, {"a"}, {"b"}, {"a", "b"}};
            std::sort(sets.begin(), sets.end());
            std::sort(expected.begin(), expected.end());
            UNIT_ASSERT(sets == expected);
        }
    }

    Y_UNIT_TEST(MemberIndexAndModuleCall) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT t.record.field, $items[0], Math::Abs(-$value);", grammar);
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Column);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 0).Text, "t.record.field");
            const auto& index = Projection(syntax, 1);
            UNIT_ASSERT(index.Kind == ESyntaxKind::Index);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, index, 0).Text, "items");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, index, 1).Text, "0");
            const auto& call = Projection(syntax, 2);
            UNIT_ASSERT(call.Kind == ESyntaxKind::Call);
            UNIT_ASSERT_VALUES_EQUAL(call.Text, "Math::Abs");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, call, 0).Text, "-");
        }
    }

    Y_UNIT_TEST(SourcePositionsAndOwnership) {
        for (const bool grammar : {false, true}) {
            auto syntax = Ast("-- comment\nSELECT 'Ж', x + 12;", grammar);
            const auto& literal = Projection(syntax, 0);
            UNIT_ASSERT_VALUES_EQUAL(literal.Position.Line, 2);
            UNIT_ASSERT_VALUES_EQUAL(literal.Position.Column, 8);
            UNIT_ASSERT_VALUES_EQUAL(literal.Text, "Ж");
            const auto& binary = Projection(syntax, 1);
            UNIT_ASSERT_VALUES_EQUAL(binary.Position.Line, 2);
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, binary, 0).Text, "x");
            UNIT_ASSERT_VALUES_EQUAL(Child(syntax, binary, 1).Text, "12");
            // Ast receives its SQL by value, so the source and parser have already
            // been destroyed. Moving the owning arena must preserve its contents.
            const auto moved = std::move(syntax);
            UNIT_ASSERT_VALUES_EQUAL(Projection(moved, 0).Text, "Ж");
            UNIT_ASSERT_VALUES_EQUAL(Child(moved, Projection(moved, 1), 0).Text, "x");
        }
    }

    Y_UNIT_TEST(EmptyScript) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("-- comment\n", grammar);
            UNIT_ASSERT(syntax.Statements.empty());
            UNIT_ASSERT(syntax.Expressions.empty());
        }
    }

    Y_UNIT_TEST(UnsupportedSyntaxDoesNotDisappear) {
        for (const bool grammar : {false, true}) {
            for (const auto* sql : {
                "SELECT * WITHOUT id FROM t;",
                "SELECT x = y = z;",
                "SELECT * FROM t OUTER JOIN u;",
                "SELECT * FROM t WITH SCHEMA Callable<() -> Int32>;",
                "SELECT * FROM ANY t;"}) {
                UNIT_ASSERT_EXCEPTION_C(Ast(sql, grammar), TFrontendError, sql);
            }
            // DELETE was outside the reference AST profile. Our effects profile
            // accepts it, so require its complete statement instead of dropping it.
            for (const auto* sql : {"SELECT 1; DELETE FROM t;", "PRAGMA OrderedColumns; DELETE FROM t;"}) {
                const auto syntax = Ast(sql, grammar);
                UNIT_ASSERT_VALUES_EQUAL(syntax.Statements.size(), 2);
                const auto& statement = syntax.Statements[1];
                UNIT_ASSERT(statement.Kind == EStatementKind::Write && statement.Write);
                UNIT_ASSERT(statement.Write->Kind == EWriteKind::Delete);
                UNIT_ASSERT_VALUES_EQUAL(statement.Write->Target.Name, "t");
                UNIT_ASSERT(statement.Write->Where == NoSyntax);
            }
        }
    }

    Y_UNIT_TEST(LiteralKindsAndSpellings) {
        for (const bool grammar : {false, true}) {
            const auto syntax = Ast("SELECT NULL, true, 1.25, 42ul, 'a\\n';", grammar);
            UNIT_ASSERT(Projection(syntax, 0).Kind == ESyntaxKind::Null);
            UNIT_ASSERT(Projection(syntax, 1).Kind == ESyntaxKind::Boolean);
            UNIT_ASSERT(Projection(syntax, 2).Kind == ESyntaxKind::Real);
            UNIT_ASSERT(Projection(syntax, 3).Kind == ESyntaxKind::Integer);
            UNIT_ASSERT(Projection(syntax, 4).Kind == ESyntaxKind::String);
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 3).Text, "42ul");
            // Native syntax owns decoded strings instead of retaining quote tokens.
            UNIT_ASSERT_VALUES_EQUAL(Projection(syntax, 4).Text, "a\n");
        }
    }
}
