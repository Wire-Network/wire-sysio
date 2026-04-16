#!/usr/bin/env python3

import json
import os
import shutil
import signal
import sys
import time

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# ship_kv_delta_test
#
# Verifies that KV table data (all tables use table_id-based storage)
# flows correctly through the SHiP delta pipeline as contract_row_kv.
#
# Topology: 1 producer, 1 API node, 1 SHiP node.
# Transactions are sent to the API node (has transaction-retry).
#
# 1. Starts cluster with state_history_plugin
# 2. Deploys both a KV contract and a multi_index contract
# 3. Pushes actions to both
# 4. Runs ship_streamer with --fetch-deltas
# 5. Verifies "contract_row_kv" delta table name appears in the
#    SHiP binary output (all KV data emits as contract_row_kv)
###############################################################

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})

Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
dumpErrorDetails = args.dump_error_details
walletPort = TestHelper.DEFAULT_WALLET_PORT

walletMgr = WalletMgr(True, port=walletPort)
testSuccessful = False
shipTempDir = None

# Default dev key — already in wallet from bootstrap
DEV_PUB_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

# Fixed topology: node 0 = producer, node 1 = API, node 2 = SHiP
prodNodeNum = 0
apiNodeNum = 1
shipNodeNum = 2
totalNodes = 3

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    Print("Stand up cluster: 1 producer, 1 API node, 1 SHiP node")
    specificExtraNodeopArgs = {}
    specificExtraNodeopArgs[apiNodeNum] = (
        "--transaction-retry-max-storage-size-gb 100 "
    )
    specificExtraNodeopArgs[shipNodeNum] = (
        "--plugin sysio::state_history_plugin "
        "--trace-history --chain-state-history "
        "--plugin sysio::net_api_plugin "
    )

    if cluster.launch(pnodes=1, totalNodes=totalNodes, totalProducers=1,
                      activateIF=True, loadSystemContract=False,
                      specificExtraNodeopArgs=specificExtraNodeopArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")

    prodNode = cluster.getNode(prodNodeNum)
    apiNode = cluster.getNode(apiNodeNum)
    shipNode = cluster.getNode(shipNodeNum)

    cluster.waitOnClusterSync(blockAdvancing=5)
    Print("Cluster in Sync")

    # Record block number before contract deployment
    startBlockNum = shipNode.getBlockNum()
    Print(f"Start block: {startBlockNum}")

    # Create accounts using dev key (already in wallet)
    kvmapAccount = Account("kvmap")
    kvmapAccount.ownerPublicKey = DEV_PUB_KEY
    kvmapAccount.activePublicKey = DEV_PUB_KEY
    apiNode.createInitializeAccount(kvmapAccount, cluster.sysioAccount, stakedDeposit=0, waitForTransBlock=True)

    miAccount = Account("mitest")
    miAccount.ownerPublicKey = DEV_PUB_KEY
    miAccount.activePublicKey = DEV_PUB_KEY
    apiNode.createInitializeAccount(miAccount, cluster.sysioAccount, stakedDeposit=0, waitForTransBlock=True)

    # Deploy test_kv_map (KV table)
    contractDir = os.path.join(os.getcwd(), "unittests", "test-contracts", "test_kv_map")
    Print("Deploy test_kv_map contract")
    trans = apiNode.publishContract(kvmapAccount, contractDir, "test_kv_map.wasm", "test_kv_map.abi")
    assert trans is not None, "Failed to deploy test_kv_map"

    # Deploy get_table_test (kv_multi_index)
    contractDir2 = os.path.join(os.getcwd(), "unittests", "test-contracts", "get_table_test")
    Print("Deploy get_table_test contract (kv_multi_index)")
    trans = apiNode.publishContract(miAccount, contractDir2, "get_table_test.wasm", "get_table_test.abi")
    assert trans is not None, "Failed to deploy get_table_test"

    apiNode.waitForHeadToAdvance()

    # Push KV store actions
    Print("Push KV store actions")
    trx1 = {"actions": [{"account": "kvmap", "name": "put",
             "authorization": [{"actor": "kvmap", "permission": "active"}],
             "data": {"region": "us-east", "id": 1, "payload": "hello", "amount": 100}}]}
    results = apiNode.pushTransaction(trx1)
    assert results[0], f"KV put failed: {results}"

    trx2 = {"actions": [{"account": "kvmap", "name": "put",
             "authorization": [{"actor": "kvmap", "permission": "active"}],
             "data": {"region": "eu-west", "id": 2, "payload": "world", "amount": 200}}]}
    results = apiNode.pushTransaction(trx2)
    assert results[0], f"KV put 2 failed: {results}"

    # Push multi_index action (format=1)
    Print("Push multi_index addnumobj action")
    trx3 = {"actions": [{"account": "mitest", "name": "addnumobj",
             "authorization": [{"actor": "mitest", "permission": "active"}],
             "data": {"input": 42}}]}
    results = apiNode.pushTransaction(trx3)
    assert results[0], f"multi_index addnumobj failed: {results}"

    apiNode.waitForHeadToAdvance()
    apiNode.waitForHeadToAdvance()

    # Verify KV data via get action
    Print("Verify KV data reads back correctly")
    trxGet = {"actions": [{"account": "kvmap", "name": "get",
              "authorization": [{"actor": "kvmap", "permission": "active"}],
              "data": {"region": "us-east", "id": 1}}]}
    results = apiNode.pushTransaction(trxGet)
    assert results[0], f"KV get failed: {results}"

    endBlockNum = shipNode.getBlockNum()
    Print(f"End block: {endBlockNum}")

    # Run ship_streamer to fetch deltas for the block range
    Print("Run ship_streamer to fetch deltas")
    shipTempDir = os.path.join(Utils.DataDir, "ship")
    os.makedirs(shipTempDir, exist_ok=True)

    outFile = os.path.join(shipTempDir, "streamer.out")
    errFile = os.path.join(shipTempDir, "streamer.err")

    # SHiP listens on default port 8080
    shipAddr = "127.0.0.1:8080"

    cmd = (f"tests/ship_streamer --socket-address {shipAddr} "
           f"--start-block-num {startBlockNum} --end-block-num {endBlockNum} "
           f"--fetch-deltas")
    if Utils.Debug:
        Utils.Print(f"cmd: {cmd}")

    with open(outFile, "w") as out, open(errFile, "w") as err:
        popen = Utils.delayedCheckOutput(cmd, stdout=out, stderr=err)
        popen.wait(timeout=60)
        retcode = popen.returncode

    if retcode != 0:
        with open(errFile, "r") as f:
            Utils.Print(f"ship_streamer stderr: {f.read()}")
        Utils.errorExit(f"ship_streamer failed with return code {retcode}")

    # Parse output and search for delta table names in the hex-encoded deltas
    Print("Verify delta content from ship_streamer output")
    with open(outFile, "r") as f:
        output = f.read()

    # The table name "contract_row_kv" appears in the binary delta data as:
    # [varuint length=15][UTF-8 "contract_row_kv"] = hex: 0f636f6e74726163745f726f775f6b76
    contract_row_kv_hex = "0f636f6e74726163745f726f775f6b76"

    found_contract_row_kv = contract_row_kv_hex in output

    Print(f"contract_row_kv delta found: {found_contract_row_kv}")

    assert found_contract_row_kv, "Expected contract_row_kv delta not found in SHiP output"

    Print("SUCCESS: contract_row_kv deltas verified in SHiP output")

    # Shutdown
    Print("Shutdown state_history_plugin nodeop")
    shipNode.kill(signal.SIGTERM)

    testSuccessful = True
    Print("All checks passed")

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)
    if shipTempDir is not None:
        if testSuccessful and not args.keep_logs:
            shutil.rmtree(shipTempDir, ignore_errors=True)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
