import importlib.util
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
