#!/usr/bin/env python3

import json

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

###############################################################
# producer_rank_test
#
# Test rank-based producer scheduling and finalizer key management
# through the system contract's update_ranked_producers mechanism.
#
# Flow:
# 1. Launch 5-node cluster with system contract (bootstrap schedule has 5 producers)
# 2. Record initial producer schedule (5 producers from bootstrap)
# 3. Register all 5 producers via regproducer
# 4. Rank only 4 via setrank (leave one unranked at UINT32_MAX)
# 5. Register BLS finalizer keys for the 4 ranked producers
# 6. Wait for update_ranked_producers to fire via onblock
# 7. Verify producer schedule changed (5 → 4 producers, version increased)
# 8. Verify finalizer policy set (4 finalizers from ranked producers)
# 9. Test setrank action (positive and negative cases)
#
###############################################################

Print = Utils.Print
errorExit = Utils.errorExit

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
pnodes = 5
totalNodes = pnodes
rankedCount = 4  # only rank 4 of the 5 producers
dumpErrorDetails = args.dump_error_details

testSuccessful = False

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    Print("Stand up cluster")
    # totalProducers=pnodes ensures 5 producers (1 per node).
    # Bootstrap calls setprodkeys which sets producer schedule with all 5.
    if cluster.launch(pnodes=pnodes, totalNodes=totalNodes, totalProducers=pnodes) is False:
        errorExit("Failed to stand up cluster.")

    Print("Validating system accounts after bootstrap")
    cluster.validateAccounts(None)

    node0 = cluster.getNode(0)
    biosNode = cluster.biosNode

    assert biosNode.waitForLibToAdvance(), "Lib should advance after launch"

    # Collect producer info
    producers = {}
    for i in range(pnodes):
        n = cluster.getNode(i)
        producers[n.producerName] = n
    prodNames = sorted(producers.keys())
    Print(f"Producers: {prodNames}")

    # The first rankedCount producers (alphabetically) will be ranked;
    # the last one will remain unranked and should be excluded from the
    # schedule and finalizer policy after update_ranked_producers fires.
    rankedProdNames = prodNames[:rankedCount]
    excludedProd = prodNames[rankedCount]
    Print(f"Ranked producers: {rankedProdNames}")
    Print(f"Excluded (unranked) producer: {excludedProd}")

    # ----------------------------------------------------------------
    # Record initial producer schedule from bootstrap
    # ----------------------------------------------------------------
    Print("=== Recording initial producer schedule ===")
    initSchedule = node0.processUrllibRequest("chain", "get_producer_schedule")
    initActiveSchedule = initSchedule["payload"]["active"]
    initVersion = initActiveSchedule["version"]
    initProducers = sorted([p["producer_name"] for p in initActiveSchedule["producers"]])
    Print(f"Initial schedule version: {initVersion}, producers: {initProducers}")
    assert len(initProducers) == pnodes, \
        f"Expected {pnodes} producers in initial schedule, got {len(initProducers)}"

    # Import producer keys into wallet so we can sign regproducer/regfinkey transactions
    ignWallet = walletMgr.create("ignition")  # returns existing wallet created during bootstrap
    for name in prodNames:
        account = cluster.defProducerAccounts[name]
        walletMgr.importKey(account, ignWallet, ignoreDupKeyWarning=True)

    # ----------------------------------------------------------------
    # Phase 1: Register producers in system contract's _producers table
    # ----------------------------------------------------------------
    Print("=== Phase 1: Register producers via regproducer ===")
    for name in prodNames:
        account = cluster.defProducerAccounts[name]
        data = json.dumps({
            "producer": name,
            "producer_key": account.activePublicKey,
            "url": "",
            "location": 0
        })
        opts = f"--permission {name}@active"
        trans = node0.pushMessage("sysio", "regproducer", data, opts)
        assert trans is not None and trans[0], f"Failed to register producer {name}: {trans}"
        Print(f"Registered producer {name}")

    assert node0.waitForHeadToAdvance(blocksToAdvance=2), "Head should advance after regproducer"

    # ----------------------------------------------------------------
    # Phase 2: Assign ranks to only rankedCount producers via setrank
    # ----------------------------------------------------------------
    # regproducer creates entries with rank=UINT32_MAX (unranked).
    # update_ranked_producers only considers producers with rank <= 21.
    # We intentionally leave the last producer unranked to verify that
    # update_ranked_producers changes the schedule from 5 to 4.
    Print("=== Phase 2: Assign producer ranks via setrank ===")
    for i, name in enumerate(rankedProdNames):
        rank = i + 1
        data = json.dumps({"producer": name, "rank": rank})
        opts = "--permission sysio@active"
        trans = node0.pushMessage("sysio", "setrank", data, opts)
        assert trans is not None and trans[0], f"Failed to set rank for {name}: {trans}"
        Print(f"Set rank {rank} for producer {name}")

    assert node0.waitForHeadToAdvance(blocksToAdvance=2), "Head should advance after setrank"

    # ----------------------------------------------------------------
    # Phase 3: Register BLS finalizer keys for ranked producers only
    # ----------------------------------------------------------------
    Print("=== Phase 3: Register finalizer keys via regfinkey ===")
    for name in rankedProdNames:
        n = producers[name]
        blsKey = n.keys[0].blspubkey
        blsPop = n.keys[0].blspop
        data = json.dumps({
            "finalizer_name": name,
            "finalizer_key": blsKey,
            "proof_of_possession": blsPop
        })
        opts = f"--permission {name}@active"
        trans = node0.pushMessage("sysio", "regfinkey", data, opts)
        assert trans is not None and trans[0], f"Failed to register finalizer key for {name}: {trans}"
        Print(f"Registered finalizer key for {name}")

    assert node0.waitForHeadToAdvance(blocksToAdvance=2), "Head should advance after regfinkey"

    # ----------------------------------------------------------------
    # Phase 4: Wait for update_ranked_producers to fire
    # ----------------------------------------------------------------
    # onblock calls update_ranked_producers when timestamp.slot - last_update.slot > 120.
    # After system contract init, last_producer_schedule_update is 0, so the first
    # update_ranked_producers fires on the very first onblock. Since that happens before
    # we register keys, it finds no qualified producers and sets last_update to current time.
    # We need to wait ~120 more slots (60 seconds) for the next cycle.
    Print("=== Phase 4: Waiting for update_ranked_producers cycle (~65 seconds) ===")
    assert node0.waitForHeadToAdvance(blocksToAdvance=135, timeout=90), \
        "Head should advance 135 blocks for update_ranked_producers cycle"

    # Wait additional blocks for pending schedule/policy to become active
    Print("Waiting for pending policies to take effect...")
    assert node0.waitForHeadToAdvance(blocksToAdvance=60, timeout=45), \
        "Head should advance 60 more blocks for policy activation"

    # Ensure LIB is still advancing (finality working)
    assert node0.waitForLibToAdvance(timeout=30), "LIB should still be advancing"

    # ----------------------------------------------------------------
    # Phase 5: Verify producer schedule changed
    # ----------------------------------------------------------------
    Print("=== Phase 5: Verify producer schedule changed ===")
    schedule = node0.processUrllibRequest("chain", "get_producer_schedule")

    activeSchedule = schedule["payload"]["active"]
    newVersion = activeSchedule["version"]
    activeProducers = sorted([p["producer_name"] for p in activeSchedule["producers"]])
    Print(f"New schedule version: {newVersion}, producers: {activeProducers}")

    # Schedule version should have increased
    assert newVersion > initVersion, \
        f"Schedule version should have increased from {initVersion}, got {newVersion}"

    # Should have exactly rankedCount producers
    assert len(activeProducers) == rankedCount, \
        f"Expected {rankedCount} active producers, got {len(activeProducers)}: {activeProducers}"

    # All ranked producers should be in the schedule
    for name in rankedProdNames:
        assert name in activeProducers, f"Ranked producer {name} should be in active schedule"

    # The excluded producer should NOT be in the schedule
    assert excludedProd not in activeProducers, \
        f"Unranked producer {excludedProd} should NOT be in active schedule"
    Print(f"Producer schedule changed: {pnodes} -> {rankedCount} producers, "
          f"version {initVersion} -> {newVersion}")

    # ----------------------------------------------------------------
    # Phase 6: Verify finalizer policy
    # ----------------------------------------------------------------
    Print("=== Phase 6: Verify finalizer policy ===")
    finInfo = node0.getFinalizerInfo()

    activeFP = finInfo["payload"]["active_finalizer_policy"]
    activeFinCount = len(activeFP.get("finalizers", []))
    Print(f"Active finalizer policy: generation={activeFP.get('generation', 'N/A')}, "
          f"threshold={activeFP.get('threshold', 'N/A')}, finalizers={activeFinCount}")

    pendingFP = finInfo["payload"].get("pending_finalizer_policy")
    pendingFinCount = len(pendingFP.get("finalizers", [])) if pendingFP else 0
    if pendingFP:
        Print(f"Pending finalizer policy: generation={pendingFP.get('generation', 'N/A')}, "
              f"threshold={pendingFP.get('threshold', 'N/A')}, finalizers={pendingFinCount}")

    # Look for the rankedCount-finalizer policy in either active or pending.
    policyToCheck = None
    policyState = None
    if activeFinCount == rankedCount:
        policyToCheck = activeFP
        policyState = "active"
    elif pendingFinCount == rankedCount:
        policyToCheck = pendingFP
        policyState = "pending"

    assert policyToCheck is not None, \
        f"Expected a finalizer policy with {rankedCount} finalizers. " \
        f"Active has {activeFinCount}, Pending has {pendingFinCount}"
    Print(f"System contract finalizer policy is {policyState}")

    # Verify threshold: (N * 2) / 3 + 1
    expectedThreshold = rankedCount * 2 // 3 + 1
    assert policyToCheck["threshold"] == expectedThreshold, \
        f"Expected threshold {expectedThreshold}, got {policyToCheck['threshold']}"

    # Verify each ranked producer has a finalizer entry with weight 1
    finalizerDescs = sorted([f["description"] for f in policyToCheck["finalizers"]])
    Print(f"Finalizer descriptions: {finalizerDescs}")
    for name in rankedProdNames:
        assert name in finalizerDescs, f"Producer {name} should be in finalizer policy"

    # The excluded producer should NOT be a finalizer
    assert excludedProd not in finalizerDescs, \
        f"Unranked producer {excludedProd} should NOT be in finalizer policy"

    for f in policyToCheck["finalizers"]:
        assert f["weight"] == 1, f"Finalizer weight should be 1, got {f['weight']}"

    # ----------------------------------------------------------------
    # Phase 7: Test setrank action
    # ----------------------------------------------------------------
    Print("=== Phase 7: Test setrank action ===")

    # setrank requires sysio@active authority
    target = rankedProdNames[0]
    data = json.dumps({"producer": target, "rank": 1})
    opts = "--permission sysio@active"
    trans = node0.pushMessage("sysio", "setrank", data, opts)
    assert trans is not None and trans[0], f"setrank for {target} to rank 1 should succeed: {trans}"
    Print(f"setrank: {target} -> rank 1 succeeded")

    # Verify setrank with rank 0 (invalid) is rejected
    data = json.dumps({"producer": target, "rank": 0})
    trans = node0.pushMessage("sysio", "setrank", data, opts, silentErrors=True)
    assert trans is None or not trans[0], "setrank with rank 0 should fail"
    Print("setrank with rank 0 correctly rejected")

    # Verify setrank with non-existent producer fails
    data = json.dumps({"producer": "nonexistent1", "rank": 1})
    trans = node0.pushMessage("sysio", "setrank", data, opts, silentErrors=True)
    assert trans is None or not trans[0], "setrank with non-existent producer should fail"
    Print("setrank with non-existent producer correctly rejected")

    # Verify setrank without sysio authority fails
    data = json.dumps({"producer": target, "rank": 2})
    badOpts = f"--permission {target}@active"
    trans = node0.pushMessage("sysio", "setrank", data, badOpts, silentErrors=True)
    assert trans is None or not trans[0], "setrank without sysio authority should fail"
    Print("setrank without sysio authority correctly rejected")

    # ----------------------------------------------------------------
    # Final verification: LIB still advancing
    # ----------------------------------------------------------------
    Print("=== Final: Verify LIB still advancing ===")
    assert node0.waitForLibToAdvance(timeout=30), "LIB should still be advancing at end of test"
    Print("LIB is advancing - finality intact")

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
