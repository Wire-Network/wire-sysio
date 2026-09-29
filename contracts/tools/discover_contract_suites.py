#!/usr/bin/env python3
"""
Register each top-level Boost.Test unit of a test executable as its own CTest entry.

CMake runs this after `contracts_unit_test` links. The executable's `--list_content=DOT` output is
the inventory, so adding a suite needs no CMake edit. Each entry is labelled with `contract` plus
every contract its sources deploy or compile against: accessor references such as
`contracts::liq_wasm()` and contract header includes such as `<sysio.opp.common/amm_math.hpp>`,
scanned in the files that declare the unit, its test cases and their dependencies, and in the local
headers those files include. Contracts the test library deploys during default tester setup label
every entry.

The labels over-approximate, since a helper in a shared header labels every suite that includes it.
That is the safe direction for selecting tests by changed contract.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Boost.Test `--list_content=DOT` grammar (boost/test/impl/unit_test_main.ipp, dot_content_reporter)
# ---------------------------------------------------------------------------

DOT_NODE_RE = re.compile(r'^tu(?P<id>\d+)\[(?P<attrs>.*)\];$')
DOT_PARENT_EDGE_RE = re.compile(r'^tu(?P<parent>\d+) -> tu(?P<child>\d+);$')
DOT_DEPENDENCY_EDGE_RE = re.compile(r'^tu(?P<unit>\d+) -> tu(?P<dependency>\d+)\[[^\]]*\];$')
DOT_LABEL_RE = re.compile(r'label="(?P<label>[^"]*)"')
DOT_LOCATION_RE = re.compile(r'^(?P<file>.*)\((?P<line>\d+)\)$')
DOT_MASTER_ATTR = 'peripheries=2'
DOT_ENABLED_ATTR = 'color=green'
DOT_LABEL_FIELD_SEPARATOR = '|'

# ---------------------------------------------------------------------------
# Contract artifact accessors, as defined by the headers passed with --accessor-header
# ---------------------------------------------------------------------------

# Every `*_wasm(`/`*_abi(` in an accessor header other than a loader call must map to a contract.
ACCESSOR_TOKEN_RE = re.compile(r'\b(?P<name>\w+_(?:wasm|abi))\s*\(')
ACCESSOR_LOADERS = frozenset({'read_wasm', 'read_abi'})
ACCESSOR_FUNCTION_RE = re.compile(
    r'\b(?P<name>\w+_(?:wasm|abi))\s*\([^;{}()]*\)[^;{}]*\{[^{}]*?"(?P<path>[^"]+\.(?:wasm|abi))"', re.S)
ACCESSOR_MACRO_RE = re.compile(
    r'^\s*MAKE_\w*WASM_ABI\(\s*(?P<prefix>\w+)\s*,\s*(?P<contract>[\w.]+)\s*,\s*[\w./-]+\s*\)', re.M)
ACCESSOR_SUFFIXES = ('_wasm', '_abi')
CONTRACT_PATH_RE = re.compile(r'/contracts/(?P<contract>sysio\.[\w.]+)/[^/]+\.(?:wasm|abi)$')
TEST_CONTRACT_PATH_RE = re.compile(r'/test[_-]contracts/(?P<name>\w+?)(?:\.(?:wasm|abi)$|/)')
SYSTEM_CONTRACT_PREFIX = 'sysio.'
TEST_CONTRACT_LABEL_PREFIX = 'test.'

# ---------------------------------------------------------------------------
# Contract references in test sources
# ---------------------------------------------------------------------------

ACCESSOR_CALL_RE = re.compile(r'\b(?P<name>\w+_(?:wasm|abi))\s*\(')
ACCESSOR_QUALIFIED_RE = re.compile(r'::\s*(?P<name>\w+_(?:wasm|abi))\b')
INCLUDE_PATH_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"](?P<path>[^>"]+)[>"]', re.M)
LOCAL_INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"(?P<path>[^"]+)"', re.M)
# A contract's directory anywhere in an include path: <sysio.opp.common/...>, "../../contracts/sysio.roa/...".
CONTRACT_COMPONENT_RE = re.compile(r'(?:^|/)(?P<contract>sysio\.[\w.]+)/')
TEST_CONTRACT_COMPONENT_RE = re.compile(r'(?:^|/)test[_-]contracts/(?P<name>\w+)/')
RAW_STRING_OPEN_RE = re.compile(r'R"(?P<delimiter>[^()\\\s]{0,16})\(')

DEFAULT_TEST_PREFIX = 'contract.'
DEFAULT_BASE_LABEL = 'contract'
DISCOVERY_FAILED_NAME = 'DISCOVERY_FAILED'
# The command line overrides BOOST_TEST_REPORT_SINK, which would otherwise divert the listing into a file.
LIST_CONTENT_ARGUMENTS = ('--list_content=DOT', '--report_sink=stderr')
LIST_CONTENT_TIMEOUT_SECONDS = 120
LISTING_ERROR_TAIL_CHARACTERS = 4000
# mkstemp creates 0600; the script is read by whoever runs ctest.
GENERATED_FILE_MODE = 0o644


class DiscoveryError(RuntimeError):
    """A fault in discovery itself or its inputs; the build should fail."""


class ListingError(DiscoveryError):
    """The test executable could not produce its listing; its failure belongs in the test results."""


@dataclass
class TestUnit:
    """A top-level Boost.Test unit: a suite or a test case declared outside any suite."""

    name: str
    enabled: bool
    files: set[str] = field(default_factory=set)


# ---------------------------------------------------------------------------
# Inventory
# ---------------------------------------------------------------------------

def parse_dot(text: str) -> list[TestUnit]:
    """Return the master suite's children, each with the files declaring it, its subtree and their dependencies.

    Dependencies count because `--run_test` enables the units a selected unit depends on; depending on the master
    suite runs everything.
    """
    nodes: dict[str, tuple[str, bool, str]] = {}
    children: dict[str, list[str]] = {}
    dependencies: dict[str, list[str]] = {}
    master_id: str | None = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        node = DOT_NODE_RE.match(line)
        if node:
            attrs = node.group('attrs')
            label = DOT_LABEL_RE.search(attrs)
            if label is None:
                raise DiscoveryError(f'test unit without a label: {line}')
            if DOT_MASTER_ATTR in attrs.split(','):
                master_id = node.group('id')
                continue
            fields = label.group('label').split(DOT_LABEL_FIELD_SEPARATOR)
            location = DOT_LOCATION_RE.match(fields[1]) if len(fields) > 1 else None
            if location is None:
                raise DiscoveryError(f'test unit without a source location: {line}')
            nodes[node.group('id')] = (fields[0], DOT_ENABLED_ATTR in attrs.split(','), location.group('file'))
            continue
        edge = DOT_PARENT_EDGE_RE.match(line)
        if edge:
            children.setdefault(edge.group('parent'), []).append(edge.group('child'))
            continue
        dependency = DOT_DEPENDENCY_EDGE_RE.match(line)
        if dependency:
            dependencies.setdefault(dependency.group('unit'), []).append(dependency.group('dependency'))

    if master_id is None:
        raise DiscoveryError('no master test suite in the --list_content=DOT output')

    units = []
    for unit_id in children.get(master_id, []):
        files: set[str] = set()
        visited: set[str] = set()
        pending = [unit_id]
        while pending:
            current = pending.pop()
            if current in visited:
                continue
            visited.add(current)
            if current == master_id:
                files.update(file for _, _, file in nodes.values())
                continue
            if current not in nodes:
                raise DiscoveryError(f'edge to undeclared test unit tu{current}')
            files.add(nodes[current][2])
            pending.extend(children.get(current, []))
            pending.extend(dependencies.get(current, []))
        name, enabled, _ = nodes[unit_id]
        units.append(TestUnit(name=name, enabled=enabled, files=files))
    return sorted(units, key=lambda unit: unit.name)


def list_test_units(executable: Path, working_directory: Path) -> list[TestUnit]:
    """Run the executable's DOT listing and parse it."""
    command = [str(executable), *LIST_CONTENT_ARGUMENTS]
    try:
        result = subprocess.run(command, cwd=working_directory, capture_output=True, text=True, errors='replace',
                                timeout=LIST_CONTENT_TIMEOUT_SECONDS, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ListingError(f'cannot run {" ".join(command)}: {error}') from error
    if result.returncode != 0:
        raise ListingError(f'{" ".join(command)} exited with {result.returncode}:\n'
                           f'{result.stderr[-LISTING_ERROR_TAIL_CHARACTERS:]}')
    units = parse_dot(result.stderr + '\n' + result.stdout)
    if not units:
        raise ListingError(f'{executable} declares no test units')
    return units


# ---------------------------------------------------------------------------
# Labels
# ---------------------------------------------------------------------------

def label_for_artifact(path: str) -> str | None:
    """Map a contract artifact path to its label: `sysio.<name>`, or `test.<name>` for a test contract."""
    contract = CONTRACT_PATH_RE.search(path)
    if contract:
        return contract.group('contract')
    test_contract = TEST_CONTRACT_PATH_RE.search(path)
    if test_contract:
        return TEST_CONTRACT_LABEL_PREFIX + test_contract.group('name')
    return None


def label_for_contract(contract: str) -> str:
    """Label a contract named by a MAKE_*_WASM_ABI invocation, whatever directory it is built in."""
    return contract if contract.startswith(SYSTEM_CONTRACT_PREFIX) else TEST_CONTRACT_LABEL_PREFIX + contract


def load_accessor_labels(headers: list[Path]) -> dict[str, set[str]]:
    """Map each contract accessor name (`liq_wasm`, `sysio_system_abi`, ...) to its contract label.

    An accessor the rules cannot map is an error: every suite calling it would otherwise lose that contract's
    label without any sign.
    """
    accessors: dict[str, set[str]] = {}
    tokens: dict[Path, set[str]] = {}
    for header in headers:
        text = strip_comments(header.read_text(encoding='utf-8', errors='replace'))
        tokens[header] = {match.group('name') for match in ACCESSOR_TOKEN_RE.finditer(text)} - ACCESSOR_LOADERS
        for match in ACCESSOR_FUNCTION_RE.finditer(text):
            label = label_for_artifact(match.group('path'))
            if label:
                accessors.setdefault(match.group('name'), set()).add(label)
        for match in ACCESSOR_MACRO_RE.finditer(text):
            for suffix in ACCESSOR_SUFFIXES:
                accessors.setdefault(match.group('prefix') + suffix, set()).add(
                    label_for_contract(match.group('contract')))
    for header, names in tokens.items():
        unmapped = names - accessors.keys()
        if unmapped:
            raise DiscoveryError(f'{header}: no contract label for accessor(s) {", ".join(sorted(unmapped))}; '
                                 'extend label_for_artifact or the accessor patterns to cover them')
    if not accessors:
        raise DiscoveryError('no contract accessors found in ' + ', '.join(str(h) for h in headers))
    return accessors


def strip_comments(text: str) -> str:
    """Blank out C++ comments, keeping string and character literals and line structure intact."""
    out: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        char = text[i]
        raw = RAW_STRING_OPEN_RE.match(text, i) if char == 'R' else None
        if raw:
            close = ')' + raw.group('delimiter') + '"'
            end = text.find(close, raw.end())
            end = length if end < 0 else end + len(close)
            out.append(text[i:end])
            i = end
            continue
        # A quote after a digit is a digit separator (2'000'000), not the start of a character literal.
        if char == '"' or (char == "'" and not (i > 0 and text[i - 1].isdigit())):
            end = i + 1
            while end < length and text[end] != char and text[end] != '\n':
                end += 2 if text[end] == '\\' else 1
            end = min(end + 1, length)
            out.append(text[i:end])
            i = end
        elif text.startswith('//', i):
            end = text.find('\n', i)
            i = length if end < 0 else end
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = length if end < 0 else end + 2
            out.append('\n' * text.count('\n', i, end))
            i = end
        else:
            out.append(char)
            i += 1
    return ''.join(out)


def scan_labels(sources: set[Path], accessors: dict[str, set[str]]) -> set[str]:
    """Collect the contract labels referenced by the sources and, transitively, their local headers."""
    labels: set[str] = set()
    pending = sorted(sources)
    visited: set[Path] = set()
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        visited.add(source)
        text = strip_comments(source.read_text(encoding='utf-8', errors='replace'))
        for pattern in (ACCESSOR_CALL_RE, ACCESSOR_QUALIFIED_RE):
            for match in pattern.finditer(text):
                labels |= accessors.get(match.group('name'), set())
        for include in INCLUDE_PATH_RE.finditer(text):
            path = include.group('path')
            labels.update(match.group('contract') for match in CONTRACT_COMPONENT_RE.finditer(path))
            labels.update(TEST_CONTRACT_LABEL_PREFIX + match.group('name')
                          for match in TEST_CONTRACT_COMPONENT_RE.finditer(path))
        for match in LOCAL_INCLUDE_RE.finditer(text):
            header = (source.parent / match.group('path')).resolve()
            if header.is_file() and header not in visited:
                pending.append(header)
    return labels


def resolve_source(location: str, sources: list[Path]) -> list[Path]:
    """Find the file a unit's location names.

    An existing absolute path is taken as is. Otherwise `__FILE__` was rewritten, relative to the compile directory
    (ccache's base_dir) or through a prefix map, so the location is matched to the target's own sources by the
    longest shared trailing path; ties return every candidate.
    """
    path = Path(location)
    if path.is_absolute() and path.is_file():
        return [path.resolve()]
    trailing = path.parts[::-1]
    best = 0
    matches: list[Path] = []
    for source in sources:
        shared = 0
        for mine, theirs in zip(trailing, source.parts[::-1]):
            if mine != theirs:
                break
            shared += 1
        if shared and shared >= best:
            if shared > best:
                best, matches = shared, []
            matches.append(source)
    return matches


def every_known_label(accessors: dict[str, set[str]], implicit: set[str], sources: list[Path]) -> set[str]:
    """Every label any entry could carry, for entries whose own sources are unknown."""
    return set().union(implicit, *accessors.values(), scan_labels(set(sources), accessors))


def derive_labels(units: list[TestUnit], accessors: dict[str, set[str]], implicit: set[str],
                  sources: list[Path]) -> dict[str, set[str]]:
    """Label every unit; one whose sources cannot be found gets every known label, so it is never under-selected."""
    labels: dict[str, set[str]] = {}
    fallback: set[str] | None = None
    for unit in units:
        resolved = {file: resolve_source(file, sources) for file in unit.files}
        missing = sorted(file for file, paths in resolved.items() if not paths)
        if not missing:
            labels[unit.name] = scan_labels({path for paths in resolved.values() for path in paths},
                                            accessors) | implicit
            continue
        if fallback is None:
            fallback = every_known_label(accessors, implicit, sources)
        print(f'{Path(__file__).name}: warning: cannot find {", ".join(missing)} for {unit.name}; '
              'labelling it with every known contract', file=sys.stderr)
        labels[unit.name] = fallback
    return labels


# ---------------------------------------------------------------------------
# CTest script
# ---------------------------------------------------------------------------

def bracket(value: str) -> str:
    """Quote a value as a CMake bracket argument, which needs no escaping.

    The trailing `]` accounts for a value that ends in the start of the closing bracket.
    """
    level = 0
    while f']{"=" * level}]' in value + ']':
        level += 1
    equals = '=' * level
    return f'[{equals}[{value}]{equals}]'


def script_header(executable: Path) -> str:
    """The first line of every generated script."""
    return (f'# Generated by {Path(__file__).name} from `{executable.name} {" ".join(LIST_CONTENT_ARGUMENTS)}`. '
            'Do not edit.')


def render_properties(test_name: str, properties: list[str]) -> str:
    """One `set_tests_properties` call."""
    return f'set_tests_properties({test_name} PROPERTIES {" ".join(properties)})'


def render_ctest(units: list[TestUnit], labels: dict[str, set[str]], executable: Path,
                 working_directory: Path, timeout: int, test_arguments: list[str],
                 test_prefix: str, base_label: str) -> str:
    """Render one `add_test` and one `set_tests_properties` per unit, in name order."""
    lines = [script_header(executable)]
    for unit in units:
        test_name = bracket(test_prefix + unit.name)
        command = [str(executable), f'--run_test={unit.name}', *test_arguments]
        lines.append(f'add_test({test_name} {" ".join(bracket(part) for part in command)})')
        properties = [f'WORKING_DIRECTORY {bracket(str(working_directory))}', f'TIMEOUT {timeout}',
                      f'LABELS {bracket(";".join([base_label, *sorted(labels[unit.name])]))}']
        if not unit.enabled:
            properties.append('DISABLED TRUE')
        lines.append(render_properties(test_name, properties))
    return '\n'.join(lines) + '\n'


def render_listing_failure(message: str, labels: set[str], executable: Path, test_prefix: str,
                           base_label: str) -> str:
    """Render a single entry that fails with the listing error.

    It carries every known label so that selecting by contract still runs, and fails on, it.
    """
    test_name = bracket(test_prefix + DISCOVERY_FAILED_NAME)
    command = [sys.executable, '-c', 'import sys; sys.exit(sys.argv[1])', message]
    return '\n'.join([script_header(executable),
                      f'add_test({test_name} {" ".join(bracket(part) for part in command)})',
                      render_properties(test_name, [f'LABELS {bracket(";".join([base_label, *sorted(labels)]))}'])
                      ]) + '\n'


def write_if_changed(path: Path, content: str) -> None:
    """Replace the file atomically, leaving it untouched when the content is unchanged."""
    if path.is_file() and path.read_text(encoding='utf-8', errors='replace') == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f'.{path.name}.')
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
            stream.write(content)
        os.chmod(temporary, GENERATED_FILE_MODE)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def run_cli(argv: list[str] | None = None) -> int:
    """Discover the executable's top-level units and write the CTest script that registers them."""
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument('--executable', type=Path, required=True, help='Boost.Test executable to inventory')
    parser.add_argument('--working-directory', type=Path, required=True,
                        help='directory the registered tests run in')
    parser.add_argument('--test-sources', type=Path, nargs='+', required=True,
                        help="the executable's own sources, which unit locations are matched against")
    parser.add_argument('--accessor-header', type=Path, action='append', required=True,
                        help='header defining contract accessors (repeatable)')
    parser.add_argument('--implicit-accessor-header', type=Path, action='append', default=[],
                        help='header defining accessors for contracts deployed during default test setup; '
                             'they label every entry (repeatable)')
    parser.add_argument('--timeout', type=int, required=True, help='CTest TIMEOUT for every entry, in seconds')
    parser.add_argument('--output', type=Path, required=True, help='CTest script to write')
    parser.add_argument('--test-argument', action='append', default=[],
                        help='extra argument passed to every registered test (repeatable)')
    parser.add_argument('--test-prefix', default=DEFAULT_TEST_PREFIX, help='prefix of every CTest name')
    parser.add_argument('--base-label', default=DEFAULT_BASE_LABEL, help='label carried by every entry')
    args = parser.parse_args(argv)

    tool = Path(__file__).name
    executable = args.executable.resolve()
    sources = sorted(source.resolve() for source in args.test_sources)
    try:
        accessors = load_accessor_labels(args.accessor_header + args.implicit_accessor_header)
        implicit = set()
        if args.implicit_accessor_header:
            implicit = set().union(*load_accessor_labels(args.implicit_accessor_header).values())
        try:
            units = list_test_units(executable, args.working_directory)
        except ListingError as error:
            # A plain rebuild does not relink an unchanged executable, and only a relink reruns discovery.
            message = (f'{error}\nTo retry discovery after fixing the cause, touch {Path(__file__).resolve()} '
                       f'and rebuild {executable.name}.')
            print(f'{tool}: warning: {message}\nregistering a failing {args.test_prefix}{DISCOVERY_FAILED_NAME} '
                  'entry', file=sys.stderr)
            write_if_changed(args.output, render_listing_failure(
                message, every_known_label(accessors, implicit, sources), executable, args.test_prefix,
                args.base_label))
            return 0
        labels = derive_labels(units, accessors, implicit, sources)
        write_if_changed(args.output, render_ctest(units, labels, executable, args.working_directory.resolve(),
                                                   args.timeout, args.test_argument, args.test_prefix,
                                                   args.base_label))
    except (DiscoveryError, OSError, ValueError) as error:
        print(f'{tool}: error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(run_cli())
