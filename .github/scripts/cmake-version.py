#!/usr/bin/env python3
"""Read or rewrite the ``VERSION_*`` block in wire-sysio's root ``CMakeLists.txt``.

The release workflows are the only consumers: ``prepare-release.yaml`` writes the
requested version into the tree, and ``tag-release.yaml`` reads it back to assert
that master's HEAD really carries the version being tagged. Both go through this
one parser so the two halves of that assertion can never drift apart.

The version string is ``<major>.<minor>.<patch>[-<suffix>]``, matching the
``VERSION_FULL`` composition in ``CMakeLists.txt``. The suffix carries the release
channel: any suffix (``dev``, ``rc1``, ...) means prerelease, no suffix means
stable.

Usage::

    cmake-version.py read [--cmakelists PATH]
    cmake-version.py write <version> [--cmakelists PATH]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

#: ``<major>.<minor>.<patch>`` with an optional ``-<suffix>`` of dot-separated
#: alphanumerics -- deliberately conservative so a typo cannot reach the tree.
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$")

DEFAULT_CMAKELISTS = Path(__file__).resolve().parents[2] / "CMakeLists.txt"


def _field_re(name: str) -> re.Pattern[str]:
    """Build the anchored matcher for one ``set(<name> <value>)`` line.

    The trailing run is ``[ \\t]*`` rather than ``\\s*`` on purpose: ``\\s``
    matches newlines, so with :data:`re.MULTILINE` the pattern would extend past
    the closing paren and swallow the blank line that follows it -- ``write``
    would then quietly delete one blank line per field on every release
    preparation.
    """
    return re.compile(rf"^set\({name}\s+(.*)\)[ \t]*$", re.MULTILINE)


def read_version(text: str) -> str:
    """Return the ``VERSION_FULL`` string composed from the ``set(VERSION_*)`` lines."""
    parts = {}
    for name in ("VERSION_MAJOR", "VERSION_MINOR", "VERSION_PATCH", "VERSION_SUFFIX"):
        match = _field_re(name).search(text)
        if match is None:
            if name == "VERSION_SUFFIX":
                parts[name] = ""
                continue
            raise SystemExit(f"cmake-version: {name} not found in CMakeLists.txt")
        parts[name] = match.group(1).strip().strip('"')

    version = f"{parts['VERSION_MAJOR']}.{parts['VERSION_MINOR']}.{parts['VERSION_PATCH']}"
    if parts["VERSION_SUFFIX"]:
        version = f"{version}-{parts['VERSION_SUFFIX']}"
    return version


def write_version(text: str, version: str) -> str:
    """Return ``text`` with the ``set(VERSION_*)`` lines rewritten to ``version``."""
    match = VERSION_RE.match(version)
    if match is None:
        raise SystemExit(
            f"cmake-version: '{version}' is not <major>.<minor>.<patch>[-<suffix>]"
        )
    major, minor, patch, suffix = match.groups()

    # An empty suffix is written as `""` rather than as a bare empty token so the
    # line stays valid CMake and `if(VERSION_SUFFIX)` still evaluates false.
    replacements = {
        "VERSION_MAJOR": major,
        "VERSION_MINOR": minor,
        "VERSION_PATCH": patch,
        "VERSION_SUFFIX": suffix if suffix else '""',
    }
    for name, value in replacements.items():
        text, count = _field_re(name).subn(f"set({name} {value})", text, count=1)
        if count != 1:
            raise SystemExit(f"cmake-version: could not rewrite set({name} ...)")
    return text


def main() -> int:
    """Parse arguments and dispatch to the read or write mode."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("read", "write"))
    parser.add_argument("version", nargs="?", help="version to write (write mode only)")
    parser.add_argument("--cmakelists", type=Path, default=DEFAULT_CMAKELISTS)
    args = parser.parse_args()

    text = args.cmakelists.read_text()

    if args.mode == "read":
        print(read_version(text))
        return 0

    if not args.version:
        raise SystemExit("cmake-version: write mode requires a version argument")
    args.cmakelists.write_text(write_version(text, args.version))
    print(read_version(args.cmakelists.read_text()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
