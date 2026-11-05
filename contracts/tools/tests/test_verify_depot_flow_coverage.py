import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "verify_depot_flow_coverage.py"
spec = importlib.util.spec_from_file_location("coverage_map", MODULE)
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class CoverageMapTests(unittest.TestCase):
    def data(self):
        return {"flows": [{"name": "flow-example", "wire_behavior": "real depot actions",
                           "cluster_only": "external execution", "tests": ["suite/case"]}]}

    def test_missing_native_case_fails(self):
        with self.assertRaisesRegex(ValueError, "missing native test"):
            coverage.validate(self.data(), {})

    def test_new_enabled_flow_requires_a_map_entry(self):
        with self.assertRaisesRegex(ValueError, "unmapped=.*flow-new"):
            coverage.validate(self.data(), {"suite/case": "tests.cpp"}, ["flow-example", "flow-new"])

    def test_disabled_flow_cannot_remain_claimed_as_enabled(self):
        with self.assertRaisesRegex(ValueError, "no longer enabled"):
            coverage.validate(self.data(), {"suite/case": "tests.cpp"}, [])

    def test_boundary_and_duplicate_flow_are_rejected(self):
        data = self.data()
        data["flows"][0]["cluster_only"] = ""
        with self.assertRaisesRegex(ValueError, "incomplete boundary"):
            coverage.validate(data, {"suite/case": "tests.cpp"})
        data = self.data()
        data["flows"] *= 2
        with self.assertRaisesRegex(ValueError, "duplicate flow"):
            coverage.validate(data, {"suite/case": "tests.cpp"})

    def test_inventory_respects_suite_boundaries_and_ignores_comments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "contracts/tests"
            source.mkdir(parents=True)
            (source / "example.cpp").write_text("""
                // BOOST_AUTO_TEST_CASE(commented)
                BOOST_AUTO_TEST_SUITE(first)
                BOOST_FIXTURE_TEST_CASE(one, fixture) {}
                BOOST_AUTO_TEST_SUITE_END()
                /* BOOST_AUTO_TEST_CASE(commented_again) */
                BOOST_AUTO_TEST_SUITE(second)
                BOOST_AUTO_TEST_CASE(two) {}
                BOOST_AUTO_TEST_SUITE_END()
            """)
            self.assertEqual(set(coverage.inventory(root)), {"first/one", "second/two"})


class CoverageMapCLITests(unittest.TestCase):
    """Exercise the actual coordinated CLI against isolated tools package manifests."""

    def run_verifier(self, *args):
        """Invoke the same verifier used by CTest and the coordinated CI job."""
        return subprocess.run([sys.executable, str(MODULE), *map(str, args)],
                              capture_output=True, text=True, check=False)

    def create_tools_checkout(self, root):
        """Create enabled manifests for the reviewed inventory without copying a repository."""
        data = json.loads(coverage.MAP.read_text())
        for flow in data["flows"]:
            package = root / "packages" / flow["name"]
            package.mkdir(parents=True)
            (package / "package.json").write_text(json.dumps({"scripts": {"test": "flow-runner"}}))

    def test_standalone_reports_consistency_without_claiming_completeness(self):
        """Standalone validation needs no tools checkout and states its narrower scope."""
        result = self.run_verifier()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("standalone consistency only; tools flow completeness not checked", result.stdout)

    def test_selected_checkout_passes_then_new_enabled_flow_fails(self):
        """A newly enabled package fails the coordinated command, not just validate()."""
        with tempfile.TemporaryDirectory(prefix="depot flow tools ") as tmp:
            root = Path(tmp)
            self.create_tools_checkout(root)
            result = self.run_verifier("--tools-root", root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("cross-repository completeness checked", result.stdout)
            package = root / "packages/flow-new"
            package.mkdir()
            manifest = package / "package.json"
            manifest.write_text(json.dumps({"scripts": {"test:unit": "unit-runner"}}))
            result = self.run_verifier("--tools-root", root)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest.write_text(json.dumps({"scripts": {"test": "flow-runner"}}))
            result = self.run_verifier("--tools-root", root)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("unmapped=['flow-new']", result.stderr)

    def test_disabled_flow_fails_coordinated_check(self):
        """Removing a flow's test script invalidates the committed enabled-flow claim."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.create_tools_checkout(root)
            manifest = next((root / "packages").glob("flow-*/package.json"))
            manifest.write_text(json.dumps({"scripts": {"test": ""}}))
            result = self.run_verifier("--tools-root", root)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(f"no longer enabled=['{manifest.parent.name}']", result.stderr)

    def test_explicit_missing_checkout_does_not_fall_back_to_standalone(self):
        """An invalid selected tools path fails instead of silently weakening the check."""
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_verifier("--tools-root", Path(tmp) / "missing")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no flow package manifests found", result.stderr)

    def test_filter_remains_a_boost_selector(self):
        """Scope messages must not corrupt the machine-readable native test filter."""
        result = self.run_verifier("--filter")
        self.assertEqual(result.returncode, 0, result.stderr)
        selected = result.stdout.strip().split(":")
        self.assertTrue(selected)
        tests = coverage.inventory(coverage.ROOT)
        self.assertTrue(all(test in tests for test in selected))


if __name__ == "__main__":
    unittest.main()
