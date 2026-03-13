#!/usr/bin/env python3

import json

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr

###############################################################
# proto_abi_test
#
# Test ABI with proto3 action serialization/deserialization.
#
# This test creates a producer node, deploys a contract with
# protobuf actions, pushes transactions with protobuf-serialized
# action data, and verifies the results.
#
###############################################################

args = TestHelper.parse_args({"-v","--dump-error-details","--leave-running","--keep-logs","--unshared"})
Utils.Debug = args.v
dumpErrorDetails=args.dump_error_details

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True)
cluster.setWalletMgr(walletMgr)

testSuccessful = False
try:
    TestHelper.printSystemInfo("BEGIN")
    assert cluster.launch(
        pnodes=1,
        prodCount=1,
        totalProducers=1,
        totalNodes=1,
        loadSystemContract=False,
        activateIF=True), "Failed to launch cluster"

    producerNode = cluster.getNode(0)

    # Create account for proto contract
    Utils.Print("Create prototest account")
    prototestAcc = Account("prototest")
    prototestAcc.ownerPublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    prototestAcc.activePublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    producerNode.createAccount(prototestAcc, cluster.sysioAccount)

    # Deploy proto_abi_test contract
    contractDir="unittests/test-contracts/proto_abi_test"
    wasmFile="proto_abi_test.wasm"
    abiFile="proto_abi_test.abi"
    Utils.Print("Publish proto_abi_test contract")
    trans = producerNode.publishContract(prototestAcc, contractDir, wasmFile, abiFile, waitForTransBlock=True)
    assert trans is not None, "Failed to publish proto_abi_test contract"

    # Test 1: Push hiproto action (protobuf input + protobuf return)
    Utils.Print("Test 1: Push hiproto action with protobuf-serialized data")
    contract="prototest"
    action="hiproto"
    data=json.dumps({"id":1,"type":2,"note":"hello"})
    opts="--permission prototest@active"
    trans=producerNode.pushMessage(contract, action, data, opts)
    assert trans[0], "Failed to push hiproto action"

    # Test 2: Push pbaction (protobuf input, void return)
    Utils.Print("Test 2: Push pbaction with protobuf-serialized data")
    action="pbaction"
    data=json.dumps({"id":5,"type":0,"note":"test"})
    trans=producerNode.pushMessage(contract, action, data, opts)
    assert trans[0], "Failed to push pbaction"

    # Test 3: Push pbaction with different data
    Utils.Print("Test 3: Push pbaction with different valid data")
    action="pbaction"
    data=json.dumps({"id":99,"type":7,"note":"another test"})
    trans=producerNode.pushMessage(contract, action, data, opts)
    assert trans[0], "Failed to push pbaction with different data"

    # Test 4: Verify pbaction rejects invalid data (id must be positive)
    Utils.Print("Test 4: Push pbaction with invalid data (id=0, should fail)")
    action="pbaction"
    data=json.dumps({"id":0,"type":0,"note":""})
    trans=producerNode.pushMessage(contract, action, data, opts, silentErrors=True)
    assert not trans[0], "Expected pbaction with id=0 to fail, but it succeeded"

    testSuccessful = True
    Utils.Print("All proto_abi_test tests passed")
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)