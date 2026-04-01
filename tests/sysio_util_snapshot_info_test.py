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
        "file": "unittests/snapshots/snap_v1.bin.gz",
        "result": {
            "version": 1,
            "chain_id": "144035215e20fd016e2b4b065349c959a1070fcbb0dc3f4784f3130685e774fc",
            "head_block_id": "0000001db8ed54f83aa97ed6112a0f39568705ead8d2c50ffe4c4c28e193aab3",
            "head_block_num": 29,
            "head_block_time": "2025-01-01T00:00:14.000"
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
