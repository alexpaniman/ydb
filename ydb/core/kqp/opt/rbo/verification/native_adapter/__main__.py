"""Compile adapter/kernel source directly; never reuse stale local bytecode."""

import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="rbo-native-adapter-pycache-") as cache:
    sys.dont_write_bytecode = True
    sys.pycache_prefix = cache
    from .cli import main

    raise SystemExit(main())
