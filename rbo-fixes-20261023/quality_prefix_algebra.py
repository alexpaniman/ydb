"""Independent integer-window algebra checks; return sanitized counters only.

This checks the mathematical precondition for Q01, not the production AST,
queue implementation, compiler, or native execution. Inputs never leave RAM.
"""
import hashlib
import random


def check_integer_prefix_algebra(seed=751239, samples=24):
    rng = random.Random(seed)
    checks = empty_frames = all_null_frames = wrapped_prefixes = 0
    digest = hashlib.sha256()
    modulus = 1 << 64
    for width in (8, 16, 32, 64):
        for signed in (False, True):
            low, high = (-(1 << (width - 1)), (1 << (width - 1)) - 1) if signed else (0, (1 << width) - 1)
            for optional in (False, True):
                for sample in range(samples):
                    length = sample % 13
                    population = [low, high, 0, min(1, high), max(-1, low)]
                    if optional:
                        population += [None, None]
                    values = [rng.choice(population) for _ in range(length)]
                    if optional and sample % 7 == 0:
                        values = [None] * length
                    prefix_sum, prefix_count = [], []
                    total = count = 0
                    for value in values:
                        if value is not None:
                            total += value
                            count += 1
                            if not 0 <= total < modulus:
                                wrapped_prefixes += 1
                            total %= modulus
                        prefix_sum.append(total)
                        prefix_count.append(count)
                    for begin in range(length + 1):
                        for end in range(begin, length + 1):
                            frame = [value for value in values[begin:end] if value is not None]
                            # Direct ordered fold is independent of prefix cancellation.
                            folded = None
                            for value in frame:
                                folded = (value if folded is None else folded + value) % modulus
                            if begin == end:
                                derived = None
                                empty_frames += 1
                            else:
                                nonnull = prefix_count[end - 1] - prefix_count[begin] + (values[begin] is not None)
                                derived = ((prefix_sum[end - 1] - prefix_sum[begin] + (values[begin] or 0)) % modulus
                                           if nonnull else None)
                                all_null_frames += not nonnull
                            if derived != folded:
                                raise AssertionError('Integer prefix algebra contract failed')
                            if signed and folded is not None and folded >= 1 << 63:
                                folded -= modulus
                            checks += 1
                            digest.update(str((folded, len(frame))).encode())
    return {'status': 'algebra_contract_passed_native_pending', 'checks': checks,
            'empty_frames': empty_frames, 'all_null_frames': all_null_frames,
            'wrapped_prefix_updates': wrapped_prefixes, 'result_digest': digest.hexdigest(),
            'input_values_persisted': False, 'native_execution': False,
            'does_not_verify': ['production AST typing', 'queue endpoint lifetime', 'runtime frame bounds',
                                'lowering reachability', 'optimization work reduction']}
