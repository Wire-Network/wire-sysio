#!/usr/bin/env python3

import time
import random
import re

from TestHarness import Account, Cluster, ReturnType, TestHelper, Utils, WalletMgr

###############################################################
# subjective-billing-test
#
# tests to verify subjective billing behavior
#
# Nodes 0 and 1 produce. Node 2 exempts account1 and has a small subjective budget, node 3 decays in one
# minute, and node 4 does not keep the charge for sysio.*-paid actions.
#
###############################################################

# Node 2's subjective budget: small enough that a few transfers by an account without CPU exhaust it.
SUBJECTIVE_CPU_ALLOWED_US = 2000
# Bound on the transfers pushed to exhaust node 2's budget and land in blocks of both producers.
MAX_STICKY_TRANSFERS = 60
# A 24 h decay window loses well under 1% of a charge over the minutes this test runs.
DECAY_TOLERANCE = 0.99

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"-p","-n","-d","-s","--nodes-file","--seed"
                            ,"--dump-error-details","-v","--leave-running"
                            ,"--keep-logs","--unshared"})

pnodes=args.p
topo=args.s
delay=args.d
total_nodes = pnodes if args.n < pnodes else args.n
debug=args.v
nodesFile=args.nodes_file
dontLaunch=nodesFile is not None
seed=args.seed
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

random.seed(seed) # Use a fixed seed for repeatability.
cluster=Cluster(unshared=args.unshared, keepRunning=True if nodesFile is not None else args.leave_running, keepLogs=args.keep_logs)

walletMgr=WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)
SYSIO_ACCT_PRIVATE_DEFAULT_KEY = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
SYSIO_ACCT_PUBLIC_DEFAULT_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

