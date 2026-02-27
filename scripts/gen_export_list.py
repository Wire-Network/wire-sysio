#!/usr/bin/env python3
"""Generate linker export lists from native intrinsic export source files.

Scans C/C++ source for INTRINSIC_EXPORT function definitions and generates:
  --format=dynamic-list : Linux --dynamic-list format (default)
  --format=exported     : macOS -exported_symbols_list format

Usage:
  python3 gen_export_list.py [--format=dynamic-list|exported] <source_file> > output
"""

import re
import sys
import argparse

def extract_exports(source_path):
    """Extract function names preceded by INTRINSIC_EXPORT."""
    exports = []
    # Match: INTRINSIC_EXPORT <optional-attributes> <return-type> <name>(
    # The function name is the last identifier before the opening parenthesis
    pattern = re.compile(
        r'INTRINSIC_EXPORT\s+'
        r'(?:(?:\[\[.*?\]\]\s+)*)'       # optional [[attributes]]
        r'(?:\w+[\s*&]+)*?'              # return type tokens
        r'(\w+)\s*\(',                    # function name followed by (
        re.MULTILINE
    )
    with open(source_path, 'r') as f:
        content = f.read()

    for m in pattern.finditer(content):
        exports.append(m.group(1))

    return exports


def format_dynamic_list(symbols):
    """Linux --dynamic-list format."""
    lines = ['{\n']
    for s in symbols:
        lines.append(f'   {s};\n')
    lines.append('};\n')
    return ''.join(lines)


def format_exported_symbols(symbols):
    """macOS -exported_symbols_list format."""
    return '\n'.join(f'_{s}' for s in symbols) + '\n'


def main():
    parser = argparse.ArgumentParser(description='Generate linker export lists from INTRINSIC_EXPORT functions')
    parser.add_argument('source', help='Source file to scan')
    parser.add_argument('--format', choices=['dynamic-list', 'exported'], default='dynamic-list',
                        help='Output format (default: dynamic-list)')
    parser.add_argument('-o', '--output', help='Output file (default: stdout)')
    args = parser.parse_args()

    exports = extract_exports(args.source)
    if not exports:
        print(f"Warning: no INTRINSIC_EXPORT functions found in {args.source}", file=sys.stderr)
        sys.exit(1)

    if args.format == 'dynamic-list':
        content = format_dynamic_list(exports)
    else:
        content = format_exported_symbols(exports)

    if args.output:
        with open(args.output, 'w') as f:
            f.write(content)
    else:
        sys.stdout.write(content)


if __name__ == '__main__':
    main()
