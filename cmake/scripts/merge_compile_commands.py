#!/usr/bin/env python3
"""Merge multiple compile_commands.json files into one.

Usage:
    merge_compile_commands.py -o OUTPUT INPUT [INPUT ...]

Each INPUT is a path to a compile_commands.json file.  Entries are
de-duplicated by (directory, file) pair, with later files winning on
conflict.  Missing input files are silently skipped so that the merge
succeeds even when not all ExternalProjects have been built yet.
"""

import argparse
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Merge compile_commands.json files")
    parser.add_argument("-o", "--output", required=True, help="Output file path")
    parser.add_argument("inputs", nargs="+", help="Input compile_commands.json files")
    args = parser.parse_args()

    seen = {}  # (directory, file) -> entry
    for path_str in args.inputs:
        p = Path(path_str)
        if not p.exists():
            continue
        try:
            with open(p) as f:
                entries = json.load(f)
        except (json.JSONDecodeError, OSError):
            print(f"warning: skipping unreadable file {p}", file=sys.stderr)
            continue
        for entry in entries:
            key = (entry.get("directory", ""), entry.get("file", ""))
            seen[key] = entry

    merged = list(seen.values())

    output = Path(args.output)
    # Only write if content actually changed to avoid unnecessary rebuilds
    new_content = json.dumps(merged, indent=2) + "\n"
    if output.exists():
        try:
            old_content = output.read_text()
            if old_content == new_content:
                return
        except OSError:
            pass

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(new_content)


if __name__ == "__main__":
    main()
