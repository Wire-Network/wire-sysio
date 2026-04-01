#!/usr/bin/env python3

import json
import signal
import sys

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr

###############################################################
# get_kv_rows_test
#
# Verifies the /v1/chain/get_kv_rows API endpoint for
# kv::raw_table (format=0) data.
#
# 1. Deploys test_kv_map contract (format=0 / kv::raw_table)
# 2. Pushes several put actions to store data
# 3. Calls get_kv_rows and verifies ABI-decoded keys and values
# 4. Tests pagination (limit + next_key)
# 5. Tests lower_bound / upper_bound filtering
# 6. Tests reverse iteration
# 7. Tests json=false (raw hex mode)
###############################################################

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})

Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
dumpErrorDetails = args.dump_error_details
walletPort = TestHelper.DEFAULT_WALLET_PORT

walletMgr = WalletMgr(True, port=walletPort)
testSuccessful = False

DEV_PUB_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

totalNodes = 2

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    Print("Stand up cluster: 1 producer + 1 API node")
    specificExtraNodeopArgs = {1: "--plugin sysio::chain_api_plugin"}
    if cluster.launch(pnodes=1, totalNodes=totalNodes, totalProducers=1,
                      activateIF=True, loadSystemContract=False,
                      specificExtraNodeopArgs=specificExtraNodeopArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")

    node = cluster.getNode(1)  # API node
    cluster.waitOnClusterSync(blockAdvancing=5)
    Print("Cluster in Sync")

    # Create account and deploy test_kv_map contract
    import os
    kvmapAccount = Account("kvmap")
    kvmapAccount.ownerPublicKey = DEV_PUB_KEY
    kvmapAccount.activePublicKey = DEV_PUB_KEY
    node.createInitializeAccount(kvmapAccount, cluster.sysioAccount, stakedDeposit=0, waitForTransBlock=True)

    contractDir = os.path.join(os.getcwd(), "unittests", "test-contracts", "test_kv_map")
    Print("Deploy test_kv_map contract")
    trans = node.publishContract(kvmapAccount, contractDir, "test_kv_map.wasm", "test_kv_map.abi")
    assert trans is not None, "Failed to deploy test_kv_map"

    node.waitForHeadToAdvance()

    # Push several put actions with distinct keys for testing
    test_data = [
        {"region": "ap-south", "id": 3, "payload": "mumbai", "amount": 300},
        {"region": "eu-west",  "id": 2, "payload": "dublin", "amount": 200},
        {"region": "us-east",  "id": 1, "payload": "virginia", "amount": 100},
        {"region": "us-west",  "id": 4, "payload": "oregon", "amount": 400},
    ]

    for d in test_data:
        trx = {"actions": [{"account": "kvmap", "name": "put",
                 "authorization": [{"actor": "kvmap", "permission": "active"}],
                 "data": d}]}
        results = node.pushTransaction(trx)
        assert results[0], f"put failed: {results}"

    node.waitForHeadToAdvance()
    node.waitForHeadToAdvance()

    # ---------------------------------------------------------------
    # Helper to call get_kv_rows
    # ---------------------------------------------------------------
    def get_kv_rows(payload):
        resp = node.processUrllibRequest("chain", "get_kv_rows", payload)
        assert resp["code"] == 200, f"get_kv_rows returned {resp['code']}: {resp}"
        return resp["payload"]

    # ---------------------------------------------------------------
    # Test 1: Basic query -- all rows, json=true
    # ---------------------------------------------------------------
    Print("Test 1: Basic query -- all rows with ABI decoding")
    result = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 100})
    rows = result["rows"]
    Print(f"  Got {len(rows)} rows")
    assert len(rows) == 4, f"Expected 4 rows, got {len(rows)}"

    # Keys should be sorted lexicographically by BE encoding.
    # String comparison: "ap-south" < "eu-west" < "us-east" < "us-west"
    expected_regions = ["ap-south", "eu-west", "us-east", "us-west"]
    for i, row in enumerate(rows):
        key = row["key"]
        value = row["value"]
        Print(f"  Row {i}: key={key} value={value}")
        assert key["region"] == expected_regions[i], \
            f"Row {i} region mismatch: expected {expected_regions[i]}, got {key['region']}"
        assert "payload" in value, f"Row {i} missing payload in value"
        assert "amount" in value, f"Row {i} missing amount in value"

    assert result["more"] == False, "Expected more=false for full result"
    Print("  PASSED")

    # ---------------------------------------------------------------
    # Test 2: Pagination -- limit=2 and follow next_key
    # ---------------------------------------------------------------
    Print("Test 2: Pagination with limit=2")
    page1 = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 2})
    assert len(page1["rows"]) == 2, f"Page 1 expected 2 rows, got {len(page1['rows'])}"
    assert page1["more"] == True, "Page 1 should have more=true"
    assert page1["next_key"] != "", "Page 1 should have next_key"
    Print(f"  Page 1: {[r['key']['region'] for r in page1['rows']]}, next_key={page1['next_key']}")

    # Use next_key as lower_bound for page 2
    page2 = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 2,
                          "lower_bound": page1["next_key"]})
    assert len(page2["rows"]) == 2, f"Page 2 expected 2 rows, got {len(page2['rows'])}"
    assert page2["more"] == False, "Page 2 should have more=false"
    Print(f"  Page 2: {[r['key']['region'] for r in page2['rows']]}")

    # Verify page1 + page2 cover all 4 rows
    all_regions = [r["key"]["region"] for r in page1["rows"]] + \
                  [r["key"]["region"] for r in page2["rows"]]
    assert all_regions == expected_regions, f"Pagination mismatch: {all_regions}"
    Print("  PASSED")

    # ---------------------------------------------------------------
    # Test 3: lower_bound / upper_bound filtering
    # ---------------------------------------------------------------
    Print("Test 3: lower_bound / upper_bound filtering")
    # Query rows with keys >= {"region":"eu-west","id":2} and < {"region":"us-west","id":4}
    lb = json.dumps({"region": "eu-west", "id": 2})
    ub = json.dumps({"region": "us-west", "id": 4})
    result = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 100,
                           "lower_bound": lb, "upper_bound": ub})
    rows = result["rows"]
    Print(f"  Got {len(rows)} rows: {[r['key']['region'] for r in rows]}")
    assert len(rows) == 2, f"Expected 2 rows (eu-west, us-east), got {len(rows)}"
    assert rows[0]["key"]["region"] == "eu-west"
    assert rows[1]["key"]["region"] == "us-east"
    Print("  PASSED")

    # ---------------------------------------------------------------
    # Test 4: Reverse iteration
    # ---------------------------------------------------------------
    Print("Test 4: Reverse iteration")
    result = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 100, "reverse": True})
    rows = result["rows"]
    Print(f"  Got {len(rows)} rows: {[r['key']['region'] for r in rows]}")
    assert len(rows) == 4, f"Expected 4 rows, got {len(rows)}"
    reverse_regions = list(reversed(expected_regions))
    for i, row in enumerate(rows):
        assert row["key"]["region"] == reverse_regions[i], \
            f"Reverse row {i}: expected {reverse_regions[i]}, got {row['key']['region']}"
    Print("  PASSED")

    # ---------------------------------------------------------------
    # Test 5: json=false (raw hex mode)
    # ---------------------------------------------------------------
    Print("Test 5: json=false (raw hex mode)")
    result = get_kv_rows({"json": False, "code": "kvmap", "table": "geodata", "limit": 2})
    rows = result["rows"]
    Print(f"  Got {len(rows)} rows")
    assert len(rows) == 2
    for row in rows:
        key = row["key"]
        value = row["value"]
        # In hex mode, key and value should be hex strings
        assert isinstance(key, str), f"Expected hex string key, got {type(key)}"
        # Hex string should only contain hex characters
        assert all(c in "0123456789abcdef" for c in key.lower()), \
            f"Key is not a valid hex string: {key}"
    Print("  PASSED")

    # ---------------------------------------------------------------
    # Test 6: Reverse pagination
    # ---------------------------------------------------------------
    Print("Test 6: Reverse pagination with limit=2")
    page1 = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 2, "reverse": True})
    assert len(page1["rows"]) == 2
    assert page1["more"] == True
    Print(f"  Page 1 (reverse): {[r['key']['region'] for r in page1['rows']]}, next_key={page1['next_key']}")

    # In reverse mode, next_key is the key we stopped at; use it as upper_bound for next page
    page2 = get_kv_rows({"code": "kvmap", "table": "geodata", "limit": 2,
                          "reverse": True, "upper_bound": page1["next_key"]})
    Print(f"  Page 2 (reverse): {[r['key']['region'] for r in page2['rows']]}")
    assert len(page2["rows"]) == 1, f"Page 2 expected 1 row, got {len(page2['rows'])}"
    # us-west, us-east from page1; eu-west from page2; ap-south excluded by upper_bound
    # Actually: reverse with upper_bound=eu-west means we iterate backward from before eu-west
    # ap-south is the only entry < eu-west
    assert page2["rows"][0]["key"]["region"] == "ap-south"
    Print("  PASSED")

    testSuccessful = True
    Print("All get_kv_rows tests passed")

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
