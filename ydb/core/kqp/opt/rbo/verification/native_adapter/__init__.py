"""Reviewed logical adapters, loaded lazily for clean-source CLI execution."""

import importlib

__all__ = ["AdapterError", "NativeArchive", "load_archive", "lower"]


def __getattr__(name):
    if name not in __all__:
        raise AttributeError(name)
    module = importlib.import_module(".lower" if name == "lower" else ".archive", __name__)
    value = getattr(module, name)
    globals()[name] = value
    return value
