#!/usr/bin/env python3
"""Verify split contract binaries and print exact Boost case manifests."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


SUITE_NAME = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")
SUITE_DECLARATION = re.compile(r"^BOOST_AUTO_TEST_SUITE\(([A-Za-z_][A-Za-z_0-9]*)\)", re.M)
LISTED_SUITE = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\*")
LISTED_ENTRY = re.compile(r"(\s*)([^\s].*?)\*\s*")
ARTIFACT_ACCESSOR = re.compile(r"contracts::([A-Za-z_][A-Za-z_0-9]*)_(?:wasm|abi)\(")
INCLUDED_ACCOUNT = re.compile(r"^\s*#include\s*[<\"](sysio\.[A-Za-z0-9.]+)(?:/|\.hpp)", re.M)
ACCESSOR_ACCOUNT = {
    "reserve": "sysio.reserv", "system": "sysio.system", "sysio_system": "sysio.system",
    "sysio_token": "sysio.token", "sendinline": "test.sendinline",
    "blockinfo_tester": "test.blockinfo_tester", "noop": "test.noop",
}


def binary_for_source(build_dir: Path, source: str) -> Path:
    return build_dir / "contracts" / "tests" / ("contract_" + Path(source).stem.replace(".", "_"))


def manifest_rows(manifest_path: Path) -> dict[str, tuple[str, set[str]]]:
    manifest_path = manifest_path.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("version") != 1 or not isinstance(manifest.get("suites"), list) or not manifest["suites"]:
        raise ValueError("unsupported or empty contract suite manifest")
    declared: dict[str, tuple[str, set[str]]] = {}
    binary_owners: dict[str, str] = {}
    for row in manifest["suites"]:
        if not isinstance(row, dict):
            raise ValueError("contract suite manifest contains a non-object row")
        name, source, accounts = row.get("suite"), row.get("source"), row.get("accounts")
        if not isinstance(name, str) or not SUITE_NAME.fullmatch(name) or name in declared:
            raise ValueError(f"invalid or duplicate suite name: {name!r}")
        if not isinstance(source, str) or Path(source).name != source or not source.endswith(".cpp"):
            raise ValueError(f"invalid owning source for {name}: {source!r}")
        if not isinstance(accounts, list) or not accounts or any(
            not isinstance(account, str) or not account for account in accounts
        ) or len(accounts) != len(set(accounts)):
            raise ValueError(f"invalid account metadata for {name}")
        source_path = manifest_path.parent / source
        if not source_path.is_file():
            raise ValueError(f"missing owning source for {name}: {source}")
        target = binary_for_source(Path("."), source).name
        if target in binary_owners and binary_owners[target] != source:
            raise ValueError(f"two sources map to the same binary target: {source}")
        binary_owners[target] = source
        source_text = source_path.read_text(encoding="utf-8")
        required = {
            ACCESSOR_ACCOUNT.get(accessor, f"sysio.{accessor}")
            for accessor in ARTIFACT_ACCESSOR.findall(source_text)
        }
        required.update(INCLUDED_ACCOUNT.findall(source_text))
        required.discard("sysio.system_tester")
        if '"sysio.system_tester.hpp"' in source_text:
            required.update({"sysio.system", "sysio.token", "sysio.msig"})
        missing = sorted(required - set(accounts))
        if missing:
            raise ValueError(f"{name} omits exercised artifacts: {missing}")
        declared[name] = (source, set(accounts))

    actual_source: dict[str, str] = {}
    for source_path in manifest_path.parent.glob("*.cpp"):
        for name in SUITE_DECLARATION.findall(source_path.read_text(encoding="utf-8")):
            if name in actual_source:
                raise ValueError(f"duplicate Boost suite in source: {name}")
            actual_source[name] = source_path.name
    expected_source = {name: source for name, (source, _) in declared.items()}
    if expected_source != actual_source:
        missing = sorted(set(actual_source) - set(expected_source))
        extra = sorted(set(expected_source) - set(actual_source))
        wrong_owner = sorted(
            name for name in set(expected_source) & set(actual_source)
            if expected_source[name] != actual_source[name]
        )
        raise ValueError(
            f"source/manifest mismatch: missing={missing}, extra={extra}, wrong_owner={wrong_owner}"
        )
    return declared


def listed_blocks(binary: Path, build_dir: Path) -> dict[str, list[str]]:
    listed = subprocess.run(
        [str(binary), "--list_content"], cwd=build_dir,
        capture_output=True, text=True, check=True,
    )
    blocks: dict[str, list[str]] = {}
    current: str | None = None
    for line in (listed.stdout + listed.stderr).splitlines():
        suite = LISTED_SUITE.fullmatch(line)
        if suite:
            current = suite.group(1)
            if current in blocks:
                raise ValueError(f"duplicate suite in executable {binary}: {current}")
            blocks[current] = [line]
        elif current is not None and LISTED_ENTRY.fullmatch(line):
            blocks[current].append(line)
    return blocks


def leaf_cases(block: list[str]) -> list[str]:
    entries: list[tuple[int, str]] = []
    for line in block:
        match = LISTED_ENTRY.fullmatch(line)
        if match:
            entries.append((len(match.group(1).expandtabs(4)), match.group(2)))
    cases: list[str] = []
    stack: list[tuple[int, str]] = []
    for index, (indent, name) in enumerate(entries):
        while stack and stack[-1][0] >= indent:
            stack.pop()
        next_indent = entries[index + 1][0] if index + 1 < len(entries) else -1
        if next_indent > indent:
            stack.append((indent, name))
        else:
            cases.append("/".join([*(part[1] for part in stack), name]))
    if not cases or len(cases) != len(set(cases)):
        raise ValueError(f"suite {block[0]} lists no cases or duplicate cases")
    return cases


def binary_inventory(
    declared: dict[str, tuple[str, set[str]]], build_dir: Path,
) -> tuple[dict[str, list[str]], int]:
    expected_by_source: dict[str, set[str]] = {}
    for suite, (source, _) in declared.items():
        expected_by_source.setdefault(source, set()).add(suite)
    blocks_by_suite: dict[str, list[str]] = {}
    total_cases = 0
    for source, expected in expected_by_source.items():
        binary = binary_for_source(build_dir, source)
        if not binary.is_file():
            raise ValueError(f"missing split contract test binary: {binary}")
        blocks = listed_blocks(binary, build_dir)
        if set(blocks) != expected:
            raise ValueError(
                f"executable/manifest mismatch for {binary.name}: "
                f"missing={sorted(expected - set(blocks))}, extra={sorted(set(blocks) - expected)}"
            )
        for suite, block in blocks.items():
            total_cases += len(leaf_cases(block))
            blocks_by_suite[suite] = block
    return blocks_by_suite, total_cases


def check(manifest_path: Path, build_dir: Path) -> dict[str, int]:
    build_dir = build_dir.resolve()
    declared = manifest_rows(manifest_path)
    _, total_cases = binary_inventory(declared, build_dir)
    ctest = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    tests = json.loads(ctest.stdout)["tests"]
    registered: dict[str, dict] = {}
    for test in tests:
        test_name = test["name"]
        if test_name == "contracts_unit_test":
            raise ValueError("aggregate contract CTest entry still exists")
        if test_name.startswith("contract."):
            if test_name in registered:
                raise ValueError(f"duplicate CTest entry: {test_name}")
            registered[test_name] = test
    expected = {f"contract.{name}" for name in declared}
    if set(registered) != expected:
        raise ValueError(
            "CTest/manifest mismatch: "
            f"missing={sorted(expected - set(registered))}, "
            f"extra={sorted(set(registered) - expected)}"
        )
    for name, test in registered.items():
        suite = name.removeprefix("contract.")
        source, accounts = declared[suite]
        binary = binary_for_source(build_dir, source)
        command = test.get("command", [])
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        expected_command = [
            str(binary), f"--run_test={suite}", "--report_level=detailed", "--", "--sys-vm"
        ]
        if command != expected_command:
            raise ValueError(f"CTest {name} command differs from the declared suite invocation")
        if Path(properties.get("WORKING_DIRECTORY", "")).resolve() != build_dir:
            raise ValueError(f"CTest {name} uses a different working directory")
        labels = properties.get("LABELS", [])
        if not isinstance(labels, list) or set(labels) != {"contract", *accounts}:
            raise ValueError(f"CTest {name} labels disagree with account metadata")
        if "contracts_source" in properties.get("RESOURCE_LOCK", []):
            raise ValueError(f"CTest {name} retains the contract-wide serial lock")
        if properties.get("TIMEOUT") != 2700.0:
            raise ValueError(f"CTest {name} changed the contract timeout")
    return {"suites": len(declared), "binaries": len({row[0] for row in declared.values()}), "cases": total_cases}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list-selected", help="Comma-separated exact suite names")
    mode.add_argument("--list-all", action="store_true")
    args = parser.parse_args()
    try:
        declared = manifest_rows(args.manifest)
        if args.list_selected is not None or args.list_all:
            suites = list(declared) if args.list_all else args.list_selected.split(",")
            if not suites or any(suite not in declared for suite in suites) or len(suites) != len(set(suites)):
                raise ValueError("case selector contains unknown or duplicate suites")
            blocks, _ = binary_inventory(declared, args.build_dir.resolve())
            for suite in suites:
                print("\n".join(blocks[suite]))
        else:
            report = check(args.manifest, args.build_dir)
            print(
                "contract suite inventory passed: "
                f"{report['suites']} suites, {report['binaries']} binaries, {report['cases']} cases"
            )
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError, TypeError) as exc:
        print(f"contract suite inventory failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
