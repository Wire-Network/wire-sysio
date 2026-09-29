"""Tests for contract test suite discovery and contract labelling."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import re
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock


DISCOVERY_PATH = Path(__file__).parents[1] / "discover_contract_suites.py"
DISCOVERY_SPEC = importlib.util.spec_from_file_location("discover_contract_suites", DISCOVERY_PATH)
if DISCOVERY_SPEC is None or DISCOVERY_SPEC.loader is None:
    raise RuntimeError(f"Unable to load discovery script from {DISCOVERY_PATH}")
DISCOVERY = importlib.util.module_from_spec(DISCOVERY_SPEC)
sys.modules[DISCOVERY_SPEC.name] = DISCOVERY
DISCOVERY_SPEC.loader.exec_module(DISCOVERY)

REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
# A CMake bracket argument ends at the first closing bracket with the same number of `=`.
BRACKET_ARGUMENT_RE = re.compile(r"\[(=*)\[(.*?)\]\1\]", re.S)
REPOSITORY_ACCESSOR_HEADERS = [
    REPOSITORY_ROOT / "contracts" / "tests" / "contracts.hpp.in",
    REPOSITORY_ROOT / "unittests" / "test_contracts.hpp.in",
    REPOSITORY_ROOT / "libraries" / "testing" / "contracts.cpp.in",
]

ACCESSOR_HEADER = """\
struct contracts {
   static std::vector<uint8_t> reserve_wasm() { return read_wasm("${CMAKE_BINARY_DIR}/contracts/sysio.reserv/sysio.reserv.wasm"); }
   static std::vector<char>    liq_abi() { return read_abi("${CMAKE_BINARY_DIR}/contracts/sysio.liq/sysio.liq.abi"); }
   static std::vector<uint8_t> liq_wasm() { return read_wasm("${CMAKE_BINARY_DIR}/contracts/sysio.liq/sysio.liq.wasm"); }
   static std::vector<uint8_t> opreg_wasm() { return read_wasm("${CMAKE_BINARY_DIR}/contracts/sysio.opreg/sysio.opreg.wasm"); }
   static std::vector<uint8_t> token_wasm() { return read_wasm("${CMAKE_BINARY_DIR}/contracts/sysio.token/sysio.token.wasm"); }
   static std::string          dex_config_dir() { return "${CMAKE_SOURCE_DIR}/etc/config/dex"; }
   struct util {
      static std::vector<uint8_t> reject_all_wasm() { return read_wasm("${CMAKE_SOURCE_DIR}/contracts/test_contracts/reject_all.wasm"); }
   };
};
[[maybe_unused]]
static std::vector<uint8_t> blockinfo_tester_wasm()
{
   return sysio::testing::read_wasm(
      "${CMAKE_BINARY_DIR}/contracts/test_contracts/blockinfo_tester/blockinfo_tester.wasm");
}
"""

MACRO_ACCESSOR_HEADER = """\
#define MAKE_READ_WASM_ABI(CN,C, D)                                                         \\
   static std::vector<uint8_t> CN ## _wasm() {                                              \\
      return read("${CMAKE_BINARY_DIR}/" #D "/" #C "/" #C ".wasm");                         \\
   }
         MAKE_READ_WASM_ABI(sysio_msig,   sysio.msig,   contracts)
         MAKE_READ_WASM_ABI(noop,         noop,         unittests/test-contracts)
         MAKE_READ_WASM_ABI(ibc,          ibc,          unittests/test-contracts/savanna)
         MAKE_READ_WASM_ABI(fresh,        fresh,        unittests/some-new-directory)
