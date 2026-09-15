#!/usr/bin/env python3
"""Inspect built contract binaries and emit Boost.Test case lists for evidence."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


SUITE = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\*")
ENTRY = re.compile(r"(\s*)([^\s].*?)\*\s*")


def manifest_rows(path: Path) -> dict[str, tuple[str, set[str]]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("version") != 1 or not isinstance(document.get("suites"), list):
        raise ValueError("unsupported contract suite manifest")
    rows: dict[str, tuple[str, set[str]]] = {}
    for row in document["suites"]:
        name, source, accounts = row["suite"], row["source"], row["accounts"]
        if (not isinstance(name, str) or not isinstance(source, str)
                or not isinstance(accounts, list) or not accounts or name in rows):
            raise ValueError(f"invalid or duplicate manifest suite: {name!r}")
        rows[name] = (source, set(accounts))
    if not rows:
        raise ValueError("empty contract suite manifest")
    return rows


def binary_for_source(build_dir: Path, source: str) -> Path:
    return build_dir / "contracts" / "tests" / ("contract_" + Path(source).stem.replace(".", "_"))


def listed_blocks(binary: Path, build_dir: Path) -> dict[str, list[str]]:
    if not binary.is_file():
        raise ValueError(f"missing split contract test binary: {binary}")
    result = subprocess.run(
        [str(binary), "--list_content"], cwd=build_dir,
        capture_output=True, text=True, check=True,
    )
    blocks: dict[str, list[str]] = {}
    current: str | None = None
    for line in (result.stdout + result.stderr).splitlines():
        if SUITE.fullmatch(line):
            current = line[:-1]
            if current in blocks:
                raise ValueError(f"duplicate Boost suite in {binary.name}: {current}")
            blocks[current] = [line]
        elif current is not None and ENTRY.fullmatch(line):
            blocks[current].append(line)
    return blocks


def case_count(block: list[str]) -> int:
    entries = [
        (len(match.group(1).expandtabs(4)), match.group(2))
        for line in block[1:] if (match := ENTRY.fullmatch(line))
    ]
    leaves = 0
    for index, (indent, _) in enumerate(entries):
        if index + 1 == len(entries) or entries[index + 1][0] <= indent:
            leaves += 1
    if not leaves:
        raise ValueError(f"Boost suite {block[0]} lists no cases")
    return leaves


def inspect_binaries(
    rows: dict[str, tuple[str, set[str]]], build_dir: Path,
) -> tuple[dict[str, list[str]], int, int]:
    by_source: dict[str, set[str]] = {}
    for name, (source, _) in rows.items():
        by_source.setdefault(source, set()).add(name)
    blocks_by_suite: dict[str, list[str]] = {}
    total_cases = 0
    targets: set[str] = set()
    for source, expected in by_source.items():
        binary = binary_for_source(build_dir, source)
        if binary.name in targets:
            raise ValueError(f"two manifest sources map to {binary.name}")
        targets.add(binary.name)
        blocks = listed_blocks(binary, build_dir)
        if set(blocks) != expected:
            raise ValueError(
                f"{binary.name} lists wrong suites: "
                f"missing={sorted(expected - set(blocks))}, extra={sorted(set(blocks) - expected)}"
            )
        for name, block in blocks.items():
            total_cases += case_count(block)
            blocks_by_suite[name] = block
    return blocks_by_suite, len(targets), total_cases


def inspect_ctest(rows: dict[str, tuple[str, set[str]]], build_dir: Path) -> None:
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    tests = json.loads(result.stdout)["tests"]
    contract_entries = [test for test in tests if test["name"].startswith("contract.")]
    contract_tests = {test["name"]: test for test in contract_entries}
    if len(contract_entries) != len(contract_tests):
        raise ValueError("duplicate contract CTest entry")
    if any(test["name"] == "contracts_unit_test" for test in tests):
        raise ValueError("aggregate contract CTest entry still exists")
    expected = {f"contract.{name}" for name in rows}
    if set(contract_tests) != expected:
        raise ValueError(
            f"CTest suite mismatch: missing={sorted(expected - set(contract_tests))}, "
            f"extra={sorted(set(contract_tests) - expected)}"
        )
    for ctest_name, test in contract_tests.items():
        name = ctest_name.removeprefix("contract.")
        source, accounts = rows[name]
        binary = binary_for_source(build_dir, source)
        command = [str(binary), f"--run_test={name}", "--report_level=detailed", "--", "--sys-vm"]
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        if test.get("command") != command:
            raise ValueError(f"CTest {ctest_name} invokes a different suite")
        if Path(properties.get("WORKING_DIRECTORY", "")).resolve() != build_dir:
            raise ValueError(f"CTest {ctest_name} uses a different working directory")
        if set(properties.get("LABELS", [])) != {"contract", *accounts}:
            raise ValueError(f"CTest {ctest_name} labels differ from the manifest")
        if "contracts_source" in properties.get("RESOURCE_LOCK", []):
            raise ValueError(f"CTest {ctest_name} retains the contract-wide lock")
        if properties.get("TIMEOUT") != 2700.0:
            raise ValueError(f"CTest {ctest_name} changed the contract timeout")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list-selected", help="Comma-separated exact suite names")
    mode.add_argument("--list-all", action="store_true")
    args = parser.parse_args()
    try:
        rows = manifest_rows(args.manifest)
        build_dir = args.build_dir.resolve()
        blocks, binaries, cases = inspect_binaries(rows, build_dir)
        inspect_ctest(rows, build_dir)
        if args.list_selected is not None or args.list_all:
            selected = list(rows) if args.list_all else args.list_selected.split(",")
            if not selected or len(selected) != len(set(selected)) or any(name not in rows for name in selected):
                raise ValueError("case selector contains unknown or duplicate suites")
            for name in selected:
                print("\n".join(blocks[name]))
        else:
            print(f"contract case inventory passed: {len(rows)} suites, {binaries} binaries, {cases} cases")
    except (ValueError, OSError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(f"contract case inventory failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
