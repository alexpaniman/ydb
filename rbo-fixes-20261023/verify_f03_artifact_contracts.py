"""Verify only F03 source and abstract contracts, with no engine execution."""
import hashlib
import json
from pathlib import Path
import subprocess

import interval_window_checks as checks
import prepare_f03_interval_window_patch as patch

ROOT = Path(__file__).resolve().parent


def main():
    files = patch.render_files()
    before, after = files[patch.SOURCE]
    contract_checks = [
        len(files) == 1 and '/opt/rbo/physical_conversion/' in patch.SOURCE,
        after.count('GetIntervalType(') == 3,
        'IsDataTypeInterval(data->GetSlot()) ? data : nullptr' in after,
        'type->GetKind() == ETypeAnnotationKind::Optional' in after.split('bool IsDecimal')[0],
        after.count('Ctx.NewAtom(Pos, "Decimal"), Ctx.NewAtom(Pos, "35"), Ctx.NewAtom(Pos, "0")') == 1,
        'auto casted = MakeOptional(std::move(sumInput), isOptional);' in after,
        'sumInput = Ctx.NewCallable(Pos, "SafeCast", {value, BuildSumCastTarget(argument)});' in after,
        'ExpandType(Pos, *intervalType, Ctx)' in after,
        after.count('"AggrAdd"') == before.count('"AggrAdd"'),
        after.count('"Fold1"') == before.count('"Fold1"'),
        'stateMembers.emplace_back(AccumulatorName(f), accumulator);' in after,
        'BuildResultFromAccumulator(funcs[f], accumulator)' in after,
    ]
    assert all(contract_checks)
    counts = checks.artifact_contracts()
    sources = {}
    for name in ['yql/essentials/mount/lib/yql/aggregate.yqls',
                 'yql/essentials/core/yql_expr_type_annotation.cpp',
                 'yql/essentials/core/type_ann/type_ann_core.cpp',
                 'yql/essentials/public/udf/udf_data_type.cpp',
                 'yql/essentials/public/udf/udf_data_type.h']:
        content = subprocess.check_output(['git', 'show', patch.BASE + ':' + name], cwd=ROOT.parent)
        sources[name] = hashlib.sha256(content).hexdigest()
    report = dict(finding='F03', base_revision=patch.BASE,
                  patch_sha256=hashlib.sha256((ROOT/'proposed-correctness-amendments/F03-interval-window-sum.patch').read_bytes()).hexdigest(),
                  generator_sha256=hashlib.sha256((ROOT/'interval_window_checks.py').read_bytes()).hexdigest(),
                  case_count=len(checks.specs()), source_contracts_passed=len(contract_checks),
                  abstract_contracts=counts, source_reference_sha256=sources,
                  frame_families=['finite ROWS', 'running ROWS', 'suffix ROWS', 'incremental ROWS',
                                  'finite RANGE', 'running RANGE', 'suffix RANGE', 'incremental RANGE', 'composite peers'],
                  data_contracts=['Interval and Interval64', 'required and optional inputs', 'all-null and empty input',
                                  'final positive and negative range overflow', 'wide partial cancellation',
                                  'range edge', 'empty prefix and tail frames', 'ascending and descending order',
                                  'partition separation', 'Int64 and Decimal controls'],
                  cast_option_proof=['Required Interval or Interval64 to Int64 is complete.',
                                     'Required Int64 to Decimal(35,0) is complete, so Just does not create a nested Optional.',
                                     'Optional input casts remain one Optional layer.',
                                     'Decimal to Int64 may fail; the final Interval cast remains Optional.',
                                     'GetSumResultType declares interval SUM output Optional, even for required input.'],
                  limitations=['No compiler or native execution; actual lowering and baseline failure still require guarded confirmation.',
                               'Whole-partition windows rewritten to logical aggregation require independent F02.',
                               'These source/model contracts do not establish optimization quality or memory bounds.'],
                  compile_executed=False,native_executed=False,production_sources_edited=False,
                  generated_queries_saved=False,generated_rows_saved=False)
    out=ROOT/'verification/F03-interval-window-artifact-contracts.json'
    out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(report_sha256=hashlib.sha256(out.read_bytes()).hexdigest(),case_count=report['case_count'],
                          generator_sha256=report['generator_sha256'],source_contracts_passed=len(contract_checks),
                          abstract_contracts=counts)))


if __name__ == '__main__':
    main()