"""

EMBEDDED_ACCESSOR_HEADER = """\
MAKE_EMBEDDED_WASM_ABI(sysio_bios,                             sysio.bios, contracts)
MAKE_EMBEDDED_WASM_ABI(sysio_roa,                              sysio.roa,  contracts)
"""


def dot_listing(root: Path) -> str:
    """Return a Boost.Test DOT listing whose units are declared in files under root.

    alpha_tests' only case depends on split_case, which beta_tests declares in beta_extra.cpp.
    """
    return textwrap.dedent(f"""\
        Random number generator seeded to 1790706793
        digraph G {{rankdir=LR;
        tu1[shape=ellipse,peripheries=2,fontname=Helvetica,color=green,label="Master Test Suite"];
        {{
        tu2[shape=Mrecord,fontname=Helvetica,color=green,label="beta_tests|{root}/beta_tests.cpp(20)"];
        tu1 -> tu2;
        {{
        tu65536[shape=Mrecord,fontname=Helvetica,color=green,label="split_case|{root}/beta_extra.cpp(7)|timeout=30"];
        tu2 -> tu65536;
        tu3[shape=Mrecord,fontname=Helvetica,color=yellow,label="nested_tests|{root}/beta_nested.cpp(3)|labels: @slow"];
        tu2 -> tu3;
        {{
        tu65537[shape=Mrecord,fontname=Helvetica,color=green,label="deep_case|{root}/beta_nested.cpp(5)"];
        tu3 -> tu65537;
        }}
        }}
        tu4[shape=Mrecord,fontname=Helvetica,color=yellow,label="alpha_tests|{root}/alpha_tests.cpp(10)"];
        tu1 -> tu4;
        {{
        tu65538[shape=Mrecord,fontname=Helvetica,color=green,label="first_case|{root}/alpha_tests.cpp(12)"];
        tu4 -> tu65538;
        tu65538 -> tu65536[color=red,style=dotted,constraint=false];
        }}
        tu65539[shape=Mrecord,fontname=Helvetica,color=green,label="loose_case|{root}/loose.cpp(1)"];
        tu1 -> tu65539;
        }}
        }}
        """)


class TemporaryTreeTestCase(unittest.TestCase):
    """Provide a temporary directory and a helper that writes files into it."""

    def setUp(self) -> None:
        """Create the temporary tree."""
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name).resolve()

    def tearDown(self) -> None:
        """Remove the temporary tree."""
        self.temporary_directory.cleanup()

    def write(self, relative: str, content: str | bytes) -> Path:
        """Write content to a file under the temporary tree and return its path."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")
        return path


class ParseDotTests(unittest.TestCase):
    """Verify the Boost.Test DOT inventory parser."""

    def test_top_level_units_carry_their_subtrees_and_dependencies_files(self) -> None:
        """Only the master suite's children become units, each with the files of its subtree and dependencies."""
        units = DISCOVERY.parse_dot(dot_listing(Path("/src")))
        self.assertEqual([unit.name for unit in units], ["alpha_tests", "beta_tests", "loose_case"])
        by_name = {unit.name: unit for unit in units}
        self.assertEqual(by_name["beta_tests"].files,
                         {"/src/beta_tests.cpp", "/src/beta_extra.cpp", "/src/beta_nested.cpp"})
        self.assertEqual(by_name["alpha_tests"].files, {"/src/alpha_tests.cpp", "/src/beta_extra.cpp"})
        self.assertEqual(by_name["loose_case"].files, {"/src/loose.cpp"})

    def test_dependency_on_the_master_suite_brings_in_every_file(self) -> None:
        """Depending on the master suite runs the whole executable, so every declared file counts."""
        listing = dot_listing(Path("/src")).replace(
            "tu1 -> tu65539;\n", "tu1 -> tu65539;\ntu65539 -> tu1[color=red,style=dotted,constraint=false];\n")
        by_name = {unit.name: unit for unit in DISCOVERY.parse_dot(listing)}
        self.assertEqual(by_name["loose_case"].files, {"/src/alpha_tests.cpp", "/src/beta_tests.cpp",
                                                        "/src/beta_extra.cpp", "/src/beta_nested.cpp", "/src/loose.cpp"})

    def test_default_status_follows_the_unit_color(self) -> None:
        """A unit Boost reports as disabled by default (yellow) is not enabled."""
        by_name = {unit.name: unit for unit in DISCOVERY.parse_dot(dot_listing(Path("/src")))}
        self.assertFalse(by_name["alpha_tests"].enabled)
        self.assertTrue(by_name["beta_tests"].enabled)
        self.assertTrue(by_name["loose_case"].enabled)

    def test_missing_master_suite_is_an_error(self) -> None:
        """A listing without the master suite is rejected rather than read as empty."""
        with self.assertRaises(DISCOVERY.DiscoveryError):
            DISCOVERY.parse_dot("digraph G {rankdir=LR;\n}\n")

    def test_unit_without_source_location_is_an_error(self) -> None:
        """Every unit must name its declaring file, which labelling depends on."""
        listing = ('tu1[shape=ellipse,peripheries=2,fontname=Helvetica,color=green,label="Master Test Suite"];\n'
                   'tu2[shape=Mrecord,fontname=Helvetica,color=green,label="alpha_tests"];\n'
                   'tu1 -> tu2;\n')
        with self.assertRaises(DISCOVERY.DiscoveryError):
            DISCOVERY.parse_dot(listing)

    def test_edge_to_undeclared_unit_is_an_error(self) -> None:
        """A parent edge must point at a declared unit."""
        listing = ('tu1[shape=ellipse,peripheries=2,fontname=Helvetica,color=green,label="Master Test Suite"];\n'
                   'tu1 -> tu9;\n')
        with self.assertRaises(DISCOVERY.DiscoveryError):
            DISCOVERY.parse_dot(listing)


