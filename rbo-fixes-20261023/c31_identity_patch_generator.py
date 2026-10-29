"""Render a scoped C31 production amendment; no test workloads are written."""
import difflib
import subprocess

SOURCE_COMMIT = '3d6548a9d6168d93276ac6a780fcd803d009fc5e'
SOURCE_PATH = 'ydb/core/kqp/opt/rbo/rules/inline_join_filters.cpp'

TYPE_HELPER = r'''// Grouping equality and StablePickle agree for these scalar values. In
// particular, do not merge complete floating rows by canonicalized zero/NaN.
bool HasExactScalarIdentity(const TTypeAnnotationNode* type) {
    if (!type) {
        return false;
    }
    if (type->GetKind() == ETypeAnnotationKind::Optional) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    if (type->GetKind() != ETypeAnnotationKind::Data) {
        return false;
    }
    switch (type->Cast<TDataExprType>()->GetSlot()) {
        case NUdf::EDataSlot::Bool:
        case NUdf::EDataSlot::Int8: case NUdf::EDataSlot::Uint8:
        case NUdf::EDataSlot::Int16: case NUdf::EDataSlot::Uint16:
        case NUdf::EDataSlot::Int32: case NUdf::EDataSlot::Uint32:
        case NUdf::EDataSlot::Int64: case NUdf::EDataSlot::Uint64:
        case NUdf::EDataSlot::Date: case NUdf::EDataSlot::Datetime:
        case NUdf::EDataSlot::Timestamp: case NUdf::EDataSlot::Interval:
        case NUdf::EDataSlot::Date32: case NUdf::EDataSlot::Datetime64:
        case NUdf::EDataSlot::Timestamp64: case NUdf::EDataSlot::Interval64:
        case NUdf::EDataSlot::String: case NUdf::EDataSlot::Utf8:
        case NUdf::EDataSlot::Decimal:
            return true;
        default:
            return false;
    }
}
'''

PREPARE = r'''    auto leftInput = join->GetLeftInput();
    auto keyColumns = leftInput->Props.Metadata->KeyColumns;
    const bool needsDomain = keyColumns.Items().empty();
    TUnorderedIUs nullableKeys;
    if (needsDomain) {
        // One inner evaluation per complete left value, then reconstruct
        // against the original bag. Volatile predicates need occurrence IDs.
        Y_ENSURE(CanDuplicateOperator(*join), "Join filter domain requires repeatable predicates");
        for (const auto call : join->GetSubplanIUs(props.Subplans)) {
            Y_ENSURE(CanDuplicateSubtree(*props.Subplans.At(call).Plan, props.Subplans),
                "Join filter domain requires repeatable subplans");
        }
        const auto& columns = leftInput->GetOutputIUs();
        keyColumns = TOrderedIUs<>(columns.begin(), columns.end());
    }
    for (const auto column : keyColumns.Items()) {
        const auto* type = leftInput->GetIUType(column, ctx.ExprCtx);
        Y_ENSURE(type, "Missing join filter identity type");
        if (needsDomain || type->IsOptionalOrNull()) {
            Y_ENSURE(HasExactScalarIdentity(type), "Unsupported join filter identity type");
        }
        if (type->IsOptionalOrNull()) {
            nullableKeys.Add(column);
        }
    }
    if (keyColumns.Items().empty()) {
        // A zero-column bag still has multiplicity; use a one-value domain.
        const auto unit = props.InfoUnitRegistry.AddGenerated("join_filter_unit");
        TMapIUs elements;
        elements.Add(unit, MakeConstant("Uint64", "0", join->Pos, &ctx.ExprCtx));
        leftInput = MakeIntrusive<TOpMap>(leftInput, join->Pos, std::move(elements));
        keyColumns.Append(unit);
    }

'''

PORTS = r'''    auto hub = TReplicate::Create(leftInput, join->Pos, props.InfoUnitRegistry);
    TIntrusivePtr<IOperator> outerLeftInput = hub->AddOutput();
    auto innerPort = hub->AddOutput();
    const auto renameMap = innerPort->GetRebindings();
    TIntrusivePtr<IOperator> innerLeftInput = innerPort;
    if (needsDomain) {
        TUnorderedIUs columns;
        for (const auto column : keyColumns.Items()) {
            columns.Add(renameMap.At(column));
        }
        innerLeftInput = MakeDomainProjection(innerLeftInput, columns, join->Pos);
    }
'''

KEYS = r'''    TJoinIUs newJoinKeys;
    TMapIUs outerKeys;
    TMapIUs innerKeys;
    auto encode = [&](TInfoUnitId column, TMapIUs& elements) {
        const auto encoded = props.InfoUnitRegistry.AddGenerated("join_filter_key");
        elements.Add(encoded, MakeUnaryCallable("StablePickle",
            MakeColumnAccess(column, join->Pos, &ctx.ExprCtx, &props)));
        return encoded;
    };
    for (const auto column : keyColumns.Items()) {
        if (nullableKeys.Contains(column)) {
            // String keys are nonoptional and compare NULL identity exactly,
            // independently of native EqualNulls support in the chosen join.
            newJoinKeys.Add(encode(column, outerKeys), encode(renameMap.At(column), innerKeys));
        } else {
            newJoinKeys.Add(column, renameMap.At(column));
        }
    }
    if (!outerKeys.Keys().Empty()) {
        outerLeftInput = MakeIntrusive<TOpMap>(outerLeftInput, join->Pos, std::move(outerKeys));
        newFilter = MakeIntrusive<TOpMap>(newFilter, join->Pos, std::move(innerKeys));
    }
'''


def _replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('production_patch_anchor_is_not_unique')
    return source.replace(before, after, 1)


def render_files(repository):
    before = subprocess.check_output(['git', 'show', SOURCE_COMMIT + ':' + SOURCE_PATH],
                                     cwd=repository, text=True)
    after = _replace_once(before, '#include "kqp_rules_include.h"',
        '#include "kqp_rules_include.h"\n\n#include "decorrelation/dependent_join_pushdown.h"\n#include <ydb/core/kqp/opt/rbo/copy_logical_subtree.h>')
    begin = after.index('bool CheckNonNullKeys(')
    end = after.index('\n}\n\n}', begin) + 2
    after = after[:begin] + TYPE_HELPER.rstrip() + after[end:]
    anchor = '    // The inner join reads the left input again:'
    after = _replace_once(after, anchor, PREPARE + anchor)
    begin = after.index('    auto hub = TReplicate::Create(')
    end = after.index('\n\n    // Build an inner join', begin)
    after = after[:begin] + PORTS.rstrip() + after[end:]
    after = _replace_once(after,
        '    auto newFilter = MakeIntrusive<TOpFilter>(innerJoin, input->Pos, filterExpr);',
        '    TIntrusivePtr<IOperator> newFilter = MakeIntrusive<TOpFilter>(innerJoin, input->Pos, filterExpr);')
    begin = after.index('    // The join will be on the keys of lhs,')
    end = after.index('\n    // Subplans called from the join filters', begin)
    after = after[:begin] + KEYS + after[end:]
    return {SOURCE_PATH: (before, after)}


def render_patch(repository):
    return ''.join('diff --git a/' + path + ' b/' + path + '\n' +
        ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                    fromfile='a/' + path, tofile='b/' + path))
        for path, (before, after) in render_files(repository).items())
