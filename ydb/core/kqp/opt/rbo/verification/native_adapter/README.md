# Python adapters for retained native structures

The native capture command's opt-in `--native-structure` preserves typed native
operator and expression graphs even when the semantic exporter rejects a query.
These files are diagnostic evidence, not semantic snapshots or proofs. They let
an adapter learn a new captured callable in Python without rebuilding or rerunning
the optimizer. Missing information still requires a producer change and recapture.

From the repository root:

```bash
export PYTHONPATH="$PWD/ydb/core/kqp/opt/rbo/verification"
python3 -m native_adapter inspect /persistent/capture/native-structure-0.json
python3 -m native_adapter lower /persistent/capture/native-structure-0.json \
  --output /persistent/converted/initial.json
```

`lower` writes a new semantic snapshot and then
`initial.json.provenance.json` as the completion marker. It never overwrites
existing files. The receipt pins the original archive, output bytes and adapter
source files; retain it alongside the original capture's revision/binary receipt.
Adaptation does not establish that an input came from a native binary.
The module entry point uses a fresh empty bytecode-cache location and disables
bytecode writes before importing the adapter/kernel, so the receipt describes
the current source rather than a stale local `.pyc`. Direct library imports use
ordinary Python import behavior; provenance-sensitive callers must likewise
use a clean interpreter/cache.

The initial adapter accepts only complete logical Read/Filter/Map/Root graphs
over Boolean and fixed-width integer values. Read ranges/pushdowns, assigned
physical properties, stages, subplans, effects and unsupported scalar/operator
shapes are rejected. Native Map appends fresh definitions to the input row. Root
projection preserves every result position and client label. Captured type
annotations, row binding identities, duplicate IDs, unknown fields, graph cycles
and unsafe scalar metadata are checked. No unknown function becomes an opaque
function automatically.

Lowered logical snapshots are only for `OPTIMIZER_TRANSFORMATION_PAIR`
diagnostics, with the original logical query as the observation anchor:

```bash
python3 -m rbo_verifier initial.json candidate.json \
  --diagnostic-transformation-pair \
  --diagnostic-observation-snapshot initial.json \
  --rows 2 --solver /existing/z3 \
  --timeout-ms 10000 --construction-timeout-ms 120000
```

The ordinary endpoint verifier rejects these logical-only final snapshots. An
adjacent transformation proof is not a proof of the full optimizer pipeline.
Existing fully exported semantic endpoints continue to use the offline corpus
runner directly, including physical StageGraph semantics.

## Adding an API in Python

`lower.py` separates structural decoding from reviewed scalar handlers. A handler
receives typed, already-lowered operands and returns a typed semantic expression.
The adapter checks its result against the captured native annotation, then the
existing semantic IR validator checks the complete plan. Handlers are part of
the trusted semantic implementation and require independent semantic tests.

The `bool_xor` handler is a small example: two nonnullable Bool operands lower to
existing Boolean operations. The fixture test first removes `Xor` from the
registry and observes rejection, then adds the handler and lowers the identical
saved graph. Optional operands remain rejected. No native export rule changes.
The API accepts an explicit handler mapping for experiments; the CLI uses only
the checked-in, source-pinned default mapping and never loads executable code
from an archive.

```bash
RBO_Z3=/existing/z3 python3 -m unittest discover \
  -s ydb/core/kqp/opt/rbo/verification/native_adapter/tests
```

Tests use explicitly synthetic native structures. With an existing solver, they
also prove an identity and detect a dropped-filter mutation through the actual
SMT kernel. These tests do not replace native producer/adaptor conformance tests
or fresh captures from the compiled exporter.
The new native structural producer still requires its first guarded build and
capture/conformance gate before attributing these adaptations to production
optimizer executions.
