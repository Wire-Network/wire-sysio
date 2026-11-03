#!/usr/bin/env python3
"""Unit tests for ``cmake-version.py``, the release workflows' version parser.

Run with ``python3 -m unittest discover -s .github/scripts -p 'test_*.py'``. Kept
identical in wire-sysio and wire-cdt, like the parser it tests.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

#: The parser under test. Its hyphenated file name cannot be imported by name.
SCRIPT = Path(__file__).resolve().with_name("cmake-version.py")

_spec = importlib.util.spec_from_file_location("cmake_version", SCRIPT)
cmake_version = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cmake_version)

#: A root CMakeLists.txt version block shaped like the real ones.
CMAKELISTS = """cmake_minimum_required(VERSION 3.16)

set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX "")

project(example)
"""


def run_script(*args: str) -> subprocess.CompletedProcess[str]:
    """Run the parser the way the workflows do, capturing its output."""
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


class ValidateVersionTests(unittest.TestCase):
    """The ``<major>.<minor>.<patch>[-<suffix>]`` grammar."""

    def test_accepts_stable_and_suffixed_versions(self) -> None:
        for version in ("1.0.0", "1.1.0-rc1", "1.0.0-dev", "10.20.30", "1.1.0-rc.2"):
            with self.subTest(version=version):
                self.assertEqual(cmake_version.validate_version(version), version)

    def test_rejects_malformed_versions(self) -> None:
        for version in ("", "1.0", "1.0.0.0", "v1.0.0", "1.0.0-", "1.0.0-rc-1", "1.0.0-rc..1", "1.x.0", "1.0.0\n"):
            with self.subTest(version=version), self.assertRaises(SystemExit):
                cmake_version.validate_version(version)

    def test_tag_form_returns_the_bare_version(self) -> None:
        self.assertEqual(cmake_version.validate_version("v1.1.0-rc1", tag=True), "1.1.0-rc1")

    def test_tag_form_requires_the_leading_v(self) -> None:
        with self.assertRaises(SystemExit):
            cmake_version.validate_version("1.1.0", tag=True)


class ReleaseBranchTests(unittest.TestCase):
    """``release_branch`` names a version's ``release/<major>.<minor>`` line."""

    def test_names_the_major_minor_line(self) -> None:
        self.assertEqual(cmake_version.release_branch("1.1.0"), "release/1.1")
        self.assertEqual(cmake_version.release_branch("1.1.3-rc2"), "release/1.1")
        self.assertEqual(cmake_version.release_branch("10.20.0"), "release/10.20")


class ValidateBranchTests(unittest.TestCase):
    """Which branch may carry which version."""

    def test_master_takes_any_version(self) -> None:
        for version in ("1.0.0", "1.1.0-rc1", "2.0.0-dev", "1.1.7"):
            with self.subTest(version=version):
                cmake_version.validate_branch("master", version)

    def test_release_branch_takes_its_own_line(self) -> None:
        for version in ("1.1.0", "1.1.0-rc1", "1.1.0-dev", "1.1.7"):
            with self.subTest(version=version):
                cmake_version.validate_branch("release/1.1", version)

    def test_release_branch_refuses_other_lines(self) -> None:
        # 1.10.0 and 11.1.0 guard against a prefix match on "release/1.1".
        for version in ("1.2.0", "1.0.1", "2.1.0", "1.10.0", "11.1.0", "0.1.0"):
            with self.subTest(version=version), self.assertRaises(SystemExit):
                cmake_version.validate_branch("release/1.1", version)

    def test_refuses_branches_that_are_neither_master_nor_a_release_line(self) -> None:
        for branch in (
            "",
            "main",
            "develop",
            "feature/release-1.1",
            "release/prep-v1.1.0",
            "release/1",
            "release/v1.1",
            "release/1.1/",
            "release/1.1.0",
            "refs/heads/master",
            "refs/heads/release/1.1",
        ):
            with self.subTest(branch=branch), self.assertRaises(SystemExit):
                cmake_version.validate_branch(branch, "1.1.0")


class CommandLineTests(unittest.TestCase):
    """``validate`` as the workflows invoke it."""

    def test_validate_with_matching_branch_succeeds_silently(self) -> None:
        for branch in ("master", "release/1.1"):
            with self.subTest(branch=branch):
                result = run_script("validate", "1.1.0-rc1", "--branch", branch)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_validate_with_other_line_names_the_expected_branch(self) -> None:
        result = run_script("validate", "1.2.0", "--branch", "release/1.1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release/1.2", result.stderr)

    def test_validate_without_branch_checks_only_the_grammar(self) -> None:
        self.assertEqual(run_script("validate", "1.2.0").returncode, 0)
        self.assertNotEqual(run_script("validate", "1.2").returncode, 0)

    def test_validate_refuses_a_trailing_newline(self) -> None:
        result = run_script("validate", "1.1.0\n", "--branch", "release/1.1")
        self.assertNotEqual(result.returncode, 0)

    def test_release_branch_prints_the_line(self) -> None:
        result = run_script("release-branch", "1.1.0-rc1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "release/1.1")

    def test_release_branch_refuses_a_malformed_version(self) -> None:
        self.assertNotEqual(run_script("release-branch", "1.1").returncode, 0)
        self.assertNotEqual(run_script("release-branch").returncode, 0)

    def test_branch_with_tag_is_refused(self) -> None:
        result = run_script("validate", "--tag", "v1.1.0", "--branch", "release/1.1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--branch", result.stderr)

    def test_validate_tag_prints_the_bare_version(self) -> None:
        result = run_script("validate", "--tag", "v1.1.0-rc1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "1.1.0-rc1")


class ReadWriteTests(unittest.TestCase):
    """``read`` / ``write`` against a CMakeLists.txt."""

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.cmakelists = Path(directory.name) / "CMakeLists.txt"
        self.cmakelists.write_text(CMAKELISTS)

    def test_read_composes_the_version(self) -> None:
        result = run_script("read", "--cmakelists", str(self.cmakelists))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "1.0.0")

    def test_write_round_trips_and_keeps_the_layout(self) -> None:
        result = run_script("write", "1.1.0-rc1", "--cmakelists", str(self.cmakelists))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "1.1.0-rc1")
        text = self.cmakelists.read_text()
        self.assertIn("set(VERSION_MINOR 1)", text)
        self.assertIn("set(VERSION_SUFFIX rc1)", text)
        self.assertEqual(text.count("\n"), CMAKELISTS.count("\n"))

    def test_write_stable_writes_an_empty_suffix(self) -> None:
        run_script("write", "1.1.0-rc1", "--cmakelists", str(self.cmakelists))
        result = run_script("write", "1.1.0", "--cmakelists", str(self.cmakelists))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('set(VERSION_SUFFIX "")', self.cmakelists.read_text())


if __name__ == "__main__":
    unittest.main()
