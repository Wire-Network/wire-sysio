#!/usr/bin/env python3

import tempfile
import gzip
import os
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
            "chain_id": "087244f65e31c0106a58554b8f855e30ae657efb98c6c40348bb14db8bdb3f8e",
            "head_block_id": "0000001dd8ac497760f09f90d77e2eb78afbdfe5ff7bae0bb19b10c5b717042f",
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
                # sys-util opens this path in a separate process; flush and fsync so macOS CI cannot mmap a
                # partially-buffered snapshot and fail validation with a root hash mismatch.
                uncompressed_snap_file.flush()
                os.fsync(uncompressed_snap_file.fileno())
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