class AccessorLabelTests(TemporaryTreeTestCase):
    """Verify the accessor-name to contract-label map."""

    def test_function_and_macro_accessors_map_to_contract_labels(self) -> None:
        """Explicit functions resolve through their artifact paths; macro invocations through the contract they name."""
        headers = [self.write("contracts.hpp.in", ACCESSOR_HEADER),
                   self.write("test_contracts.hpp.in", MACRO_ACCESSOR_HEADER),
                   self.write("contracts.cpp.in", EMBEDDED_ACCESSOR_HEADER)]
        accessors = DISCOVERY.load_accessor_labels(headers)
        self.assertEqual(accessors["reserve_wasm"], {"sysio.reserv"})
        self.assertEqual(accessors["liq_abi"], {"sysio.liq"})
        self.assertEqual(accessors["reject_all_wasm"], {"test.reject_all"})
        self.assertEqual(accessors["blockinfo_tester_wasm"], {"test.blockinfo_tester"})
        self.assertEqual(accessors["sysio_msig_wasm"], {"sysio.msig"})
        self.assertEqual(accessors["sysio_msig_abi"], {"sysio.msig"})
        self.assertEqual(accessors["noop_wasm"], {"test.noop"})
        self.assertEqual(accessors["ibc_wasm"], {"test.ibc"})
        self.assertEqual(accessors["fresh_abi"], {"test.fresh"})
        self.assertEqual(accessors["sysio_roa_abi"], {"sysio.roa"})
        self.assertNotIn("CN_wasm", accessors)
        self.assertNotIn("dex_config_dir", accessors)

    def test_accessor_definitions_in_any_function_form_are_mapped(self) -> None:
        """Parameters, specifiers and trailing return types do not hide a definition from the map."""
        header = self.write("contracts.hpp.in", textwrap.dedent("""\
            static std::vector<uint8_t> foo_wasm() noexcept { return read_wasm("/b/contracts/sysio.foo/sysio.foo.wasm"); }
            static auto bar_wasm() -> std::vector<uint8_t> { return read_wasm("/b/contracts/sysio.bar/sysio.bar.wasm"); }
            static std::vector<uint8_t> baz_wasm(bool v2) { return read_wasm("/b/contracts/sysio.baz/sysio.baz.wasm"); }
            """))
        accessors = DISCOVERY.load_accessor_labels([header])
        self.assertEqual(accessors, {"foo_wasm": {"sysio.foo"}, "bar_wasm": {"sysio.bar"}, "baz_wasm": {"sysio.baz"}})

    def test_accessor_without_a_contract_path_is_an_error(self) -> None:
        """An accessor the label rules cannot map fails discovery instead of silently dropping a label."""
        header = self.write("contracts.hpp.in",
                            'static auto fixture_wasm() noexcept -> bytes { return read_wasm("/data/fixture.wasm"); }\n')
        with self.assertRaisesRegex(DISCOVERY.DiscoveryError, "fixture_wasm"):
            DISCOVERY.load_accessor_labels([header])

    def test_accessor_the_patterns_cannot_parse_is_an_error(self) -> None:
        """Every `*_wasm(` in an accessor header other than a loader call must map, whatever its parameter list."""
        header = self.write("contracts.hpp.in", textwrap.dedent("""\
            static std::vector<uint8_t> token_wasm() { return read_wasm("/b/contracts/sysio.token/sysio.token.wasm"); }
            static std::vector<uint8_t> qux_wasm(std::function<void()> cb) { return read_wasm("/b/contracts/sysio.qux/sysio.qux.wasm"); }
            """))
        with self.assertRaisesRegex(DISCOVERY.DiscoveryError, "qux_wasm"):
            DISCOVERY.load_accessor_labels([header])

    def test_accessor_names_in_comments_are_ignored(self) -> None:
        """A comment naming tester calls such as set_abi() is documentation, not an accessor to map."""
        header = self.write("contracts.hpp.in", "/// Hand any *_abi() result to set_abi() via .data().\n" + ACCESSOR_HEADER)
        self.assertEqual(DISCOVERY.load_accessor_labels([header])["liq_abi"], {"sysio.liq"})

    def test_header_without_accessors_is_an_error(self) -> None:
        """Pointing discovery at the wrong header fails instead of producing unlabelled entries."""
        with self.assertRaises(DISCOVERY.DiscoveryError):
            DISCOVERY.load_accessor_labels([self.write("empty.hpp.in", "#pragma once\n")])

    def test_repository_accessor_headers_map_every_accessor(self) -> None:
        """The real accessor headers parse, and every accessor they define maps to its contract."""
        accessors = DISCOVERY.load_accessor_labels(REPOSITORY_ACCESSOR_HEADERS)
        self.assertEqual(accessors["reserve_wasm"], {"sysio.reserv"})
        self.assertEqual(accessors["system_abi"], {"sysio.system"})
        self.assertEqual(accessors["badtoken_wasm"], {"test.badtoken"})
        self.assertEqual(accessors["reenter_deposit_wasm"], {"test.reenter_deposit"})
        self.assertEqual(accessors["sendinline_wasm"], {"test.sendinline"})
        self.assertEqual(accessors["sysio_system_wasm"], {"sysio.system"})
        self.assertEqual(accessors["noop_wasm"], {"test.noop"})
        self.assertEqual(accessors["ibc_wasm"], {"test.ibc"})
        self.assertEqual(accessors["sysio_bios_wasm"], {"sysio.bios"})
        self.assertEqual(accessors["reject_all_wasm"], {"test.reject_all"})


