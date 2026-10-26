#!/usr/bin/env python3

import random
import time
import signal
import threading
import os
import traceback

from TestHarness import Account, Cluster, ReturnType, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# send_read_only_transaction_tests
#
# Tests to exercise the send_read_only_transaction RPC endpoint functionality.
# More internal tests are in unittest/read_only_trx_tests.cpp
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

appArgs=AppArgs()
appArgs.add(flag="--read-only-threads", type=int, help="number of read-only threads", default=0)
appArgs.add(flag="--read-only-read-window-time-us", type=int, help="read-only read window in microseconds", default=1000000)
appArgs.add(flag="--num-test-runs", type=int, help="number of times to run the tests", default=1)
appArgs.add(flag="--sys-vm-oc-enable", type=str, help="specify sys-vm-oc-enable option", default=Utils.SysVmOcEnableAuto)
appArgs.add(flag="--wasm-runtime", type=str, help="if wanting sys-vm-oc, must use 'sys-vm-oc-forced'",
            default=Utils.defaultWasmRuntime())
appArgs.add(flag="--api-node-read-mode", type=str, help="read-mode of the API node", default="head")

args=TestHelper.parse_args({"-p","-n","-d","-s","--nodes-file","--seed"
                            ,"--activate-if","--dump-error-details","-v","--leave-running"
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
nodesFile=args.nodes_file
dontLaunch=nodesFile is not None
seed=args.seed
activateIF=args.activate_if
dumpErrorDetails=args.dump_error_details
numTestRuns=args.num_test_runs

Utils.Debug=debug
testSuccessful=False
noOC = args.sys_vm_oc_enable == Utils.SysVmOcEnableNone
ocSupported = Utils.nodeopSupportsSysVmOc()
ocRequested = Utils.sysVmOcExecutionExpected(args.sys_vm_oc_enable)

random.seed(seed) # Use a fixed seed for repeatability.
# all debuglevel so that "executing ${h} with sys vm oc" is logged
cluster=Cluster(loggingLevel="all", unshared=args.unshared, keepRunning=True if nodesFile is not None else args.leave_running, keepLogs=args.keep_logs)

walletMgr=WalletMgr(True)
SYSIO_ACCT_PRIVATE_DEFAULT_KEY = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
SYSIO_ACCT_PUBLIC_DEFAULT_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

producerNode = None
apiNode = None
testAccountName = "test"
userAccountName = "user"
payloadlessAccountName = "payloadless"
infiniteAccountName = "infinite"
READ_WINDOW_ASSERTION_MARGIN_US = 60000
if args.read_only_read_window_time_us <= READ_WINDOW_ASSERTION_MARGIN_US:
    errorExit("--read-only-read-window-time-us must be greater than %d" % (READ_WINDOW_ASSERTION_MARGIN_US))
# Keep the fallback assertion threshold below the read window.
max_trx_time = args.read_only_read_window_time_us - READ_WINDOW_ASSERTION_MARGIN_US

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
    global testSuccessful

    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

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

    Print ("producing nodes: %d, non-producing nodes: %d, topology: %s, delay between nodes launch(seconds): %d" % (pnodes, total_nodes-pnodes, topo, delay))

    Print("Stand up cluster")
    # set up read-only options for API node
    specificExtraNodeopArgs={}
    # producer nodes will be mapped to 0 through pnodes-1, so the number pnodes is the no-producing API node
    specificExtraNodeopArgs[pnodes]=" --plugin sysio::net_api_plugin"
    specificExtraNodeopArgs[pnodes]+=" --contracts-console "
    specificExtraNodeopArgs[pnodes]+=" --read-only-write-window-time-us "
    specificExtraNodeopArgs[pnodes]+=" 10000 "
    specificExtraNodeopArgs[pnodes]+=" --read-only-read-window-time-us "
    specificExtraNodeopArgs[pnodes]+=str(args.read_only_read_window_time_us)
    specificExtraNodeopArgs[pnodes]+=" "
    specificExtraNodeopArgs[pnodes]+=" --read-only-threads "
    specificExtraNodeopArgs[pnodes]+=str(args.read_only_threads)
    specificExtraNodeopArgs[pnodes]+=" --read-mode "
    specificExtraNodeopArgs[pnodes]+=args.api_node_read_mode
    if Utils.shouldSkipBecauseSysVmOcUnavailable(args.sys_vm_oc_enable, args.wasm_runtime):
        Print("sys-vm-oc is unavailable on this platform. Skip the test")
        testSuccessful = True
        exit(0) # Do not fail the test
    if ocSupported:
        specificExtraNodeopArgs[pnodes]+=" --sys-vm-oc-enable "
        specificExtraNodeopArgs[pnodes]+=args.sys_vm_oc_enable
    if ocRequested:
        specificExtraNodeopArgs[pnodes]+=" --sys-vm-oc-cache-size-mb "
        specificExtraNodeopArgs[pnodes]+=" 1 " # set small so there is churn
    if args.wasm_runtime:
        specificExtraNodeopArgs[pnodes]+=" --wasm-runtime "
        specificExtraNodeopArgs[pnodes]+=args.wasm_runtime
    extraNodeopArgs=" --http-max-response-time-ms 990000 --disable-subjective-api-billing false "
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, topo=topo, delay=delay, activateIF=activateIF, specificExtraNodeopArgs=specificExtraNodeopArgs, extraNodeopArgs=extraNodeopArgs ) is False:
        errorExit("Failed to stand up sys cluster.")

    Print ("Wait for Cluster stabilization")
    # wait for cluster to start producing blocks
    if not cluster.waitOnClusterBlockNumSync(3):
        errorExit("Cluster never stabilized")

    producerNode = cluster.getNode()
    apiNode = cluster.nodes[-1]

    # sysio.* should be using oc unless oc tierup disabled.
    Utils.assertInitialSysVmOcLogState(cluster, producerNode, apiNode, args.sys_vm_oc_enable, ocSupported)

    if ocRequested:
        verifyOcVirtualMemory()

def verifyOcVirtualMemory():
    try:
        with open(f"/proc/{apiNode.pid}/statm") as f:
            data = f.read().split()
            vmPages = int(data[0])
            pageSize = os.sysconf("SC_PAGESIZE")
            actualVmSize = vmPages * pageSize

            # In OC tierup a memory slice is 8GB;
            # The main thread uses 529 slices; each read-only thread uses 11 slices.
            # Total virtual memory is around:
            # 529 slices * 8GB (for main thread) + numReadOnlyThreads * 11 slices * 8GB
            # This test verifies virtual memory does not grow by the number
            # of read-only thread in the order of TB.
            memoryByOthersGB = 1000 # add 1TB for virtual memory used by others
            expectedVmSize = ((529 * 8) + (args.read_only_threads * 88) + memoryByOthersGB) * 1024 * 1024 * 1024
            Utils.Print(f"pid: {apiNode.pid}, actualVmSize: {actualVmSize}, expectedVmSize: {expectedVmSize}")
            assert(actualVmSize < expectedVmSize)
    except FileNotFoundError:
        Utils.Print(f"/proc/{apiNode.pid}/statm not found")
        assert(False)

def deployTestContracts():
    Utils.Print("create test accounts")
    testAccount = Account(testAccountName)
    testAccount.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    testAccount.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(testAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=500000) # 95632 bytes required for test contract

    userAccount = Account(userAccountName)
    userAccount.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    userAccount.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(userAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, stakeCPU=2000)

    noAuthTableContractDir="unittests/test-contracts/no_auth_table"
    noAuthTableWasmFile="no_auth_table.wasm"
    noAuthTableAbiFile="no_auth_table.abi"
    Utils.Print("Publish no_auth_table contract")
    producerNode.publishContract(testAccount, noAuthTableContractDir,noAuthTableWasmFile, noAuthTableAbiFile, waitForTransBlock=True)

    Utils.Print("Create payloadless account and deploy payloadless contract")
    payloadlessAccount = Account(payloadlessAccountName)
    payloadlessAccount.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    payloadlessAccount.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(payloadlessAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=100000)
    payloadlessContractDir="unittests/test-contracts/payloadless"
    payloadlessWasmFile="payloadless.wasm"
    payloadlessAbiFile="payloadless.abi"
    producerNode.publishContract(payloadlessAccount, payloadlessContractDir, payloadlessWasmFile, payloadlessAbiFile, waitForTransBlock=True)

    Utils.Print("Create infinite account and deploy infinite contract")
    infiniteAccount = Account(infiniteAccountName)
    infiniteAccount.ownerPublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    infiniteAccount.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(infiniteAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=100000)
    infiniteContractDir="unittests/test-contracts/infinite"
    infiniteWasmFile="infinite.wasm"
    infiniteAbiFile="infinite.abi"
    producerNode.publishContract(infiniteAccount, infiniteContractDir, infiniteWasmFile, infiniteAbiFile, waitForTransBlock=True)

def sendTransaction(account, action, data, auth=[], opts=None):
    trx = {
       "actions": [{
          "account": account,
          "name": action,
          "authorization": auth,
          "data": data
      }]
    }
    return apiNode.pushTransaction(trx, opts)

def sendReadOnlyPayloadless():
    return sendTransaction('payloadless', action='doit', data={}, auth=[], opts='--read')

def sendReadOnlySlowPayloadless():
    return sendTransaction('payloadless', action='doitslow', data={}, auth=[], opts='--read')

def sendReadOnlyForeverPayloadless():
    return sendTransaction('payloadless', action='doitforever', data={}, auth=[], opts='--read')

def sendReadOnlyForeverInfinite():
    return sendTransaction('infinite', action='runforever', data={}, auth=[], opts='--read')

def sendReadOnlySEGV():
    return sendTransaction('infinite', action='segv', data={}, auth=[], opts='--read')

# Exception codes a read-only transaction may legitimately fail with
# (libraries/chain/include/sysio/chain/exceptions.hpp). Anything else, above all a transport
# error from clio because nodeop is gone, is a test failure.
TX_CPU_USAGE_EXCEEDED = 3080004 # tx_cpu_usage_exceeded: the transaction's own CPU limit
DEADLINE_EXCEPTION    = 3080006 # deadline_exception: a deadline closed on the transaction
WASM_EXECUTION_ERROR  = 3070002 # wasm_execution_error: e.g. an access violation in the contract
TIMEOUT_CODES = (TX_CPU_USAGE_EXCEEDED, DEADLINE_EXCEPTION)

def assertFailedWithCode(result, expectedCodes, what):
    """Require a failure that carries one of expectedCodes.

    pushTransaction reports every clio failure, a refused connection after nodeop died included,
    as a (False, message) tuple, so a bare `not result[0]` would accept a dead node. A read-only
    failure arrives as a trace dict; a regular transaction's failure may arrive as clio's stderr
    text, which still has to name the expected code.
    """
    ok, payload = result
    assert not ok, f"{what}: expected a failure, but the transaction succeeded: {payload}"
    if isinstance(payload, dict):
        exc = payload.get('processed', {}).get('except')
        assert exc is not None, f"{what}: response carries no exception trace: {payload}"
        assert exc['code'] in expectedCodes, \
            f"{what}: unexpected exception {exc['code']} {exc['name']}: {exc['message']}"
    else:
        text = str(payload)
        assert any(str(code) in text for code in expectedCodes), \
            f"{what}: clio failed without the expected exception, is nodeop still up? {text}"

def assertApiNodeAlive(stage):
    """Fail if the API node process exited: no read-only workload may take nodeop down."""
    pid = apiNode.pid
    assert apiNode.verifyAlive(), f"API node (pid {pid}) exited during {stage}"

class WorkerThread(threading.Thread):
    """A thread that keeps the exception its target raised, so joinAll can fail the test on it.

    A plain Thread prints the traceback and dies; join() then returns as if nothing happened.
    """
    def __init__(self, target, args=()):
        super().__init__(target=target, args=args)
        self.targetName = target.__name__
        self.error = None

    def run(self):
        try:
            super().run()
        except Exception as e:
            self.error = e
            Print(f"Exception in {self.name} ({self.targetName}): {repr(e)}")
            traceback.print_exc()

def joinAll(threads):
    """Join every thread, then fail on the ones that raised."""
    for thr in threads:
        thr.join()
    failed = [f"{thr.name} ({thr.targetName}): {repr(thr.error)}" for thr in threads if thr.error is not None]
    assert not failed, "worker thread failures: " + "; ".join(failed)

# Send read-only trxs from multiple threads to bump load. Runs on a WorkerThread, which keeps a
# failure for joinAll.
def sendReadOnlyTrxOnThread(startId, numTrxs):
    Print("start sendReadOnlyTrxOnThread")

    for i in range(numTrxs):
        results = sendTransaction(testAccountName, 'age', {"user": userAccountName, "id": startId + i}, opts='--read')
        assert results[0], f"read-only age failed: {results[1]}"
        assert(results[1]['processed']['action_traces'][0]['return_value_data'] == 25)

        results = sendReadOnlyPayloadless()
        assert results[0], f"read-only payloadless failed: {results[1]}"
        assert(results[1]['processed']['action_traces'][0]['console'] == "Im a payloadless action")

        results = sendReadOnlySlowPayloadless()
        assert results[0], f"read-only slow payloadless failed: {results[1]}"
        assert(results[1]['processed']['action_traces'][0]['console'] == "Im a payloadless slow action")
        assert(int(results[1]['processed']['elapsed']) > 100)

# Send regular trxs from multiple threads to bump load. Runs on a WorkerThread.
def sendTrxsOnThread(startId, numTrxs, opts=None):
    Print("sendTrxsOnThread: ", startId, numTrxs, opts)

    for i in range(numTrxs):
        results = sendTransaction(testAccountName, 'age', {"user": userAccountName, "id": startId + i}, auth=[{"actor": userAccountName, "permission":"active"}], opts=opts)
        assert results[0], f"age transaction failed: {results[1]}"

# Issue an RPC repeatedly. Runs on a WorkerThread.
def doRpc(resource, command, numRuns, fieldIn, expectedValue, code, payload={}):
    for i in range(numRuns):
        ret_json = apiNode.processUrllibRequest(resource, command, payload)
        if code is None:
            assert(ret_json["payload"][fieldIn] is not None)
            if expectedValue is not None:
                assert(ret_json["payload"][fieldIn] == expectedValue)
        else:
            assert(ret_json["code"] == code)

def runReadOnlyTrxAndRpcInParallel(resource, command, fieldIn=None, expectedValue=None, code=None, payload={}):
    Print("runReadOnlyTrxAndRpcInParallel: ", command)

    numRuns = 10
    trxThread = WorkerThread(sendReadOnlyTrxOnThread, (0, numRuns))
    rpcThread = WorkerThread(doRpc, (resource, command, numRuns, fieldIn, expectedValue, code, payload))
    trxThread.start()
    rpcThread.start()

    joinAll([trxThread, rpcThread])
    assertApiNodeAlive(f"runReadOnlyTrxAndRpcInParallel {command}")

def mixedOpsTest(opt=None):
    Print("mixedOpsTest -- opt = ", opt)

    numRuns = 100
    readOnlyThread = WorkerThread(sendReadOnlyTrxOnThread, (0, numRuns))
    readOnlyThread.start()
    sendTrxThread = WorkerThread(sendTrxsOnThread, (numRuns, numRuns, opt))
    sendTrxThread.start()
    getRawThread = WorkerThread(doRpc, ("chain", "get_raw_code_and_abi", numRuns, "account_name", testAccountName, 200, {"account_name":testAccountName}))
    getRawThread.start()

    joinAll([readOnlyThread, sendTrxThread, getRawThread])
    assertApiNodeAlive("mixedOpsTest")

def sendMulReadOnlyTrx(numThreads):
    threadList = []
    num_trxs_per_thread = 200
    for i in range(numThreads):
        thr = WorkerThread(sendReadOnlyTrxOnThread, (i * num_trxs_per_thread, num_trxs_per_thread))
        thr.start()
        threadList.append(thr)
    joinAll(threadList)
    assertApiNodeAlive(f"sendMulReadOnlyTrx with {numThreads} threads")

def basicTests():
    Print("Insert a user")
    results = sendTransaction(testAccountName, 'insert', {"user": userAccountName, "id": 1, "age": 10}, auth=[{"actor": userAccountName, "permission":"active"}])
    assert(results[0])
    apiNode.waitForTransactionInBlock(results[1]['transaction_id'])

    testAccountCodeHash = getCodeHash(producerNode, testAccountName)
    found = producerNode.findInLog(f"executing {testAccountCodeHash} with sys vm oc")
    assert(not found)

    # verify the return value (age) from read-only is the same as created.
    Print("Send a read-only Get transaction to verify previous Insert")
    results = sendTransaction(testAccountName, 'getage', {"user": userAccountName}, opts='--read')
    assert(results[0])
    assert(results[1]['processed']['action_traces'][0]['return_value_data'] == 10)
    if noOC or not ocSupported:
        Utils.assertSysVmOcLogAbsent(apiNode, testAccountCodeHash, "apiNode")

    # verify non-read-only modification works
    Print("Send a non-read-only Modify transaction")
    results = sendTransaction(testAccountName, 'modify', {"user": userAccountName, "age": 25}, auth=[{"actor": userAccountName, "permission": "active"}])
    assert(results[0])
    apiNode.waitForTransactionInBlock(results[1]['transaction_id'])

    Print("Send a non-read-only SEGV transaction")
    result = sendTransaction('infinite', action='segv', data={}, auth=[{"actor": userAccountName, "permission": "active"}])
    assertFailedWithCode(result, (WASM_EXECUTION_ERROR,), "non-read-only segv")

    Print("Send a read-only SEGV transaction")
    result = sendReadOnlySEGV()
    assertFailedWithCode(result, (WASM_EXECUTION_ERROR,), "read-only segv")
    assertApiNodeAlive("basicTests")

def multiReadOnlyTests():
    sendMulReadOnlyTrx(numThreads=5)

def chainApiTests():
    # verify chain APIs can run in parallel with read-ony transactions
    runReadOnlyTrxAndRpcInParallel("chain", "get_info", "server_version")
    runReadOnlyTrxAndRpcInParallel("chain", "get_consensus_parameters", "chain_config")
    runReadOnlyTrxAndRpcInParallel("chain", "get_activated_protocol_features", "activated_protocol_features")
    runReadOnlyTrxAndRpcInParallel("chain", "get_block", "block_num", expectedValue=1, payload={"block_num_or_id":1})
    runReadOnlyTrxAndRpcInParallel("chain", "get_block_info", "block_num", expectedValue=1, payload={"block_num":1})
    runReadOnlyTrxAndRpcInParallel("chain", "get_account", "account_name", expectedValue=userAccountName, payload = {"account_name":userAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_code", "account_name", expectedValue=testAccountName, payload = {"account_name":testAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_code_hash", "account_name", expectedValue=testAccountName, payload = {"account_name":testAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_abi", "account_name", expectedValue=testAccountName, payload = {"account_name":testAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_raw_code_and_abi", "account_name", expectedValue=testAccountName, payload = {"account_name":testAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_raw_abi", "account_name", expectedValue=testAccountName, payload = {"account_name":testAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_producers", "rows", payload = {"json":"true","lower_bound":""})
    runReadOnlyTrxAndRpcInParallel("chain", "get_table_rows", "rows", payload = {"json":"true","code":"sysio","scope":"sysio","table":"global"})
    runReadOnlyTrxAndRpcInParallel("chain", "get_table_by_scope", fieldIn="rows", payload = {"json":"true","table":"noauth"})
    runReadOnlyTrxAndRpcInParallel("chain", "get_currency_balance", code=200, payload = {"code":"sysio.token", "account":testAccountName})
    runReadOnlyTrxAndRpcInParallel("chain", "get_currency_stats", fieldIn="SYS", payload = {"code":"sysio.token", "symbol":"SYS"})
    runReadOnlyTrxAndRpcInParallel("chain", "get_required_keys", code=400)
    runReadOnlyTrxAndRpcInParallel("chain", "get_transaction_id", code=400, payload = {"ref_block_num":"1"})
    runReadOnlyTrxAndRpcInParallel("chain", "get_producer_schedule", "active")

def netApiTests():
    # NET APIs
    runReadOnlyTrxAndRpcInParallel("net", "status", code=201, payload = "localhost")
    runReadOnlyTrxAndRpcInParallel("net", "connections", code=201)
    runReadOnlyTrxAndRpcInParallel("net", "connect", code=201, payload = "localhost")
    runReadOnlyTrxAndRpcInParallel("net", "disconnect", code=201, payload = "localhost")

def runEverythingParallel():
    threadList = [WorkerThread(multiReadOnlyTests), WorkerThread(chainApiTests), WorkerThread(netApiTests),
                  WorkerThread(mixedOpsTest)]
    for thr in threadList:
        thr.start()
    joinAll(threadList)
    assertApiNodeAlive("runEverythingParallel")

def fastTransactions():
    Print("fastTransactions")
    for i in range(1000):
        result = sendReadOnlyPayloadless()
        if not result[0]:
            assertFailedWithCode(result, TIMEOUT_CODES, "read-only payloadless under load")
            assert(result[1]['processed']['elapsed'] > max_trx_time)

def slowTransactions():
    Print("slowTransactions")
    for i in range(100):  # run fewer number than regular Payloadless so total running time is close
        result = sendReadOnlySlowPayloadless()
        if not result[0]:
            assertFailedWithCode(result, TIMEOUT_CODES, "read-only slow payloadless under load")
            assert(result[1]['processed']['elapsed'] > max_trx_time)

def foreverTransactions():
    Print("foreverTransactions")
    for i in range(5): # run fewer number than slowPayloadless so total running time is close
        assertFailedWithCode(sendReadOnlyForeverPayloadless(), TIMEOUT_CODES, "read-only forever payloadless")
        assertFailedWithCode(sendReadOnlySEGV(), (WASM_EXECUTION_ERROR,), "read-only segv")

def infiniteTransactions():
    Print("infiniteTransactions")
    for i in range(5): # run fewer number than slowPayloadless so total running time is close
        assertFailedWithCode(sendReadOnlyForeverInfinite(), TIMEOUT_CODES, "read-only infinite loop")
        assertFailedWithCode(sendReadOnlySEGV(), (WASM_EXECUTION_ERROR,), "read-only segv")

def timeoutTest():
    Print("timeoutTest")

    # Send a forever readonly transaction. It should timeout
    Print("Sending a forever read only transaction")
    results = sendReadOnlyForeverPayloadless()
    # Results look like
    '''
    ( False,
      {'processed':
        {
          ...
          'except': {'code': 3080004, 'name': 'tx_cpu_usage_exceeded', 'message': 'Transaction exceeded the current CPU usage limit imposed on the transaction' ...}
          ...
         }
      }
    )
    '''
    if Utils.Debug: Utils.Print(f"result: {results}")
    assertFailedWithCode(results, (TX_CPU_USAGE_EXCEEDED,), "forever read-only transaction")

    # Send multiple different speeds of read only transactions simutaneously
    # to trigger forever transactions are exhausted in read window but RETRYING
    # at next round.
    threadList = [WorkerThread(target) for target in (fastTransactions, slowTransactions, foreverTransactions,
                                                       infiniteTransactions, infiniteTransactions, foreverTransactions,
                                                       infiniteTransactions, foreverTransactions)]
    Print("Sending different speeds of read only transactions simultaneously")
    for thr in threadList:
        thr.start()
    joinAll(threadList)
    assertApiNodeAlive("timeoutTest")

try:
    startCluster()
    deployTestContracts()
    if args.api_node_read_mode == "irreversible":
        # the API node sees writes only once they are irreversible
        if not apiNode.waitForIrreversibleBlock(producerNode.getHeadBlockNum()):
            errorExit("API node LIB never reached the contract deployments")

    basicTests()

    if args.read_only_threads > 0: # Save test time. No need to run other tests if multi-threaded is not enabled
        for i in range(numTestRuns):
            multiReadOnlyTests()
            chainApiTests()
            netApiTests()
            mixedOpsTest()
            runEverythingParallel()

            # should be running under multiple threads
            timeoutTest()

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
