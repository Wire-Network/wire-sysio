#!/usr/bin/env python3

import tempfile
import gzip
import shutil
import json

from TestHarness import Utils

Print=Utils.Print
testSuccessful=False

expected_results = [
    {
        "file": "unittests/snapshots/snap_v8.bin.gz",
        "result": {
            "version": 8,
            "chain_id": "526169f0107d4390d1f87e4ab46ff9da11b7f286139599d557dd6234d3855f94",
            "head_block_id": "000000037b815d696953eda2e1bd049e467b977b8b69ce1572f7bcb0b9f5003c",
            "head_block_num": 3,
            "head_block_time": "2020-01-01T00:00:01.000"
        }
    }
]

def test_success():
    for test in expected_results:
        with gzip.open(test['file'], 'rb') as compressed_snap_file:
            with tempfile.NamedTemporaryFile('wb') as uncompressed_snap_file:
                shutil.copyfileobj(compressed_snap_file, uncompressed_snap_file)
                assert(test['result'] == json.loads(Utils.processSysioUtilCmd(f"snapshot info {uncompressed_snap_file.name}", "do snap info", silentErrors=False, exitOnError=True)))

def test_failure():
    assert(None == Utils.processSysioUtilCmd("snapshot info nonexistentfile.bin", "do snap info"))

try:
    test_success()
    test_failure()

    testSuccessful=True
except Exception as e:
    Print(e)
    Utils.errorExit("exception during processing")

exitCode = 0 if testSuccessful else 1
exit(exitCode)
