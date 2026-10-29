"""Sanitized abstract contracts for the general-frame MIN/MAX candidate.

Frames/data are generated in memory. This validates the interval/deque design
and capacity-accounting arithmetic, not the uncompiled C++ implementation or
the production RANGE comparator. No SQL or generated C++ is persisted.
"""
from collections import deque
import hashlib
import json
from pathlib import Path
import random

import q01_range_minmax_patch_generator as patch_generator


def _frames(length, seed):
    rng = random.Random(seed)
    left = right = 0
    for current in range(length):
        left = min(length, left + rng.randrange(0, 6))
        right = min(length, right + rng.randrange(0, 6))
        # Sufficient fetched rows and legitimate collector pruning. The
        # algorithm must tolerate gaps where the lower bound passes the upper.
        fetched = max(current + 1, left, right)
        base = min(current, left)
        yield fetched, base, left - base, max(0, right - base)


def _values(length, shape, seed):
    rng = random.Random(seed)
    if shape == 'null':
        return [None] * length
    if shape == 'equal':
        return [17] * length
    if shape == 'increasing':
        return list(range(length))
    if shape == 'decreasing':
        return list(reversed(range(length)))
    if shape == 'wide_integer':
        return [None if i % 7 == 0 else rng.choice((-(2**126), 2**126, -1, 0, 1)) for i in range(length)]
    return [None if rng.randrange(5) == 0 else rng.randrange(-50, 51) for _ in range(length)]


def _run_intervals(values, frames, minimum, mutation=None):
    candidates = deque()
    seen = 0
    pushes = front_pops = back_pops = comparisons = 0
    peak_candidates = 0
    matched = True
    checked = 0
    for fetched, base, relative_begin, relative_end in frames:
        begin, end = base + relative_begin, base + relative_end
        if mutation == 'ignore_queue_rebase':
            begin, end = relative_begin, relative_end
        entering = max(seen, begin)
        while candidates and candidates[0][0] < begin:
            candidates.popleft()
            front_pops += 1
        if begin >= end:
            front_pops += len(candidates)
            candidates.clear()
        else:
            incoming = range(entering, end)
            if mutation == 'last_boundary_only':
                incoming = range(max(entering, end - 1), end)
            for ordinal in incoming:
                value = values[ordinal]
                if value is None:
                    continue
                while candidates:
                    comparisons += 1
                    retained = candidates[-1][1] < value if minimum else candidates[-1][1] > value
                    if retained:
                        break
                    candidates.pop()
                    back_pops += 1
                candidates.append((ordinal, value))
                pushes += 1
        seen = max(seen, begin, end)
        actual = candidates[0][1] if candidates else None
        original_begin = base + relative_begin
        original_end = base + relative_end
        present = [value for value in values[original_begin:original_end] if value is not None]
        expected = (min(present) if minimum else max(present)) if present else None
        matched &= actual == expected
        checked += 1
        peak_candidates = max(peak_candidates, len(candidates))
        if mutation is None:
            assert fetched >= original_end and fetched >= original_begin
            assert len(candidates) <= max(0, original_end - original_begin)
    if mutation is None:
        assert matched
        assert pushes <= len(values)
        assert front_pops + back_pops <= pushes
        assert comparisons <= 2 * pushes
    return matched, {'checked_frames': checked, 'pushes': pushes,
                     'pops': front_pops + back_pops, 'comparisons': comparisons,
                     'peak_candidates': peak_candidates}


def _capacity_contracts():
    assertions = 0
    for peak in (1, 2, 3, 17, 128, 257, 1024):
        size = capacity = charged = 0
        growth_peak = 0
        while size < peak:
            if size == capacity:
                next_capacity = max(2, 2 * capacity)
                new_bytes = 16 * next_capacity
                old_bytes = 16 * capacity
                # Both allocations are charged before the public Reserve.
                pending = charged + new_bytes
                growth_peak = max(growth_peak, pending)
                assert pending == old_bytes + new_bytes
                # A throwing reserve returns exactly the tentative charge.
                assert pending - new_bytes == charged
                charged = pending - old_bytes
                capacity = next_capacity
                assert charged == 16 * capacity
                assertions += 3
            size += 1
        assert capacity < 2 * peak or peak == 1
        assert growth_peak <= 24 * capacity
        charged -= 16 * capacity
        assert charged == 0
        assertions += 3
    return assertions