try:
    if dontLaunch: # run test against remote cluster
        jsonStr=None
        with open(nodesFile, "r") as f:
            jsonStr=f.read()
        if not cluster.initializeNodesFromJson(jsonStr):
            errorExit("Failed to initilize nodes from Json string.")
        total_nodes=len(cluster.getNodes())

        print("Stand up walletd")
        if walletMgr.launch() is False:
            errorExit("Failed to stand up kiod.")
    else:
        cluster.setWalletMgr(walletMgr)

    Print ("producing nodes: %s, non-producing nodes: %d, topology: %s, delay between nodes launch(seconds): %d" % (pnodes, total_nodes-pnodes, topo, delay))

    specificArgs = {"2": f"--disable-subjective-account-billing=account1 --subjective-account-cpu-allowed-us={SUBJECTIVE_CPU_ALLOWED_US}",
                    "3": "--subjective-account-decay-time-minutes=1",
                    "4": "--disable-subjective-sysio-authorizer-billing=true" }

    Print("Stand up cluster")
    # Deliberately does NOT pass --disable-subjective-api-billing: subjective billing is on by
    # default, so leaving it unset exercises the shipped configuration. If the default is ever
    # flipped back, this test fails rather than silently covering nothing.
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, topo=topo, delay=delay, activateIF=True,
                      extraNodeopArgs=" --http-max-response-time-ms 990000 ",
                      specificExtraNodeopArgs=specificArgs ) is False:
       errorExit("Failed to stand up sys cluster.")

    Print ("Wait for Cluster stabilization")
    # wait for cluster to start producing blocks
    if not cluster.waitOnClusterBlockNumSync(3):
       errorExit("Cluster never stabilized")

    Print("Creating account1")
    account1 = Account('account1')
    account1.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    account1.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(account1, cluster.sysioAccount, stakedDeposit=1000)

    Print("Creating account2")
    account2 = Account('account2')
    account2.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    account2.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(account2, cluster.sysioAccount, stakedDeposit=1000, stakeCPU=1)

    Print("Validating accounts after bootstrap")
    cluster.validateAccounts([account1, account2], testSysAccounts=False)

    node = cluster.getNode()

    # api node configured with whitelist
    wlnode = cluster.nodes[2]

    # api node configured with decay of 1 min
    fdnode = cluster.nodes[3]

    preBalances = node.getSysBalances([account1, account2])
    Print("Starting balances:")
    Print(preBalances)

    #   Send through some failing transactions on 1 min decay node
    for x in range(5):
        memo = 'tx-{}'.format(x)
        txn = {
            "actions": [{"account": "sysio.token","name": "transfer",
                         "authorization": [{"actor": "account1","permission": "sysio.payer"},{"actor": "account1","permission": "active"}],
                         "data": {"from": "account1","to": "account2","quantity": "100000.0001 SYS","memo": memo},
                         "compression": "none"}]
        }

        results = fdnode.pushTransaction(txn)
        assert(not results[0])

    # Get subjective billing.  We'll check on this later.
    decaySubjectiveBilling = fdnode.getAccountSubjectiveInfo("account1")
    Print('Subjective billing used from fast decay node {}'.format(decaySubjectiveBilling["used"]))


    #   Send through some failing transactions on a non-whitelisted account
    Print('Verifying non-whitelisted account...')
    subjectiveBilling = wlnode.getAccountSubjectiveInfo("account2")
    Print('Initial Subjective billing used: {}'.format(subjectiveBilling["used"]))

    for x in range(5):
        memo = 'tx-{}'.format(x)
        txn = {
            "actions": [{"account": "sysio.token","name": "transfer",
                         "authorization": [{"actor": "account2","permission": "sysio.payer"},{"actor": "account2","permission": "active"}],
                         "data": {"from": "account2","to": "account1","quantity": "100000.0001 SYS","memo": memo},
                         "compression": "none"}]
        }

        results = wlnode.pushTransaction(txn)
        assert(not results[0])

    # Verify that subjective billing was charged
    subjectiveBilling = wlnode.getAccountSubjectiveInfo("account2")
    Print(subjectiveBilling)
    Print(subjectiveBilling["used"])
    assert(subjectiveBilling["used"] > 0)

    #   Send through some failing transactions on a whitelisted account
    Print("Verifying whitelisted account...")
    subjectiveBilling = wlnode.getAccountSubjectiveInfo("account1")
    Print('Initial Subjective billing used: {}'.format(subjectiveBilling["used"]))
    for x in range(5):
        memo = 'tx-{}'.format(x)
        txn = {
            "actions": [{"account": "sysio.token","name": "transfer",
                         "authorization": [{"actor": "account1","permission": "sysio.payer"},{"actor": "account1","permission": "active"}],
                         "data": {"from": "account1","to": "account2","quantity": "100000.0001 SYS","memo": memo},
                         "compression": "none"}]
        }

        results = wlnode.pushTransaction(txn)
        assert(not results[0])

    # Verify that no subjective billing was not charged
    subjectiveBilling = wlnode.getAccountSubjectiveInfo("account1")
    Print(subjectiveBilling["used"])
    assert(subjectiveBilling["used"] == 0)

    def transfer(source, destination, memo, explicitPayer=False):
        """A 0.0001 SYS sysio.token transfer, paid by sysio.token unless explicitPayer makes the source pay."""
        authorization = [{"actor": source, "permission": "active"}]
        if explicitPayer:
            authorization.insert(0, {"actor": source, "permission": "sysio.payer"})
        return {"actions": [{"account": "sysio.token", "name": "transfer", "authorization": authorization,
                             "data": {"from": source, "to": destination, "quantity": "0.0001 SYS", "memo": memo},
                             "compression": "none"}]}

    def pushAndWaitForBlock(pushNode, txn, waitNodes):
        """Push txn through pushNode and wait until every node in waitNodes has it in a block. Returns its id."""
        results = pushNode.pushTransaction(txn)
        assert results[0], results[1]
        transId = pushNode.getTransId(results[1])
        for waitNode in waitNodes:
            assert waitNode.waitForTransactionInBlock(transId), f"{transId} never reached a block on {waitNode.nodeId}"
        return transId

    def subjectiveUsed(node, account):
        return node.getAccountSubjectiveInfo(account)["used"]

    def producerNames(node):
        """The producer names a node produces for, from its command line."""
        return set(re.findall(r'--producer-name ([a-z1-5.]+)', " ".join(node.cmd)))

    producers = [cluster.nodes[0], cluster.nodes[1]]
    keepOffNode = cluster.nodes[4]

    Print("Verifying an explicit sysio.payer is not charged after its transfer is in a block...")
    pushAndWaitForBlock(producers[0], transfer("account1", "account2", "payer-0", explicitPayer=True), [producers[1]])
    assert subjectiveUsed(producers[1], "account1") == 0

    Print("Verifying disable-subjective-account-billing exempts a sysio.token-paid transfer...")
    pushAndWaitForBlock(producers[0], transfer("account1", "account2", "sticky-0"), [producers[1], wlnode])
    assert subjectiveUsed(producers[1], "account1") > 0
    assert subjectiveUsed(wlnode, "account1") == 0

    Print("Creating account3 with no CPU of its own")
    account3 = Account('account3')
    account3.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    account3.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(account3, cluster.sysioAccount, stakedDeposit=10000, stakeCPU=0)

    # Transfers pushed only to node 3 are charged on every node from the blocks that include them, in the
    # producers' own blocks and in each other's, until node 2 has seen account3 use more than its budget.
    Print("Verifying sysio.token-paid transfers stay charged to their authorizer on every node...")
    allNodes = cluster.getNodes()
    totalCpu = 0
    includingNodes = set()
    for x in range(MAX_STICKY_TRANSFERS):
        if subjectiveUsed(wlnode, "account3") > SUBJECTIVE_CPU_ALLOWED_US and len(includingNodes) == len(producers):
            break
        transId = pushAndWaitForBlock(fdnode, transfer("account3", "account1", f"sticky-{x}"), allNodes)
        blockNum = producers[0].getBlockNumByTransId(transId)
        block = producers[0].getBlock(blockNum)
        includingNodes.update(p.nodeId for p in producers if block["producer"] in producerNames(p))
        receipt = next(r for r in block["transactions"] if r["trx"]["id"] == transId)
        totalCpu += sum(receipt["cpu_usage_us"])
    else:
        errorExit(f"account3 did not exceed node 2's budget in blocks of both producer nodes: {includingNodes}, "
                  f"node 2 charged {subjectiveUsed(wlnode, 'account3')}us")
    Print(f"account3 used {totalCpu}us in blocks produced by nodes {sorted(includingNodes)}")

    for chargingNode in producers + [wlnode]:
        used = subjectiveUsed(chargingNode, "account3")
        Print(f"node {chargingNode.nodeId} charged account3 {used}us")
        assert totalCpu * DECAY_TOLERANCE <= used <= totalCpu, f"node {chargingNode.nodeId}: {used} vs {totalCpu}"
    assert subjectiveUsed(fdnode, "account3") > 0
    assert subjectiveUsed(keepOffNode, "account3") == 0

    Print("Verifying an authorizer over budget is rejected, though its transfers are already in blocks...")
    results = wlnode.pushTransaction(transfer("account3", "account1", "over-budget"), silentErrors=True)
    assert not results[0]
    assert "Authorized account account3 exceeded subjective CPU limit" in str(results[1]), results[1]


    # Sleep for 1 min
    time.sleep(72)

    # Verify subjective decay
    acct1 = fdnode.getAccountSubjectiveInfo("account1")
    originalUsed = decaySubjectiveBilling["used"]
    finalUsed = acct1["used"]

    Print('Original Fast decay node subjective billing: {}'.format(decaySubjectiveBilling["used"]))
    Print('End decay node subjective billing: {}'.format(acct1["used"]))
    assert(finalUsed == 0)

    # The kept charge decays like any other: gone on the 1 minute node, still held on the 24 h nodes.
    assert subjectiveUsed(fdnode, "account3") == 0
    assert subjectiveUsed(producers[0], "account3") > 0

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
