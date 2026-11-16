#!/usr/bin/env python3
"""Read or rewrite the ``VERSION_*`` block in wire-sysio's root ``CMakeLists.txt``.

The release workflows are the only consumers: ``prepare-release.yaml`` writes the
requested version into the tree, ``tag-release.yaml`` reads it back to assert that
the target branch's HEAD really carries the version being tagged, and ``release.yaml``
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

``validate --branch`` adds the branch rule prepare-release.yaml and
tag-release.yaml enforce: ``master`` carries any version, and a
``release/<major>.<minor>`` branch carries only its own line's versions.
``release-branch`` prints the branch of a version's line, so those workflows can
also refuse master for a line whose release branch already exists.

``check-pr`` is the pull-request check: only the bump PR prepare-release.yaml
opens from ``prep-branch``'s branch changes the base branch's version. Every
other PR, a forward merge from an older release branch included, leaves it as
it is.

``highest-tag`` reads release tags on stdin and prints the highest one in the
version's ``<major>.<minor>`` line or an older one. wire-sysio's tag build picks
its wire-cdt release with it, so a patch on an older line keeps that line's CDT.

Usage::

    cmake-version.py read [--cmakelists PATH]
    cmake-version.py write <version> [--cmakelists PATH]
    cmake-version.py validate <version> [--branch BRANCH]
    cmake-version.py validate --tag <vversion>
    cmake-version.py release-branch <version>
    cmake-version.py prep-branch <version>
    cmake-version.py check-pr --base-branch BRANCH --head-branch BRANCH
                              --base-cmakelists PATH [--cmakelists PATH]
    cmake-version.py highest-tag <version> < tags
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Iterable
from pathlib import Path

#: ``<major>.<minor>.<patch>`` with an optional ``-<suffix>`` of dot-separated
#: alphanumerics -- deliberately conservative so a typo cannot reach the tree.
#: Anchored with ``\Z`` rather than ``$``, which would also accept a trailing newline.
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?\Z")

DEFAULT_CMAKELISTS = Path(__file__).resolve().parents[2] / "CMakeLists.txt"

#: The development branch. Any version may be prepared and tagged on it.
DEVELOPMENT_BRANCH = "master"

#: Prefix of a release branch, ``release/<major>.<minor>``.
RELEASE_BRANCH_PREFIX = "release/"

#: Prefix of the branch prepare-release.yaml opens a bump PR from,
#: ``release/prep-v<version>``.
PREP_BRANCH_PREFIX = f"{RELEASE_BRANCH_PREFIX}prep-v"

#: Splits a version suffix into its digit and non-digit runs, so ``rc10`` sorts
#: after ``rc2``.
SUFFIX_RUN_RE = re.compile(r"(\d+)")


def _field_re(name: str) -> re.Pattern[str]:
    """Build the anchored matcher for one ``set(<name> <value>)`` line.

    The trailing run is ``[ \\t]*`` rather than ``\\s*`` on purpose: ``\\s``
    matches newlines, so with :data:`re.MULTILINE` the pattern would extend past
    the closing paren and swallow the blank line that follows it -- ``write``
    would then quietly delete one blank line per field on every release
    preparation.
    """
    return re.compile(rf"^set\({name}\s+(.*)\)[ \t]*$", re.MULTILINE)


def _single_field(text: str, name: str) -> re.Match[str] | None:
    """Return the one ``set(<name> ...)`` line in ``text``, or ``None`` if there is none.

    More than one is refused: a merge that left conflict markers in the version
    block carries both sides' lines, and reading only the first would report one
    side's version as the tree's.
    """
    matches = list(_field_re(name).finditer(text))
    if len(matches) > 1:
        raise SystemExit(
            f"cmake-version: found {len(matches)} set({name} ...) lines, expected one"
        )
    return matches[0] if matches else None


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


def version_key(version: str) -> tuple[object, ...]:
    """Return the key that orders versions the way they ship.

    ``version`` must already have passed :func:`validate_version`. Major, minor
    and patch compare as numbers, a prerelease sorts before the stable version it
    leads to, and suffixes compare run by run with numbers compared numerically.
    """
    major, minor, patch, suffix = VERSION_RE.match(version).groups()
    runs = tuple(
        (0, int(run)) if run.isdigit() else (1, run)
        for run in SUFFIX_RUN_RE.split(suffix or "")
        if run
    )
    return (int(major), int(minor), int(patch), 0 if suffix else 1, runs)


def highest_tag(tags: Iterable[str], version: str) -> str | None:
    """Return the highest tag in ``tags`` whose ``<major>.<minor>`` line is ``version``'s or older.

    ``version`` must already have passed :func:`validate_version`. Tags outside
    the ``v<major>.<minor>.<patch>[-<suffix>]`` grammar are skipped. Returns
    ``None`` when no tag qualifies.
    """
    line = version_key(version)[:2]
    best: tuple[tuple[object, ...], str] | None = None
    for tag in tags:
        try:
            key = version_key(validate_version(tag, tag=True))
        except SystemExit:
            continue
        if key[:2] <= line and (best is None or key > best[0]):
            best = (key, tag)
    return best[1] if best else None


def release_branch(version: str) -> str:
    """Return the ``release/<major>.<minor>`` branch of ``version``'s line.

    ``version`` must already have passed :func:`validate_version`.
    """
    major, minor = VERSION_RE.match(version).group(1, 2)
    return f"{RELEASE_BRANCH_PREFIX}{major}.{minor}"


def validate_branch(branch: str, version: str) -> None:
    """Assert ``version`` may be prepared and tagged on ``branch``.

    :data:`DEVELOPMENT_BRANCH` takes any version. A release branch takes only its
    own line, so ``1.2.0`` can never be cut from ``release/1.1``. Any other
    branch is refused.
    """
    if branch == DEVELOPMENT_BRANCH:
        return
    expected = release_branch(version)
    if branch != expected:
        raise SystemExit(
            f"cmake-version: version '{version}' is prepared and tagged on "
            f"'{DEVELOPMENT_BRANCH}' or '{expected}', not '{branch}'"
        )


def prep_branch(version: str) -> str:
    """Return the ``release/prep-v<version>`` branch of ``version``'s bump PR."""
    return f"{PREP_BRANCH_PREFIX}{version}"