def run_contracts(repository):
    files = patch_generator.render_files(repository)
    patch = patch_generator.render_patch(repository)
    assert all(path.startswith('ydb/core/kqp/') for path in files)
    total_frames = scenarios = mutations_rejected = 0
    counters = {'pushes': 0, 'pops': 0, 'comparisons': 0, 'peak_candidates': 0}
    for shape in ('null', 'equal', 'increasing', 'decreasing', 'wide_integer', 'mixed'):
        for length in (1, 7, 31, 257):
            for seed in range(12):
                values = _values(length, shape, seed)
                frames = list(_frames(length, seed + 100))
                for minimum in (True, False):
                    matched, result = _run_intervals(values, frames, minimum)
                    scenarios += 1
                    total_frames += result['checked_frames']
                    for key in ('pushes', 'pops', 'comparisons'):
                        counters[key] += result[key]
                    counters['peak_candidates'] = max(counters['peak_candidates'], result['peak_candidates'])
    # Peers and long overlapping frames exercise the linear work bound where
    # a complete-frame fold performs quadratically many visits.
    for length in (64, 128, 256, 512):
        values = _values(length, 'mixed', length)
        frames = [(length, 0, 0, length)] * length
        for minimum in (True, False):
            _, result = _run_intervals(values, frames, minimum)
            assert result['pushes'] <= length and result['comparisons'] <= 2 * length
            scenarios += 1
            total_frames += length
    suffix_scenarios = 0
    for length in (1, 7, 31, 257):
        for lower in (-97, -5, 0, 2, 97):
            values = _values(length, 'mixed', length + lower)
            frames = []
            for current in range(length):
                begin = min(length, max(0, current + lower))
                base = min(current, begin)
                frames.append((length, base, begin - base, length - base))
            for minimum in (True, False):
                _, result = _run_intervals(values, frames, minimum)
                assert result['pushes'] <= length and result['comparisons'] <= 2 * length
                suffix_scenarios += 1
                scenarios += 1
                total_frames += length
    for mutation in ('last_boundary_only', 'ignore_queue_rebase'):
        detected = False
        for seed in range(12):
            values = _values(257, 'mixed', seed)
            matched, _ = _run_intervals(values, list(_frames(257, seed + 100)), True, mutation)
            detected |= not matched
        assert detected
        mutations_rejected += 1
    return {
        'status': 'abstract_design_contracts_passed',
        'production_commit': patch_generator.SOURCE_COMMIT,
        'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
        'generator_sha256': hashlib.sha256(Path(patch_generator.__file__).read_bytes()).hexdigest(),
        'contract_generator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'production_files': len(files), 'all_changes_within_kqp': True,
        'interval_scenarios': scenarios, 'checked_frames': total_frames,
        'unbounded_following_scenarios': suffix_scenarios,
        'mutation_classes_rejected': mutations_rejected,
        'capacity_arithmetic_assertions': _capacity_contracts(),
        'model_work_counters': counters,
        'range_comparator_semantics_tested': False,
        'compiled_production_callable_tested': False,
        'native_allocations_or_operation_counts_measured': False,
        'compiler_or_native_processes_started': 0,
        'rendered_test_inputs_persisted': False,
        'limitations': [
            'Abstract monotone interval and deque contracts only; production collection and runtime have not executed.',
            'Memory tests verify proposed accounting arithmetic, not allocator integration or process RSS.',
            'Finite RANGE bounds may still retain a whole peer group; this is frame-size-dependent memory, not a fixed row cap.',
            'Mixed windows retain the existing second collector and can require additional following-bound lookahead.',
            'Unbounded-following frames require an initial partition drain; this does not introduce a partition drain for bounded frames.',
            'Excluded aggregate/value families retain their existing implementation; all admitted scalar MIN/MAX general frame shapes are covered by the design.'
        ],
    }


if __name__ == '__main__':
    print(json.dumps(run_contracts(Path(__file__).resolve().parents[1]), sort_keys=True))