class StripCommentsTests(unittest.TestCase):
    """Verify comment removal keeps code, literals, and line numbering."""

    def test_comments_are_removed_and_lines_preserved(self) -> None:
        """Line and block comments disappear; the newline count is unchanged."""
        text = "a(); // contracts::liq_wasm()\n/* contracts::token_wasm()\n */ b();\n"
        stripped = DISCOVERY.strip_comments(text)
        self.assertNotIn("liq_wasm", stripped)
        self.assertNotIn("token_wasm", stripped)
        self.assertIn("a();", stripped)
        self.assertIn("b();", stripped)
        self.assertEqual(stripped.count("\n"), text.count("\n"))

    def test_comment_markers_inside_literals_are_code(self) -> None:
        """`//` and `/*` inside string, raw-string, and character literals do not start comments."""
        text = ('auto u = "http://x"; contracts::liq_wasm();\n'
                'auto r = R"x(/* not a comment )x"; contracts::token_wasm();\n'
                "char c = '/'; contracts::opreg_wasm();\n")
        stripped = DISCOVERY.strip_comments(text)
        for accessor in ("liq_wasm", "token_wasm", "opreg_wasm"):
            self.assertIn(accessor, stripped)

    def test_digit_separator_does_not_open_a_character_literal(self) -> None:
        """`2'000'000` leaves the trailing comment recognisable as a comment."""
        stripped = DISCOVERY.strip_comments("f(int64_t(2'000'000)); // contracts::liq_wasm()\ng();\n")
        self.assertNotIn("liq_wasm", stripped)
        self.assertIn("g();", stripped)


