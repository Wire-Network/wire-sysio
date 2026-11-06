#!/usr/bin/env python3
"""Register exactly the given ports in the bind registry until stdin closes: ``bind_registry_register.py <port>...``.

The external-client entry point for wire-tools-ts' interop suite (its README section 8):
``WIRE_BIND_REGISTRY_PYTHON_CLIENT=<this file>``. It runs :func:`bind_registry.register` in
this process, so this process owns the registration file, as the suite requires.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bind_registry  # noqa: E402

if __name__ == "__main__":
    sys.exit(bind_registry.register(sys.argv[1:]))
