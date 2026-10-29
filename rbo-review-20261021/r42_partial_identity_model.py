"""Generate abstract identity projections; retain only check counts, never cases."""
from collections import Counter
from itertools import product
import json

counts = {kind: 0 for kind in ("left", "semi", "anti")}
checks = 0
for width in range(2, 7):
    for groups in product(range(2), repeat=width):
        for matches in product(range(2), repeat=width):
            complete = Counter(i for i, found in enumerate(matches) if found)
            projected = Counter(groups[i] for i, found in enumerate(matches) if found)
            for kind in counts:
                expected = []
                actual = []
                for i in range(width):
                    true_count, projected_count = complete[i], projected[groups[i]]
                    if kind == "left":
                        expected.append(max(1, true_count))
                        actual.append(max(1, projected_count))
                    elif kind == "semi":
                        expected.append(bool(true_count))
                        actual.append(bool(projected_count))
                    else:
                        expected.append(not true_count)
                        actual.append(not projected_count)
                counts[kind] += expected != actual
            checks += 1
print(json.dumps({"generated_states": checks, "mismatch_counts": counts}))