class ScanLabelsTests(TemporaryTreeTestCase):
    """Verify contract labels are derived from test sources and their local headers."""

    def setUp(self) -> None:
        """Write the accessor header every scan resolves names against."""
        super().setUp()
        self.accessors = DISCOVERY.load_accessor_labels([self.write("contracts.hpp.in", ACCESSOR_HEADER)])

    def test_accessor_calls_and_qualified_references_label_the_suite(self) -> None:
        """Calls and qualified references count; unknown `*_wasm` helpers and comments do not."""
        source = self.write("suite.cpp", textwrap.dedent("""\
            void deploy() {
               set_code(LIQ, contracts::liq_wasm());
               deploy_with(&contracts::reserve_wasm);
               set_code(X, notify_system_account_wasm());
               // set_code(T, contracts::token_wasm());
            }
            """))
        self.assertEqual(DISCOVERY.scan_labels({source}, self.accessors), {"sysio.liq", "sysio.reserv"})

    def test_contract_header_includes_label_the_suite(self) -> None:
        """Contract headers compiled into the test and test-contract headers count, however they are reached."""
        source = self.write("tests/suite.cpp", textwrap.dedent("""\
            #include <sysio.opp.common/amm_math.hpp>
            #include "../sysio.councl/include/sysio.councl/council_math.hpp"
            #include "../../contracts/sysio.roa/include/sysio.roa/roa.hpp"
            #include <test-contracts/noop/noop.hpp>
            #include "../test_contracts/blockinfo_tester/blockinfo_tester.hpp"
            #include <test_contracts/badtoken/badtoken.hpp>
            #include <test_contracts.hpp>
            #include <sysio/opp/opp.hpp>
            #include "contracts.hpp"
            """))
        self.assertEqual(DISCOVERY.scan_labels({source}, self.accessors),
                         {"sysio.opp.common", "sysio.councl", "sysio.roa", "test.blockinfo_tester", "test.badtoken",
                          "test.noop"})

    def test_local_headers_are_followed_transitively_and_cycles_terminate(self) -> None:
        """A deploy helper two headers away still labels the suite, and an include cycle ends."""
        self.write("tester.hpp", '#include "support.hpp"\nvoid deploy_opreg_once() { contracts::opreg_wasm(); }\n')
        self.write("support.hpp", '#include "tester.hpp"\nvoid reject() { contracts::util::reject_all_wasm(); }\n')
        source = self.write("suite.cpp", '#include "tester.hpp"\n')
        self.assertEqual(DISCOVERY.scan_labels({source}, self.accessors), {"sysio.opreg", "test.reject_all"})

    def test_undecodable_bytes_do_not_stop_the_scan(self) -> None:
        """A source with bytes that are not UTF-8 is still scanned."""
        source = self.write("suite.cpp", b"// \xff\xfe latin-1 comment\nvoid f() { contracts::liq_wasm(); }\n")
        self.assertEqual(DISCOVERY.scan_labels({source}, self.accessors), {"sysio.liq"})

    def test_repository_suite_reaches_opreg_through_its_tester_header(self) -> None:
        """getpeerkeys_tests deploys sysio.opreg only through sysio.system_tester.hpp's deploy_opreg_once."""
        accessors = DISCOVERY.load_accessor_labels(REPOSITORY_ACCESSOR_HEADERS)
        source = REPOSITORY_ROOT / "contracts" / "tests" / "getpeerkeys_tests.cpp"
        self.assertIn("sysio.opreg", DISCOVERY.scan_labels({source}, accessors))


