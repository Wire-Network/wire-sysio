#!/usr/bin/env python3
"""Read or rewrite the ``VERSION_*`` block in wire-sysio's root ``CMakeLists.txt``.

The release workflows are the only consumers: ``prepare-release.yaml`` writes the
requested version into the tree, ``tag-release.yaml`` reads it back to assert that
master's HEAD really carries the version being tagged, and ``release.yaml``
validates the tag it was handed. All three go through this one parser so those
assertions can never drift apart.

The version string is ``<major>.<minor>.<patch>[-<suffix>]``, matching the
``VERSION_FULL`` composition in ``CMakeLists.txt``. The suffix carries the release
channel: any suffix (``dev``, ``rc1``, ...) means prerelease, no suffix means
stable.

``validate`` exposes that same grammar as a standalone check so the workflows
never re-spell it as an inline bash regex: every version/tag shape assertion in
prepare-release.yaml, tag-release.yaml and release.yaml routes through this one
implementation of :data:`VERSION_RE`.

Usage::

    cmake-version.py read [--cmakelists PATH]
    cmake-version.py write <version> [--cmakelists PATH]
    cmake-version.py validate <version>
    cmake-version.py validate --tag <vversion>
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


def validate_version(version: str, tag: bool = False) -> str:
    """Return the BARE version after asserting ``version`` matches the grammar.

    ``tag`` mode is the release-tag spelling: the leading ``v`` every tag carries
    is required, then stripped before the check, so :data:`VERSION_RE` stays the
    ONE definition of the grammar for both the bare and the tagged spelling. The
    bare version is returned (and printed by ``main``) so a caller can validate
    and destructure a tag in one call instead of re-deriving ``${TAG#v}``.
    """
    bare = version
    if tag:
        if not version.startswith("v"):
            raise SystemExit(
                f"cmake-version: tag '{version}' must start with 'v'"
            )
        bare = version[1:]
    if VERSION_RE.match(bare) is None:
        prefix = "v" if tag else ""
        raise SystemExit(
            f"cmake-version: '{version}' is not "
            f"{prefix}<major>.<minor>.<patch>[-<suffix>]"
        )
    return bare


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
    parser.add_argument("mode", choices=("read", "write", "validate"))
    parser.add_argument(
        "version", nargs="?", help="version to write or validate"
    )
    # `--tag` carries its VALUE rather than flagging the trailing positional:
    # argparse cannot fill an `nargs="?"` positional that follows an optional on
    # the command line, so `validate --tag v1.0.0` would fail as an unrecognized
    # argument in the flag form.
    parser.add_argument(
        "--tag",
        metavar="TAG",
        help="validate mode: the vX.Y.Z[-suffix] tag to check; prints its bare version",
    )
    parser.add_argument("--cmakelists", type=Path, default=DEFAULT_CMAKELISTS)
    args = parser.parse_args()

    # `validate` is a pure grammar check on its ARGUMENT, so it deliberately runs
    # before the file is touched: a caller validating a dispatch input must not
    # also depend on a readable CMakeLists.txt.
    if args.mode == "validate":
        if args.tag:
            # The tag form is consumed as `version=$(... validate --tag "$TAG")`,
            # so it prints the bare version; the plain form is a pure exit-status
            # check and stays silent on success.
            print(validate_version(args.tag, tag=True))
            return 0
        if not args.version:
            raise SystemExit(
                "cmake-version: validate mode requires a version argument or --tag"
            )
        validate_version(args.version)
        return 0

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
