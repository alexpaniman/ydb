"""Independent C31 source and bag-law review; generated relations stay in RAM."""
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import random
import subprocess

ROOT = Path(__file__).resolve().parent
BASE = '3d6548a9d6168d93276ac6a780fcd803d009fc5e'
PATCH = ROOT / 'proposed-correctness-amendments/C31-scalar-row-identity.patch'
EXPECTED = '9ed0d81563d8dd825b28de0bbe71003d4b8263d92ce9ac1b8f90f3b8a7801f0b'


def predicate(left, right):
    # A total pure relation over the entire visible left value.
    return (sum(sum(str(item).encode()) for item in left) + right * 3) % 7 in (0, 2, 3)


def direct(left, right, kind):
    result = []
    for row in left:
        matches = [other for other in right if predicate(row, other)]
        if kind == 'left':
            result.extend((row, other) for other in matches)
            if not matches:
                result.append((row, None))
        elif bool(matches) == (kind == 'semi'):
            result.append(row)
    return Counter(result)


def reconstructed(left, right, kind, unique, deduplicate=True):
    domain = list(left) if unique or not deduplicate else list(set(left))
    filtered = defaultdict(list)
    for row in domain:
        # This abstract identity assumes the independently reviewed scalar
        # equality/encoding law; it does not emulate production serialization.
        identity = tuple(('null',) if item is None else ('value', item) for item in row) or (('unit',),)
        filtered[identity].extend(other for other in right if predicate(row, other))
    result = []
    for row in left:
        identity = tuple(('null',) if item is None else ('value', item) for item in row) or (('unit',),)
        matches = filtered.get(identity, [])
        if kind == 'left':
            result.extend((row, other) for other in matches)
            if not matches:
                result.append((row, None))
        elif bool(matches) == (kind == 'semi'):
            result.append(row)
    return Counter(result)


