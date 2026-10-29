"""Anonymous model checks for a proposed window MIN/MAX two-stack mechanism.

This is a generator and model oracle, not production execution or a proposed
complete Q01 fix. Concrete values and frame intervals stay in caller memory.
The source audit records the unresolved RANGE entering-row access boundary.
"""
import itertools
import math
import struct


def _bits(value):
    return None if value is None else struct.pack('>d', value)


def _atoms():
    finite = [float(value) for value in range(-2, 3)]
    signed_zeros = [math.copysign(0.0, sign) for sign in (-1.0, 1.0)]
    nonfinite = [math.copysign(math.inf, sign) for sign in (-1.0, 1.0)]
    payloads = [struct.unpack('>d', struct.pack('>Q', (0x7ff8 << 48) | value))[0]
                for value in range(1, 4)]
    return [None, *finite, *signed_zeros, *nonfinite, *payloads]


def combine(left, right, operation, backend):
    """Model the inspected aggregate selectors, including backend NaN ties."""
    if left is None:
        return right
    if right is None:
        return left
    if operation == 'min':
        choose_left = left < right or math.isnan(right)
        if backend == 'codegen':
            choose_left = choose_left and not math.isnan(left)
    else:
        choose_left = left > right or math.isnan(left)
        if backend == 'codegen':
            choose_left = choose_left and not math.isnan(right)
    return left if choose_left else right


def _fold(values, operation, backend):
    state = None
    for value in values:
        state = combine(state, value, operation, backend)
    return state


def _frames(count, variant):
    """Yield only monotone half-open intervals; do not persist the output."""
    if variant.startswith('rows_'):
        left, right = map(int, variant.split('_')[1:])
        for index in range(count):
            yield max(0, min(count, index + left)), max(0, min(count, index + right + 1))
        return
    start = end = 0
    for index in range(count):
        # Irregular advancing bounds include repeated peers, empty intervals,
        # disjoint windows and skipped input ordinals.
        step = (index * 7 + len(variant)) % 5
        if variant == 'batches':
            end = min(count, end + step)
            start = min(end, max(start, end - 1 - (index % 7)))
        elif variant == 'gaps':
            start = min(count, start + step)
            end = min(count, max(end, start + (index % 3)))
        else:
            end = min(count, end + (index % 2))
            start = min(end, max(start, end - (index % 4)))
        yield start, end


def run_model_checks():
    atoms = _atoms()
    checks = 0
    associations = 0
    max_transfer_factor = 0
    for operation, backend in itertools.product(('min', 'max'), ('interpreter', 'codegen')):
        for first, second, third in itertools.product(atoms, repeat=3):
            lhs = combine(combine(first, second, operation, backend), third, operation, backend)
            rhs = combine(first, combine(second, third, operation, backend), operation, backend)
            assert _bits(lhs) == _bits(rhs)
            associations += 1
        variants = [*(f'rows_{left}_{right}' for left in range(-7, 8, 2)
                      for right in range(left, 8, 3)), 'batches', 'gaps', 'empty']
        for count, variant in itertools.product((1, 9, 31, 83), variants):
            values = [atoms[(index * 11 + count) % len(atoms)] for index in range(count)]
            # Python list tail represents stack head. Production uses Prepend
            # for rear/transfer construction and Skip only for front expiry.
            front, rear = [], []
            last_seen = 0
            appended = transferred = popped = 0
            for begin, end in _frames(count, variant):
                if begin >= end:
                    front.clear()
                    rear.clear()
                    last_seen = max(last_seen, end)
                    actual = None
                else:
                    while True:
                        while front and front[-1][0] < begin:
                            front.pop()
                            popped += 1
                        if front or not rear:
                            break
                        while rear:
                            ordinal, value, _ = rear.pop()
                            summary = combine(value, front[-1][2] if front else None, operation, backend)
                            front.append((ordinal, value, summary))
                            transferred += 1
                    for ordinal in range(max(begin, last_seen), end):
                        value = values[ordinal]
                        summary = combine(rear[-1][2] if rear else None, value, operation, backend)
                        rear.append((ordinal, value, summary))
                        appended += 1
                    last_seen = max(last_seen, end)
                    actual = combine(front[-1][2] if front else None,
                                     rear[-1][2] if rear else None, operation, backend)
                expected = _fold(values[begin:end], operation, backend)
                assert _bits(actual) == _bits(expected)
                checks += 1
            assert appended <= count and transferred <= appended and popped <= transferred
            max_transfer_factor = max(max_transfer_factor, transferred / max(1, appended))
    return {'model_result_checks': checks, 'model_associativity_checks': associations,
            'max_transfers_per_appended_item': max_transfer_factor,
            'status': 'model_only_not_native', 'complete_minmax_fix': False,
            'range_entering_rows_api_resolved': False}
