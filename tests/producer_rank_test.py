#!/usr/bin/env python3

import json
import signal
import time

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
# 4. Register BLS finalizer keys for only 4 of them
# 5. Wait for update_ranked_producers to fire via onblock
# 6. Verify producer schedule changed (5 → 4 producers, version increased)
# 7. Verify finalizer policy set (4 finalizers from the keyed producers)
# 8. Take one scheduled producer's node down and verify it is DEMOTED for missed rounds
# 9. Verify the schedule-size floor retains it rather than publishing a short schedule
# 10. Restart the node and verify producing a block clears the demotion
#
# Rank is POSITION in the score-ordered producer index, derived by iteration -- there is no
# action that assigns it. A producer holds a position only if it is schedulable, which requires
# an ACTIVE PRODUCER operator row in sysio.opreg AND an active finalizer key. Withholding the
# finalizer key from one producer is therefore what makes the schedule drop from 5 to 4.
#
# The demotion phases exercise what no single-process contract test can: miss attribution against
# real block production, and recovery against real finality. They are only reachable on a live
# cluster because a producer has to actually stop producing, and then actually start again.
#
###############################################################

Print = Utils.Print
errorExit = Utils.errorExit

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
pnodes = 5
totalNodes = pnodes
keyedCount = 4  # only rank 4 of the 5 producers
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

    # The first keyedCount producers (alphabetically) get finalizer keys; the last one does not
    # and so holds no rank position -- it should be excluded from the schedule and the finalizer
    # policy once update_ranked_producers fires.
    keyedProdNames = prodNames[:keyedCount]
    excludedProd = prodNames[keyedCount]
    Print(f"Finalizer-keyed producers: {keyedProdNames}")
    Print(f"Excluded (no finalizer key) producer: {excludedProd}")

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

    # The bootstrap deploys sysio.opreg and registers every producer as an ACTIVE
    # OPERATOR_TYPE_PRODUCER operator, so update_ranked_producers' collateral gate is
    # satisfied and scheduling turns purely on rank + finalizer key below.

    # ----------------------------------------------------------------
    # Phase 2: Register BLS finalizer keys for all but one producer
    # ----------------------------------------------------------------
    Print("=== Phase 2: Register finalizer keys via regfinkey ===")
    for name in keyedProdNames:
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
    # Phase 3: Wait for update_ranked_producers to fire
    # ----------------------------------------------------------------
    # onblock calls update_ranked_producers when timestamp.slot - last_update.slot > 120.
    # After system contract init, last_producer_schedule_update is 0, so the first
    # update_ranked_producers fires on the very first onblock. Since that happens before
    # we register keys, it finds no qualified producers and sets last_update to current time.
    # We need to wait ~120 more slots (60 seconds) for the next cycle.
    Print("=== Phase 3: Waiting for update_ranked_producers cycle (~65 seconds) ===")
    assert node0.waitForHeadToAdvance(blocksToAdvance=135, timeout=90), \
        "Head should advance 135 blocks for update_ranked_producers cycle"

    # Wait additional blocks for pending schedule/policy to become active
    Print("Waiting for pending policies to take effect...")
    assert node0.waitForHeadToAdvance(blocksToAdvance=60, timeout=45), \
        "Head should advance 60 more blocks for policy activation"

    # Ensure LIB is still advancing (finality working)
    assert node0.waitForLibToAdvance(timeout=30), "LIB should still be advancing"

    # ----------------------------------------------------------------
    # Phase 4: Verify producer schedule changed
    # ----------------------------------------------------------------
    Print("=== Phase 4: Verify producer schedule changed ===")
    schedule = node0.processUrllibRequest("chain", "get_producer_schedule")

    activeSchedule = schedule["payload"]["active"]
    newVersion = activeSchedule["version"]
    activeProducers = sorted([p["producer_name"] for p in activeSchedule["producers"]])
    Print(f"New schedule version: {newVersion}, producers: {activeProducers}")

    # Schedule version should have increased
    assert newVersion > initVersion, \
        f"Schedule version should have increased from {initVersion}, got {newVersion}"

    # Should have exactly keyedCount producers
    assert len(activeProducers) == keyedCount, \
        f"Expected {keyedCount} active producers, got {len(activeProducers)}: {activeProducers}"

    # All ranked producers should be in the schedule
    for name in keyedProdNames:
        assert name in activeProducers, f"Ranked producer {name} should be in active schedule"

    # The excluded producer should NOT be in the schedule
    assert excludedProd not in activeProducers, \
        f"Unranked producer {excludedProd} should NOT be in active schedule"
    Print(f"Producer schedule changed: {pnodes} -> {keyedCount} producers, "
          f"version {initVersion} -> {newVersion}")

    # ----------------------------------------------------------------
    # Phase 5: Verify finalizer policy
    # ----------------------------------------------------------------
    Print("=== Phase 5: Verify finalizer policy ===")
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

    # Look for the keyedCount-finalizer policy in either active or pending.
    policyToCheck = None
    policyState = None
    if activeFinCount == keyedCount:
        policyToCheck = activeFP
        policyState = "active"
    elif pendingFinCount == keyedCount:
        policyToCheck = pendingFP
        policyState = "pending"

    assert policyToCheck is not None, \
        f"Expected a finalizer policy with {keyedCount} finalizers. " \
        f"Active has {activeFinCount}, Pending has {pendingFinCount}"
    Print(f"System contract finalizer policy is {policyState}")

    # Verify threshold: (N * 2) / 3 + 1
    expectedThreshold = keyedCount * 2 // 3 + 1
    assert policyToCheck["threshold"] == expectedThreshold, \
        f"Expected threshold {expectedThreshold}, got {policyToCheck['threshold']}"

    # Verify each ranked producer has a finalizer entry with weight 1
    finalizerDescs = sorted([f["description"] for f in policyToCheck["finalizers"]])
    Print(f"Finalizer descriptions: {finalizerDescs}")
    for name in keyedProdNames:
        assert name in finalizerDescs, f"Producer {name} should be in finalizer policy"

    # The excluded producer should NOT be a finalizer
    assert excludedProd not in finalizerDescs, \
        f"Unranked producer {excludedProd} should NOT be in finalizer policy"

    for f in policyToCheck["finalizers"]:
        assert f["weight"] == 1, f"Finalizer weight should be 1, got {f['weight']}"

    # ----------------------------------------------------------------
    # Phase 6: A scheduled producer that stops producing is demoted
    # ----------------------------------------------------------------
    # Demotion is what makes the score model self-defending: a producer that holds a slot but is
    # absent has to lose its standing without anyone intervening. `onblock` charges a missed round
    # to every name the round-robin passed over, and once `max_consecutive_missed_rounds` (3 by
    # default) land consecutively the producer moves into a tier no amount of collateral can climb
    # out of.
    Print("=== Phase 6: Demote a producer by taking its node down ===")

    def producerRow(name):
        """The producer's `sysio.system::producers` row, or None if it has none.

        v6 promotes the table to KV, so each row arrives as {"key": ..., "value": ...} and the
        fields live under `value`; the fallback keeps this working if that ever flattens.
        """
        resp = node0.processUrllibRequest("chain", "get_table_rows", {
            "code": "sysio", "scope": "sysio", "table": "producers", "limit": 100, "json": True
        })
        assert resp["code"] == 200, f"get_table_rows(producers) returned {resp['code']}: {resp}"
        for row in resp["payload"]["rows"]:
            fields = row.get("value", row)
            if fields.get("owner") == name:
                return fields
        return None

    def waitForDemotedFlag(name, expected, timeout=300):
        """Poll `name`'s producers row until `is_demoted` is `expected`.

        Returns the row, or None if the flag never got there. Polling one producer window at a
        time keeps the query count low while a node is down and blocks arrive at a reduced rate.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            row = producerRow(name)
            if row is not None and row["is_demoted"] == expected:
                return row
            node0.waitForHeadToAdvance(blocksToAdvance=12, timeout=60)
        return None

    # Demote a producer node0 does NOT host, so every query below keeps working while it is down.
    demotedProd = next(name for name in keyedProdNames
                       if producers[name].nodeId != node0.nodeId)
    demotedNode = producers[demotedProd]
    Print(f"Demotion target: {demotedProd} (node {demotedNode.nodeId})")

    beforeRow = producerRow(demotedProd)
    assert beforeRow is not None, f"{demotedProd} should have a producers row"
    assert not beforeRow["is_demoted"], f"{demotedProd} should not be demoted before its outage"

    demotedNode.kill(signal.SIGTERM)
    Print(f"Stopped {demotedProd}'s node; waiting for its rounds to go unproduced")

    demotedRow = waitForDemotedFlag(demotedProd, True)
    assert demotedRow is not None, \
        f"{demotedProd} should have been demoted after missing consecutive rounds"
    assert demotedRow["consecutive_missed_rounds"] >= 3, \
        f"Expected at least 3 consecutive missed rounds, got {demotedRow['consecutive_missed_rounds']}"
    Print(f"{demotedProd} demoted after {demotedRow['consecutive_missed_rounds']} missed rounds")

    # Every other producer keeps producing, so none of them may be charged a miss: attribution is
    # per-slot, not a blanket penalty on the round.
    for name in keyedProdNames:
        if name == demotedProd:
            continue
        row = producerRow(name)
        assert row is not None and not row["is_demoted"], \
            f"{name} was still producing and must not be demoted"

    # Finality survives the outage. The policy carries keyedCount finalizers with a threshold of
    # keyedCount * 2 // 3 + 1, so one absent finalizer still leaves enough to reach it -- which is
    # what lets the chain keep advancing long enough to demote the absent producer at all.
    assert node0.waitForLibToAdvance(timeout=60), \
        "LIB should keep advancing with one of the finalizers down"

    # ----------------------------------------------------------------
    # Phase 7: The schedule-size floor retains the demoted producer
    # ----------------------------------------------------------------
    # Demotion drops the schedulable count to keyedCount - 1, below `min_schedule_size`.
    # `update_ranked_producers` refuses to publish a schedule under that floor -- it retains the
    # last good one rather than concentrate block production and finality onto too few nodes --
    # so the demoted producer keeps its slot. That gap between "demoted" and "rescheduled" is
    # exactly the window Phase 8 recovers from, and on a real outage it can stay open for good.
    Print("=== Phase 7: Verify the schedule-size floor retains the demoted producer ===")
    assert node0.waitForHeadToAdvance(blocksToAdvance=135, timeout=240), \
        "Head should advance through an update_ranked_producers cycle"

    heldSchedule = node0.processUrllibRequest("chain", "get_producer_schedule")
    heldProducers = sorted([p["producer_name"] for p in heldSchedule["payload"]["active"]["producers"]])
    Print(f"Schedule after demotion: {heldProducers}")
    assert demotedProd in heldProducers, \
        f"The floor should have retained {demotedProd}; schedule is {heldProducers}"
    assert len(heldProducers) == keyedCount, \
        f"Expected the schedule retained at {keyedCount} producers, got {heldProducers}"

    # ----------------------------------------------------------------
    # Phase 8: Producing again clears the demotion
    # ----------------------------------------------------------------
    # A block is the strongest liveness proof there is, so a demoted producer that is STILL in the
    # active schedule recovers by producing one -- no `regproducer`, no operator intervention.
    # Without it the producers a mass outage demoted would keep producing under the retained
    # schedule while `payepoch` skipped them, earning nothing until every operator re-registered by
    # hand. A producer the schedule has actually dropped never reaches this path.
    Print("=== Phase 8: Restart the node and verify the demotion clears ===")
    assert demotedNode.relaunch(), f"Failed to relaunch {demotedProd}'s node"

    recoveredRow = waitForDemotedFlag(demotedProd, False)
    assert recoveredRow is not None, \
        f"{demotedProd} should have cleared its demotion by producing a block"
    assert recoveredRow["consecutive_missed_rounds"] == 0, \
        f"Expected the miss streak to reset, got {recoveredRow['consecutive_missed_rounds']}"
    Print(f"{demotedProd} recovered by producing -- demotion and miss streak both cleared")

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