class RenderTests(TemporaryTreeTestCase):
    """Verify the generated CTest script."""

    def test_entries_are_ordered_labelled_and_disabled_as_declared(self) -> None:
        """Each unit becomes add_test plus properties; a default-disabled unit is registered DISABLED."""
        units = [DISCOVERY.TestUnit("alpha_tests", False, {"/src/a.cpp"}),
                 DISCOVERY.TestUnit("beta_tests", True, {"/src/b.cpp"})]
        labels = {"alpha_tests": set(), "beta_tests": {"sysio.token", "sysio.liq"}}
        script = DISCOVERY.render_ctest(units, labels, Path("/build/contracts_unit_test"), Path("/build"), 2700,
                                        ["--report_level=detailed"], "contract.", "contract")
        self.assertEqual(script.splitlines()[1:], [
            "add_test([[contract.alpha_tests]] [[/build/contracts_unit_test]] [[--run_test=alpha_tests]] "
            "[[--report_level=detailed]])",
            "set_tests_properties([[contract.alpha_tests]] PROPERTIES WORKING_DIRECTORY [[/build]] TIMEOUT 2700 "
            "LABELS [[contract]] DISABLED TRUE)",
            "add_test([[contract.beta_tests]] [[/build/contracts_unit_test]] [[--run_test=beta_tests]] "
            "[[--report_level=detailed]])",
            "set_tests_properties([[contract.beta_tests]] PROPERTIES WORKING_DIRECTORY [[/build]] TIMEOUT 2700 "
            "LABELS [[contract;sysio.liq;sysio.token]])",
        ])

    def test_bracket_quoting_outgrows_its_content(self) -> None:
        """A value containing, or ending in the start of, a closing bracket gets a longer bracket."""
        self.assertEqual(DISCOVERY.bracket("plain"), "[[plain]]")
        self.assertEqual(DISCOVERY.bracket("a]]b"), "[=[a]]b]=]")
        self.assertEqual(DISCOVERY.bracket("a]]b]=]c"), "[==[a]]b]=]c]==]")
        self.assertEqual(DISCOVERY.bracket("a]"), "[=[a]]=]")
        self.assertEqual(DISCOVERY.bracket("a]="), "[[a]=]]")
        self.assertEqual(DISCOVERY.bracket("b]=]"), "[==[b]=]]==]")

    def test_unchanged_script_is_not_rewritten(self) -> None:
        """Identical content leaves the file (and its timestamp) alone; new content is readable by others."""
        output = self.root / "suites.cmake"
        DISCOVERY.write_if_changed(output, "first\n")
        self.assertEqual(stat.S_IMODE(output.stat().st_mode), DISCOVERY.GENERATED_FILE_MODE)
        os.utime(output, (1, 1))
        DISCOVERY.write_if_changed(output, "first\n")
        self.assertEqual(output.stat().st_mtime, 1)
        DISCOVERY.write_if_changed(output, "second\n")
        self.assertEqual(output.read_text(encoding="utf-8"), "second\n")

    def test_failed_write_leaves_no_temporary_file(self) -> None:
        """If the final rename fails, the previous script stays and no temporary file is left behind."""
        output = self.root / "suites.cmake"
        DISCOVERY.write_if_changed(output, "first\n")
        with mock.patch.object(DISCOVERY.os, "replace", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                DISCOVERY.write_if_changed(output, "second\n")
        self.assertEqual([path.name for path in self.root.iterdir()], ["suites.cmake"])
        self.assertEqual(output.read_text(encoding="utf-8"), "first\n")


class CommandLineTests(TemporaryTreeTestCase):
    """Run the script end to end against a stand-in test executable."""

    def make_executable(self, listing: str, exit_code: int = 0) -> Path:
        """Write a stand-in executable that prints the listing the way Boost does, to its report stream."""
        self.write("listing.dot", listing)
        expected_arguments = " ".join(DISCOVERY.LIST_CONTENT_ARGUMENTS)
        executable = self.write("build/contracts_unit_test", textwrap.dedent(f"""\
            #!/bin/sh
            [ "$*" = "{expected_arguments}" ] || exit 64
            echo "Random number generator seeded to 1"
            cat "{self.root / 'listing.dot'}" >&2
            exit {exit_code}
            """))
        executable.chmod(0o755)
        return executable

    def run_cli(self, executable: Path, *extra: str, sources: list[str] | None = None) -> tuple[int, str]:
        """Invoke the command-line entry point with the arguments CMake passes; return its code and stderr."""
        if sources is None:
            sources = [str(path) for path in sorted(self.root.glob("*.cpp"))]
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            code = DISCOVERY.run_cli([
                "--executable", str(executable), "--working-directory", str(self.root / "build"),
                "--test-sources", *sources, "--accessor-header", str(self.root / "contracts.hpp.in"),
                "--timeout", "2700", "--test-argument=--report_level=detailed",
                "--output", str(self.root / "suites.cmake"), *extra])
        return code, errors.getvalue()

    def script(self) -> str:
        """The generated CTest script."""
        return (self.root / "suites.cmake").read_text(encoding="utf-8")

    def write_sources(self) -> None:
        """Write the sources the stand-in listing declares its units in."""
        self.write("contracts.hpp.in", ACCESSOR_HEADER)
        self.write("beta_tests.cpp", "void f() { contracts::liq_wasm(); }\n")
        self.write("beta_extra.cpp", "void g() { contracts::token_wasm(); }\n")
        self.write("beta_nested.cpp", "#include <sysio.opp.common/twap.hpp>\n")
        self.write("alpha_tests.cpp", "void h() {}\n")
        self.write("loose.cpp", "void i() { contracts::opreg_wasm(); }\n")

    def test_every_top_level_unit_is_registered_with_its_labels(self) -> None:
        """Suites, their split and nested parts, dependencies, and a file-scope case are registered and labelled."""
        self.write_sources()
        self.assertEqual(self.run_cli(self.make_executable(dot_listing(self.root))), (0, ""))
        script = self.script()
        self.assertIn("LABELS [[contract;sysio.token]] DISABLED TRUE", script)
        self.assertIn("LABELS [[contract;sysio.liq;sysio.opp.common;sysio.token]])", script)
        self.assertIn("add_test([[contract.loose_case]]", script)
        self.assertIn("LABELS [[contract;sysio.opreg]])", script)
        self.assertEqual(script.count("add_test("), 3)

    def test_contracts_deployed_by_default_test_setup_label_every_entry(self) -> None:
        """Every entry carries the contracts from the implicit accessor header."""
        self.write_sources()
        self.write("contracts.cpp.in", EMBEDDED_ACCESSOR_HEADER)
        code, _ = self.run_cli(self.make_executable(dot_listing(self.root)),
                               "--implicit-accessor-header", str(self.root / "contracts.cpp.in"))
        self.assertEqual(code, 0)
        label_lines = [line for line in self.script().splitlines() if line.startswith("set_tests_properties(")]
        self.assertEqual(len(label_lines), 3)
        for line in label_lines:
            self.assertIn(";sysio.bios;", line)
            self.assertIn(";sysio.roa", line)

    def test_rewritten_locations_match_the_executables_sources(self) -> None:
        """Relative (ccache base_dir) and prefix-mapped locations resolve to the sources that were compiled."""
        self.write_sources()
        for root in (Path(".."), Path("../../.."), Path("/usr/src/wire-sysio/contracts/tests")):
            self.assertEqual(self.run_cli(self.make_executable(dot_listing(root))), (0, ""), root)
            self.assertIn("LABELS [[contract;sysio.liq;sysio.opp.common;sysio.token]])", self.script())

    def test_unresolvable_location_gets_every_label_and_a_warning(self) -> None:
        """A unit whose sources cannot be found is over-labelled, not dropped, and the build goes on."""
        self.write_sources()
        (self.root / "loose.cpp").unlink()
        code, errors = self.run_cli(self.make_executable(dot_listing(self.root)))
        self.assertEqual(code, 0)
        self.assertIn("cannot find", errors)
        loose = next(line for line in self.script().splitlines()
                     if line.startswith("set_tests_properties([[contract.loose_case]]"))
        for label in ("sysio.liq", "sysio.opp.common", "sysio.opreg", "sysio.reserv", "sysio.token", "test.reject_all"):
            self.assertIn(label, loose)

    def test_fallback_labels_come_from_every_source_when_nothing_resolves(self) -> None:
        """With no location resolvable, a contract reached only through an include still labels every unit."""
        self.write("contracts.hpp.in", ACCESSOR_HEADER)
        other = self.write("other_tests.cpp", "#include <sysio.opp.common/twap.hpp>\n")
        code, errors = self.run_cli(self.make_executable(dot_listing(Path("/nonexistent"))), sources=[str(other)])
        self.assertEqual(code, 0)
        self.assertEqual(errors.count("warning: cannot find"), 3)
        for line in self.script().splitlines():
            if line.startswith("set_tests_properties("):
                self.assertIn(";sysio.opp.common;", line)

    def test_failing_listing_registers_a_failing_entry(self) -> None:
        """A test executable that cannot list itself becomes one failing entry carrying its error; the build goes on."""
        self.write_sources()
        code, errors = self.run_cli(self.make_executable(dot_listing(self.root), exit_code=3))
        self.assertEqual(code, 0)
        self.assertIn("exited with 3", errors)
        script = self.script()
        self.assertEqual(script.count("add_test("), 1)
        add_test = script[script.index("add_test("):script.index("set_tests_properties(")]
        name, *command = [match.group(2) for match in BRACKET_ARGUMENT_RE.finditer(add_test)]
        self.assertEqual(name, "contract.DISCOVERY_FAILED")
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("exited with 3", result.stderr)
        self.assertIn("To retry discovery", result.stderr)
        labels = script[script.index("LABELS"):]
        for label in ("contract;", "sysio.liq", "sysio.opp.common", "sysio.opreg", "test.reject_all"):
            self.assertIn(label, labels)

    def test_malformed_listing_fails_discovery(self) -> None:
        """A listing discovery cannot parse is a fault in discovery, so the build fails."""
        self.write_sources()
        code, errors = self.run_cli(self.make_executable("digraph G {rankdir=LR;\n}\n"))
        self.assertEqual(code, 1)
        self.assertIn("no master test suite", errors)
        self.assertFalse((self.root / "suites.cmake").exists())


if __name__ == "__main__":
    unittest.main()