def main():
    patch = PATCH.read_bytes()
    assert hashlib.sha256(patch).hexdigest() == EXPECTED
    import c31_identity_patch_generator as author
    files = author.render_files(ROOT.parent)
    text = next(iter(files.values()))[1]
    contracts = (
        'const bool needsDomain = keyColumns.Items().empty();',
        'Y_ENSURE(CanDuplicateOperator(*join)',
        'CanDuplicateSubtree(*props.Subplans.At(call).Plan, props.Subplans)',
        'keyColumns = TOrderedIUs<>(columns.begin(), columns.end());',
        'if (needsDomain || type->IsOptionalOrNull())',
        'Y_ENSURE(HasExactScalarIdentity(type)',
        'MakeConstant("Uint64", "0", join->Pos, &ctx.ExprCtx)',
        'keyColumns.Append(unit);',
        'auto hub = TReplicate::Create(leftInput, join->Pos, props.InfoUnitRegistry);',
        'innerLeftInput = MakeDomainProjection(innerLeftInput, columns, join->Pos);',
        'newJoinKeys.Add(encode(column, outerKeys), encode(renameMap.At(column), innerKeys));',
        'props.Subplans.RebindInputs(call, renameMap);',
        'MakeIntrusive<TOpJoin>(outerLeftInput, newFilter, join->Pos, join->JoinKind, newJoinKeys)',
    )
    assert all(item in text for item in contracts)
    assert text.index('keyColumns.Append(unit);') < text.index('auto hub = TReplicate::Create(')
    helper = text[text.index('bool HasExactScalarIdentity'):text.index('\n}\n\n}', text.index('bool HasExactScalarIdentity'))]
    assert all(('EDataSlot::' + excluded + ':') not in helper for excluded in ('Float', 'Double', 'TzDate', 'Json', 'Yson'))
    assert 'if (type->GetKind() != ETypeAnnotationKind::Data)' in helper
    rng = random.Random(311023)
    comparisons = zero_column = empty_left = mutation_rejections = 0
    for width in range(5):
        alphabet = [tuple(rng.choice((None, '', '\x00', 'alpha')) if column % 2 else rng.choice((None, -2, 0, 3))
                          for column in range(width)) for _ in range(8)]
        for trial in range(160):
            left = [rng.choice(alphabet) for _ in range(trial % 11)]
            right = [rng.randrange(5) for _ in range((trial // 3) % 9)]
            for unique in (False, True):
                if unique and not width:
                    continue
                left_input = list(set(left)) if unique else left
                for kind in ('left', 'semi', 'only'):
                    expected = direct(left_input, right, kind)
                    actual = reconstructed(left_input, right, kind, unique)
                    assert actual == expected
                    comparisons += 1
                    zero_column += width == 0
                    empty_left += not left_input
                    if not unique and kind == 'left':
                        mutation_rejections += reconstructed(left_input, right, kind, unique, False) != expected
    assert mutation_rejections > 0
    source_paths = (
        'ydb/core/kqp/opt/rbo/rules/decorrelation/dependent_join_pushdown.cpp',
        'ydb/core/kqp/opt/rbo/copy_logical_subtree.cpp',
        'ydb/core/kqp/opt/rbo/kqp_operator.cpp',
        'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp',
        'yql/essentials/minikql/computation/mkql_computation_node_pack.cpp',
        'yql/essentials/public/udf/udf_type_ops.h',
    )
    source_hashes = {path: hashlib.sha256(subprocess.check_output(['git', 'show', BASE + ':' + path],
                                                                  cwd=ROOT.parent)).hexdigest()
                     for path in source_paths}
    report = {
        'id': 'C31', 'reviewer': 'quality_review', 'independent_of_patch_author': True,
        'base': BASE, 'patch_sha256': EXPECTED, 'status': 'source_and_abstract_bag_contracts_passed_native_pending',
        'source_contracts': len(contracts) + 3, 'bag_comparisons': comparisons,
        'zero_column_comparisons': zero_column, 'empty_left_comparisons': empty_left,
        'naive_duplicate_rejoin_mutations_rejected': mutation_rejections,
        'source_sha256': source_hashes,
        'reviewed_laws': [
            'For each complete scalar left value with multiplicity m and k matching right rows, the domain produces k matches and reconstruction produces m*k rows.',
            'LeftSemi and LeftOnly preserve m original occurrences based on existence; unmatched Left produces m padded rows.',
            'The helper constant is added before Replicate; a zero-column empty bag remains empty under nonempty-key grouping.',
            'Nullable unique identities retain their bag and encode only reconstruction keys; aggregate/domain deduplication is used only without key metadata.',
            'Admitted fixed-width scalars, byte strings, ordinary date/time and Decimal have injective same-type StablePickle representations consistent with grouping equality. Optional encoding distinguishes absent and present values.',
            'Keyless deduplication checks direct residual repeatability and recursively checks called subplans; producer copying retains existing guards.',
            'Both insertion stages precede AssignStages and aggregate phase propagation.',
            'New helper outputs and secondary IDs remain fresh; existing call captures are rebound with the secondary-port substitutions.'
        ],
        'excluded_scope': [
            'Keyless complete rows containing Float/Double, timezone scalars, Pg, opaque/containers, nested optional or bare Null.',
            'New nullable unique identities outside the scalar whitelist.',
            'Volatile/UDF keyless residual predicates or unproved referenced subplans.',
            'Previously supported nonnullable unique-key types are unchanged, not newly certified by this review.',
            'Actual zero-column direct-IR/native reachability, native serializer execution, duplicate bag semantics and sharing/staging remain pending.'
        ],
        'compiled': False, 'native_verified': False, 'production_edited': False,
        'generated_payloads_saved': False,
    }
    (ROOT / 'verification/C31-scalar-identity-independent-review.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('patch_sha256', 'source_contracts', 'bag_comparisons',
                                                  'zero_column_comparisons', 'empty_left_comparisons',
                                                  'naive_duplicate_rejoin_mutations_rejected')}))


if __name__ == '__main__':
    main()
