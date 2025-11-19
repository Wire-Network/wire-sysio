#!/usr/bin/env python3

import time
import signal
import threading
import os
import platform

from TestHarness import Account, Cluster, ReturnType, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# interrupt_read_only_trx_test
#
# Verify an infinite read-only trx can be interrupted by ctrl-c
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

appArgs=AppArgs()
appArgs.add(flag="--read-only-threads", type=int, help="number of read-only threads", default=0)
appArgs.add(flag="--sys-vm-oc-enable", type=str, help="specify sys-vm-oc-enable option", default="auto")
appArgs.add(flag="--wasm-runtime", type=str, help="if wanting sys-vm-oc, must use 'sys-vm-oc-forced'", default="sys-vm-jit")

args=TestHelper.parse_args({"-p","-n","-d","-s","--dump-error-details","-v","--leave-running"
                            ,"--keep-logs","--unshared"}, applicationSpecificArgs=appArgs)

pnodes=args.p
topo=args.s
delay=args.d
total_nodes = pnodes if args.n < pnodes else args.n
# For this test, we need at least 1 non-producer
if total_nodes <= pnodes:
    Print ("non-producing nodes %d must be greater than 0. Force it to %d. producing nodes: %d," % (total_nodes - pnodes, pnodes + 1, pnodes))
    total_nodes = pnodes + 1
debug=args.v
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False
noOC = args.sys_vm_oc_enable == "none"
allOC = args.sys_vm_oc_enable == "all"

# all debuglevel so that "executing ${h} with sys vm oc" is logged
cluster=Cluster(loggingLevel="all", unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)

walletMgr=WalletMgr(True)
SYSIO_ACCT_PRIVATE_DEFAULT_KEY = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
SYSIO_ACCT_PUBLIC_DEFAULT_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

producerNode = None
apiNode = None
payloadlessAccountName = "payloadless"

def getCodeHash(node, account):
    # Example get code result: code hash: 67d0598c72e2521a1d588161dad20bbe9f8547beb5ce6d14f3abd550ab27d3dc
    cmd = f"get code {account}"
    codeHash = node.processClioCmd(cmd, cmd, silentErrors=False, returnType=ReturnType.raw)
    if codeHash is None: errorExit(f"Unable to get code {account} from node {node.nodeId}")
    else: codeHash = codeHash.split(' ')[2].strip()
    if Utils.Debug: Utils.Print(f"{account} code hash: {codeHash}")
    return codeHash

def startCluster():
    global total_nodes
    global producerNode
    global apiNode

    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    Print ("producing nodes: %d, non-producing nodes: %d, topology: %s, delay between nodes launch(seconds): %d" % (pnodes, total_nodes-pnodes, topo, delay))

    Print("Stand up cluster")
    # set up read-only options for API node
    specificExtraNodeopArgs={}
    # producer nodes will be mapped to 0 through pnodes-1, so the number pnodes is the no-producing API node
    specificExtraNodeopArgs[pnodes]=" --plugin sysio::net_api_plugin"
    specificExtraNodeopArgs[pnodes]+=" --contracts-console "
    specificExtraNodeopArgs[pnodes]+=" --read-only-read-window-time-us "
    specificExtraNodeopArgs[pnodes]+=" 3600000000 " # 1hr to test interrupt of ctrl-c
    specificExtraNodeopArgs[pnodes]+=" --sys-vm-oc-cache-size-mb "
    specificExtraNodeopArgs[pnodes]+=" 1 " # set small so there is churn
    specificExtraNodeopArgs[pnodes]+=" --read-only-threads "
    specificExtraNodeopArgs[pnodes]+=str(args.read_only_threads)
    if args.sys_vm_oc_enable:
        if platform.system() != "Linux":
            Print("OC not run on Linux. Skip the test")
            exit(0) # Do not fail the test
        specificExtraNodeopArgs[pnodes]+=" --sys-vm-oc-enable "
        specificExtraNodeopArgs[pnodes]+=args.sys_vm_oc_enable
    if args.wasm_runtime:
        specificExtraNodeopArgs[pnodes]+=" --wasm-runtime "
        specificExtraNodeopArgs[pnodes]+=args.wasm_runtime
    extraNodeopArgs=" --http-max-response-time-ms 99000000 --disable-subjective-api-billing false "
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, topo=topo, delay=delay, activateIF=True,
                      specificExtraNodeopArgs=specificExtraNodeopArgs, extraNodeopArgs=extraNodeopArgs ) is False:
        errorExit("Failed to stand up sys cluster.")

    Print ("Wait for Cluster stabilization")
    # wait for cluster to start producing blocks
    if not cluster.waitOnClusterBlockNumSync(3):
        errorExit("Cluster never stabilized")

    producerNode = cluster.getNode()
    apiNode = cluster.nodes[-1]

    sysioCodeHash = getCodeHash(producerNode, "sysio.token")
    # sysio.* should be using oc unless oc tierup disabled
    Utils.Print(f"search: executing {sysioCodeHash} with sys vm oc")
    found = producerNode.findInLog(f"executing {sysioCodeHash} with sys vm oc")
    assert( found or (noOC and not found) )

def deployTestContracts():
    Utils.Print("Create payloadless account and deploy payloadless contract")
    payloadlessAccount = Account(payloadlessAccountName)
    payloadlessAccount.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    payloadlessAccount.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(payloadlessAccount, cluster.sysioAccount, buyRAM=100000)

    payloadlessContractDir="unittests/test-contracts/payloadless"
    payloadlessWasmFile="payloadless.wasm"
    payloadlessAbiFile="payloadless.abi"
    producerNode.publishContract(payloadlessAccount, payloadlessContractDir, payloadlessWasmFile, payloadlessAbiFile, waitForTransBlock=True)

def sendTransaction(account, action, data, auth=[], opts=None):
    trx = {
       "actions": [{
          "account": account,
          "name": action,
          "authorization": auth,
          "data": data
      }]
    }
    return apiNode.pushTransaction(trx, opts, silentErrors=True)

def sendReadOnlyForeverPayloadless():
    try:
        sendTransaction('payloadless', action='doitforever', data={}, auth=[], opts='--read')
    except Exception as e:
        Print("Ignore Exception in sendReadOnlyForeverPayloadless: ", repr(e))

def timeoutTest():
    Print("Timeout Test")

    # Send a forever readonly transaction
    Print("Sending a forever read only transaction")
    trxThread = threading.Thread(target = sendReadOnlyForeverPayloadless)
    trxThread.start()

    # give plenty of time for thread to send read-only trx
    assert producerNode.waitForHeadToAdvance(blocksToAdvance=5)
    assert producerNode.waitForLibToAdvance(), "Producer node stopped advancing LIB"

    Print("Verify node not processing incoming blocks, blocks do not interrupt read-only trx")
    blockNum = apiNode.getHeadBlockNum()
    assert producerNode.waitForLibToAdvance(), "Producer node stopped advancing LIB after forever read-only trx"
    assert blockNum == apiNode.getHeadBlockNum(), "Head still advancing when node should be processing a read-only transaction"

    Print("Verify ctrl-c will interrupt and shutdown node")
    assert apiNode.kill(signal.SIGTERM), "API Node not killed by SIGTERM"

    trxThread.join()

    Print("Verify node will restart")
    assert apiNode.relaunch(), "API Node not restarted after SIGTERM shutdown"


try:
    startCluster()
    deployTestContracts()
    timeoutTest()

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
