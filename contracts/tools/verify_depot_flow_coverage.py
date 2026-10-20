#!/usr/bin/env python3
"""Validate the reviewed flow/native-test map; optionally detect sibling flow drift."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAP = ROOT / "contracts/tests/depot_flow_coverage.json"
MACRO = re.compile(r"BOOST_(?:AUTO|FIXTURE|DATA)_TEST_(SUITE_END|SUITE|CASE)\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)?")


def inventory(root):
    result = {}
    for source in sorted((root / "contracts/tests").glob("*.cpp")):
        # Ignore commented-out declarations. This is an inventory check, not a C++ parser.
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", source.read_text(), flags=re.S)
        stack = []
        for kind, name in MACRO.findall(text):
            if kind == "SUITE_END":
                stack.pop()
            elif kind == "SUITE":
                stack.append(name)
            elif stack:
                test = "/".join([*stack, name])
                if test in result:
                    raise ValueError(f"duplicate test: {test}")
                result[test] = str(source.relative_to(root))
    return result


def validate(data, tests, enabled=None):
    flows = data["flows"]
    names = [flow["name"] for flow in flows]
    if len(names) != len(set(names)):
        raise ValueError("duplicate flow entry")
    if enabled is not None and set(names) != set(enabled):
        raise ValueError(f"flow drift: unmapped={sorted(set(enabled) - set(names))}, "
                         f"no longer enabled={sorted(set(names) - set(enabled))}")
    selected = set()
    for flow in flows:
        if not flow.get("wire_behavior") or not flow.get("cluster_only") or not flow.get("tests"):
            raise ValueError(f"incomplete boundary description: {flow['name']}")
        for test in flow["tests"]:
            if test not in tests:
                raise ValueError(f"{flow['name']}: missing native test {test}")
            selected.add(test)
    return sorted(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, help="wire-tools-ts checkout to compare enabled test scripts")
    parser.add_argument("--filter", action="store_true", help="print Boost --run_test value only")
    args = parser.parse_args()
    enabled = None
    if args.tools_root:
        packages = list((args.tools_root / "packages").glob("flow-*/package.json"))
        if not packages:
            parser.error("no flow package manifests found")
        enabled = [p.parent.name for p in packages if json.loads(p.read_text()).get("scripts", {}).get("test")]
    try:
        data = json.loads(MAP.read_text())
        selected = validate(data, inventory(ROOT), enabled)
    except (ValueError, KeyError) as error:
        parser.exit(1, f"{error}\n")
    print(":".join(selected) if args.filter else
          f"{len(data['flows'])} flows mapped to {len(selected)} native cases; references and boundaries valid")


if __name__ == "__main__":
    main()
