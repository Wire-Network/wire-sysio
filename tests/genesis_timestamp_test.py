#!/usr/bin/env python3
"""Check genesis timestamp precision and UTC semantics without launching processes."""

import datetime
import json
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from TestHarness import Utils
from TestHarness.launcher import cluster_generator


UTC = datetime.timezone.utc
FIXED_NOW = datetime.datetime(2026, 10, 23, 9, 44, 37, 579083, tzinfo=UTC)
EXPECTED_TIMESTAMP = "2026-10-23T09:44:37.579"
TIME_ZONES = ("UTC", "America/New_York")
EXISTING_TIMESTAMP = "2020-01-02T03:04:05.678"


class FrozenDateTime(datetime.datetime):
    """Model both timezone-aware and local naive reads of the same fixed instant."""

    @classmethod
    def now(cls, tz=None):
        """Return the fixture instant using datetime.now's timezone semantics."""
        if tz is None:
            return FIXED_NOW.astimezone().replace(tzinfo=None)
        return FIXED_NOW.astimezone(tz)


class GenesisTimestampTest(unittest.TestCase):
    """Exercise formatting and actual genesis construction in both host timezones."""

    def in_time_zones(self, check):
        """Run a check with a fixed clock, restoring the process timezone afterwards."""
        try:
            for zone in TIME_ZONES:
                with self.subTest(zone=zone), patch.dict(os.environ, {"TZ": zone}), \
                    patch("TestHarness.testUtils.datetime", FrozenDateTime):
                    time.tzset()
                    check()
        finally:
            time.tzset()

    def test_shared_formatter(self):
        """Discard submillisecond digits without shifting the instant to local time."""
        self.in_time_zones(lambda: self.assertEqual(Utils.genesisTimestamp(), EXPECTED_TIMESTAMP))

    def test_launcher_default_timestamp(self):
        """The default genesis uses UTC milliseconds even on a non-UTC host."""
        with tempfile.TemporaryDirectory() as directory:
            launcher = cluster_generator(["--nodes", "1", "--pnodes", "1", "--producers", "1",
                                          "--genesis", str(Path(directory) / "genesis.json")])
            launcher.define_network()
            launcher.network.nodes["bios"].keys = [SimpleNamespace(pubkey="fixture-key", blspubkey="fixture-bls")]
            self.in_time_zones(lambda: self.assertEqual(
                launcher.init_genesis()["initial_timestamp"], EXPECTED_TIMESTAMP))

    def test_existing_genesis_timestamp(self):
        """An explicitly supplied genesis keeps its timestamp instead of using the clock."""
        with tempfile.TemporaryDirectory() as directory:
            genesis_path = Path(directory) / "genesis.json"
            genesis_path.write_text(json.dumps({"initial_timestamp": EXISTING_TIMESTAMP}))
            launcher = cluster_generator(["--nodes", "1", "--pnodes", "1", "--producers", "1",
                                          "--genesis", str(genesis_path)])
            launcher.define_network()
            launcher.network.nodes["bios"].keys = [SimpleNamespace(pubkey="fixture-key")]
            self.assertEqual(launcher.init_genesis()["initial_timestamp"], EXISTING_TIMESTAMP)


if __name__ == "__main__":
    unittest.main()
