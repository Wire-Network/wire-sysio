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
            "chain_id": "70b972fe45a55e057d342b84a059d29eba3628954f3a04164765ab0d2d48a8dd",
            "head_block_id": "0000001df8c4ce22e9505e991f002696cd1c0961706110805b4f562deba3c862",
            "head_block_num": 29,
            "head_block_time": "2020-01-01T00:00:14.000"
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
