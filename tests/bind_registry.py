#!/usr/bin/env python3
"""CTest entry point for the bind-registry client in ``TestHarness/bind_registry.py``.

``python3 tests/bind_registry.py <test command> [args...]`` runs a test under a port lease (see the
client's docstring). Imported as ``bind_registry``, this module is the client itself: the client file
is loaded once, without importing the rest of TestHarness, and registered as
``TestHarness.bind_registry`` so TestHarness and its callers share one instance.
"""
import importlib.util
import os
import sys

_CLIENT_NAME = "TestHarness.bind_registry"
_CLIENT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "TestHarness", "bind_registry.py")


def _load_client():
    """Return the client module, loading it from ``TestHarness/bind_registry.py`` on first use."""
    client = sys.modules.get(_CLIENT_NAME)
    if client is None:
        spec = importlib.util.spec_from_file_location(_CLIENT_NAME, _CLIENT_PATH)
        client = importlib.util.module_from_spec(spec)
        sys.modules[_CLIENT_NAME] = client
        try:
            spec.loader.exec_module(client)
        except BaseException:
            sys.modules.pop(_CLIENT_NAME, None)  # As the import system does: no half-initialized module stays.
            raise
    return client


if __name__ == "__main__":
    sys.exit(_load_client().main(sys.argv[1:]))
else:
    sys.modules[__name__] = _load_client()
