"""Independent sorted-boundary oracle checks; generated rows remain in memory."""
import hashlib
import json
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))

import quality_range_minmax_checks as checks


def _slice_members(current, partition, spec):
    """Locate frame endpoints in sorted order without the production membership predicate."""
    ordered = sorted(partition, key=lambda row: checks._sortkey(row, spec))
    control = spec.get('control')
    if control == 'whole':
        return ordered
    peer_indices = [index for index, row in enumerate(ordered) if row['o'] == current['o']]
    if control == 'running':
        return ordered[:peer_indices[-1] + 1]
    if control == 'suffix':
        return ordered[peer_indices[0]:]
    lower, upper = checks.FRAMES[spec['frame']]
    if current['o'] is None:
        begin = peer_indices[0]
        end = len(ordered) if upper is None else peer_indices[-1] + 1
        return ordered[begin:end]
    # NULL groups are separate sorting domains. A finite bound reaches only
    # the numeric domain; an infinite upper bound includes the later domain.
    numeric = [(index, row['o'] if spec['direction'] == 'asc' else -row['o'])
               for index, row in enumerate(ordered) if row['o'] is not None]
    current_key = current['o'] if spec['direction'] == 'asc' else -current['o']
    begin = next((index for index, key in numeric if key >= current_key + lower), numeric[-1][0] + 1)
    end = (len(ordered) if upper is None else
           next((index for index, key in numeric if key > current_key + upper), numeric[-1][0] + 1))
    return ordered[begin:end]


def verify():
    rng = random.Random(291144)
    assertions = nullable_suffix_assertions = 0
    for spec in checks._specifications().values():
        for size in (8, 24, 59):
            rows = checks._rows(spec, size)
            for current in rows:
                partition = [row for row in rows if row['p'] == current['p']]
                expected = {row['id'] for row in _slice_members(current, partition, spec)}
                actual = {row['id'] for row in checks._members(current, partition, spec)}
                assert actual == expected
                assertions += 1
                nullable_suffix_assertions += (spec['key'].startswith('nullable') and
                    checks.FRAMES[spec['frame']][1] is None)
    # Independently generated peer domains include empty numeric domains,
    # repeated NULL peers and numeric gaps beyond both finite endpoints.
    for direction in ('asc', 'desc'):
        for null_order in ('nullable_first', 'nullable_last'):
            for frame in checks.FRAMES:
                spec = dict(direction=direction, key=null_order, frame=frame)
                for _ in range(25):
                    rows = [{'id': index, 'o': None if rng.randrange(3) == 0 else rng.randrange(-40, 41)}
                            for index in range(rng.randrange(1, 35))]
                    for current in rows:
                        expected = {row['id'] for row in _slice_members(current, rows, spec)}
                        actual = {row['id'] for row in checks._members(current, rows, spec)}
                        assert actual == expected
                        assertions += 1
                        nullable_suffix_assertions += checks.FRAMES[frame][1] is None
    source = Path(checks.__file__)
    return dict(status='abstract_oracle_contracts_passed', assertions=assertions,
                nullable_unbounded_assertions=nullable_suffix_assertions,
                generator_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                native_collector_executed=False, generated_inputs_saved=False,
                sql_parser_executed=False)


if __name__ == '__main__':
    print(json.dumps(verify(), sort_keys=True))