def check_pr_version(
    base_branch: str, head_branch: str, base_version: str, merged_version: str
) -> None:
    """Assert a PR from ``head_branch`` into ``base_branch`` changes the version only if it is a bump PR.

    ``base_version`` is the base branch's version and ``merged_version`` the PR's
    merge result's. A changed version passes only from the merged version's
    :func:`prep_branch`, and only onto a branch :func:`validate_branch` lets carry
    it. A forward merge is refused when it carries the source branch's version,
    which git merges without a conflict when the two sides changed different
    ``VERSION_*`` lines.
    """
    if merged_version == base_version:
        return
    if head_branch == prep_branch(merged_version):
        validate_branch(base_branch, validate_version(merged_version))
        return
    raise SystemExit(
        f"cmake-version: this PR changes {base_branch}'s version from {base_version} "
        f"to {merged_version}. Only a prepare-release bump PR changes it; keep "
        f"{base_branch}'s set(VERSION_*) lines."
    )


def read_version(text: str) -> str:
    """Return the ``VERSION_FULL`` string composed from the ``set(VERSION_*)`` lines."""
    parts = {}
    for name in ("VERSION_MAJOR", "VERSION_MINOR", "VERSION_PATCH", "VERSION_SUFFIX"):
        match = _single_field(text, name)
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
        if _single_field(text, name) is None:
            raise SystemExit(f"cmake-version: could not rewrite set({name} ...)")
        text = _field_re(name).sub(f"set({name} {value})", text, count=1)
    return text


def main() -> int:
    """Parse arguments and dispatch to the requested mode."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "mode",
        choices=(
            "read",
            "write",
            "validate",
            "release-branch",
            "prep-branch",
            "check-pr",
            "highest-tag",
        ),
    )
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
    parser.add_argument(
        "--branch",
        metavar="BRANCH",
        help="validate mode: also assert the version may be prepared and tagged on BRANCH",
    )
    parser.add_argument(
        "--base-branch", metavar="BRANCH", help="check-pr mode: the branch the PR merges into"
    )
    parser.add_argument(
        "--head-branch", metavar="BRANCH", help="check-pr mode: the branch the PR merges from"
    )
    parser.add_argument(
        "--base-cmakelists",
        type=Path,
        metavar="PATH",
        help="check-pr mode: the base branch's CMakeLists.txt (--cmakelists is the merge result's)",
    )
    parser.add_argument("--cmakelists", type=Path, default=DEFAULT_CMAKELISTS)
    args = parser.parse_args()

    # `validate` is a pure grammar check on its ARGUMENT, so it deliberately runs
    # before the file is touched: a caller validating a dispatch input must not
    # also depend on a readable CMakeLists.txt.
    if args.mode == "validate":
        if args.tag:
            if args.branch is not None:
                raise SystemExit("cmake-version: --branch applies to a version, not --tag")
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
        if args.branch is not None:
            validate_branch(args.branch, args.version)
        return 0

    if args.mode == "release-branch":
        if not args.version:
            raise SystemExit("cmake-version: release-branch mode requires a version argument")
        print(release_branch(validate_version(args.version)))
        return 0

    if args.mode == "prep-branch":
        if not args.version:
            raise SystemExit("cmake-version: prep-branch mode requires a version argument")
        print(prep_branch(validate_version(args.version)))
        return 0

    # Prints nothing when no tag qualifies, so the caller reports the miss.
    if args.mode == "highest-tag":
        if not args.version:
            raise SystemExit("cmake-version: highest-tag mode requires a version argument")
        tag = highest_tag(sys.stdin.read().split(), validate_version(args.version))
        if tag is not None:
            print(tag)
        return 0

    text = args.cmakelists.read_text()

    if args.mode == "check-pr":
        if None in (args.base_branch, args.head_branch, args.base_cmakelists):
            raise SystemExit(
                "cmake-version: check-pr mode requires --base-branch, --head-branch "
                "and --base-cmakelists"
            )
        check_pr_version(
            args.base_branch,
            args.head_branch,
            read_version(args.base_cmakelists.read_text()),
            read_version(text),
        )
        return 0

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
