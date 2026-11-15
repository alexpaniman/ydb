import sys
import tempfile

# -B disables cache writes, not reads. Start local imports with a fresh cache
# namespace so receipts describe source even after rapid same-size edits.
with tempfile.TemporaryDirectory(prefix="rbo-offline-imports-") as cache:
    sys.dont_write_bytecode = True
    sys.pycache_prefix = cache
    from .cli import main

    raise SystemExit(main())
