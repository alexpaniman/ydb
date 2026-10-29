"""Render the C47 shared plain-lookup amendment without editing source trees."""
import difflib
import subprocess

COMMIT = '92a3aebcaa4858c945c9a56e929b5d2d4a030b58'
_ROOT = 'ydb/core/kqp/opt/rbo/'


def render_files(repository):
    paths = (_ROOT + 'copy_logical_subtree.cpp', _ROOT + 'kqp_operator.h')
    originals = {path: subprocess.check_output(['git', 'show', COMMIT + ':' + path],
                                               cwd=repository, text=True) for path in paths}
    changed = dict(originals)
    path = paths[0]
    marker = 'bool IsRepeatable(const IOperator& op) {'
    helper = '''// Plain imported index reads have no ID-bound lookup program to clone.
// Their inputs still pass the recursive read/repeatability checks below.
bool CanCopyTableLookup(const TOpTableLookup& lookup) {
    return lookup.Strategy == ELookupStrategy::LookupRows && lookup.JoinKind.empty()
        && !lookup.FetchedRowFilter && !lookup.Prefix && !lookup.SourceRead
        && lookup.ResidualJoinKeys.Items().empty() && !lookup.KeysFromInputLookup;
}

'''
    assert changed[path].count(marker) == 1
    changed[path] = changed[path].replace(marker, helper + marker)
    marker = '        case EOperator::EmptySource:\n'
    branch = '''        case EOperator::TableLookup:
            return CanCopyTableLookup(CastOperator<TOpTableLookup>(op));
'''
    assert changed[path].count(marker) == 1
    changed[path] = changed[path].replace(marker, branch + marker)
    marker = 'TIntrusivePtr<IOperator> TOpMap::CopyImpl('
    method = '''TIntrusivePtr<IOperator> TOpTableLookup::CopyImpl(TInfoUnitRegistry& registry, TSubstitutions& renames) const {
    if (!CanCopyTableLookup(*this)) {
        return nullptr;
    }
    auto copy = MakeIntrusive<TOpTableLookup>(GetInput(), Pos, Table,
        CopyColumns(Columns, registry, renames), LookupKeys);
    // Copy() rebinds input IDs after every definition has been allocated.
    // Storage key names and their physical order remain unchanged.
    copy->AllowNullKeys = AllowNullKeys;
    return copy;
}

'''
    assert changed[path].count(marker) == 1
    changed[path] = changed[path].replace(marker, method + marker)
    path = paths[1]
    marker = '    std::optional<TSourceRead> SourceRead;\n\nprotected:\n'
    assert changed[path].count(marker) == 1
    changed[path] = changed[path].replace(marker, marker +
        '    TIntrusivePtr<IOperator> CopyImpl(TInfoUnitRegistry& registry, TSubstitutions& renames) const override;\n')
    return originals, changed


def render_patch(repository):
    originals, changed = render_files(repository)
    return ''.join(''.join(difflib.unified_diff(originals[path].splitlines(True),
                                             changed[path].splitlines(True),
                                             fromfile='a/' + path, tofile='b/' + path))
                   for path in sorted(originals))
