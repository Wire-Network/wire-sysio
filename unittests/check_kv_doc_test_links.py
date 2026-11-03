#!/usr/bin/env python3
"""Checks that the tests the KV docs cite exist in unit_test.

The spec and the audit name the tests that pin each rule as ``suite/test``, or as a bare ``test`` after one, meaning the
same suite. A renamed or deleted test leaves a link that still reads well, so this compares every name with what
``unit_test --list_content`` reports. A doc that is not in this tree is skipped.

Usage: check_kv_doc_test_links.py --unit-test <path> <doc.md> [<doc.md> ...]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

BACKTICKED = re.compile(r"`([^`]+)`")
BOOST_NAME = r"[A-Za-z_][A-Za-z0-9_]*"
QUALIFIED = re.compile(rf"^{BOOST_NAME}(/{BOOST_NAME})+$")
BARE = re.compile(rf"^{BOOST_NAME}$")
SUITE_SUFFIXES = ("_test", "_tests")


def listed_tests(unit_test):
    """Returns the ``suite/test`` paths unit_test reports, with test case template arguments dropped."""
    listing = subprocess.run([unit_test, "--list_content"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             check=True).stdout
    tests, path = set(), []
    for line in listing.splitlines():
        match = re.match(r"^( *)(\S+?)\*?$", line)
        if not match or line.startswith("Random number"):
            continue
        depth = len(match.group(1)) // 4
        path[depth:] = [match.group(2).split("<")[0]]
        tests.add("/".join(path))
    return tests


def cited_tests(text):
    """Yields (line number, ``suite/test``) for each test a doc names.

    In a table only the last cell cites tests, and text in parentheses there is commentary. A bare name counts only
    after a qualified one in the same cell, because the other columns use backticks for intrinsics and values. Outside a
    table a qualified name whose first part looks like a suite counts, which leaves file paths alone.
    """
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("|"):
            cell = re.sub(r"\([^()]*\)", "", stripped.strip("|").split("|")[-1])
            suite = None
            for token in BACKTICKED.findall(cell):
                if QUALIFIED.match(token):
                    suite = token.rsplit("/", 1)[0]
                    yield number, token
                elif suite and BARE.match(token):
                    yield number, f"{suite}/{token}"
        else:
            for token in BACKTICKED.findall(line):
                if QUALIFIED.match(token) and token.split("/")[0].endswith(SUITE_SUFFIXES):
                    yield number, token


def main():
    """Reports each cited test that unit_test does not have, and fails if there is one."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--unit-test", required=True, help="path of the unit_test executable")
    parser.add_argument("docs", nargs="+", type=Path, help="markdown files to check")
    args = parser.parse_args()

    known = listed_tests(args.unit_test)
    missing, checked = [], 0
    for doc in args.docs:
        if not doc.is_file():
            print(f"skipping {doc}: not in this tree")
            continue
        for number, test in cited_tests(doc.read_text()):
            checked += 1
            if test not in known:
                missing.append(f"{doc}:{number}: {test}")

    print(f"{checked} test links checked, {len(missing)} broken")
    print("\n".join(missing))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
