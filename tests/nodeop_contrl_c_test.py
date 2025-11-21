#!/usr/bin/env python3

import signal
import time

from TestHarness import Cluster, Node, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys

###############################################################
# nodeop_contrl_c_lr_test
#
# This test sets up one producing nodes and one "bridge" node using test_control_api_plugin. We send
# transactions to the bridge node. After the accounts are initialized and allocated with symbols, we
# kill the producing node. Then we flood hundreds transactions to the bridge node. At the end, we
# kill the bridge. It tests that no crashes happen when the nodes are killed.
#
###############################################################

Print = Utils.Print
errorExit=Utils.errorExit

args = TestHelper.parse_args({"--wallet-port","--activate-if","-v","--unshared"})

cluster=Cluster(unshared=args.unshared)
totalProducerNodes=2
totalNonProducerNodes=1
totalNodes=totalProducerNodes+totalNonProducerNodes
maxActiveProducers=2
totalProducers=maxActiveProducers
activateIF=args.activate_if
walletPort=args.wallet_port
walletMgr=WalletMgr(True, port=walletPort)
producerEndpoint = '127.0.0.1:8888'
httpServerAddress = '127.0.0.1:8889'

testSuccessful=False
trxGenLauncher=None

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    specificExtraNodeopArgs = {}
    # producer nodes will be mapped to 0 through totalProducerNodes-1, so the number totalProducerNodes will be the non-producing node
    specificExtraNodeopArgs[totalProducerNodes] = "--plugin sysio::producer_plugin --plugin sysio::chain_api_plugin --plugin sysio::http_plugin "
    "--plugin sysio::producer_api_plugin "
    extraNodeopArgs = " --http-max-response-time-ms 990000 "

    # ***   setup topogrophy   ***

    # "bridge" shape connects defprocera through defproducerk (in node0) to each other and defproducerl through defproduceru (in node01)
    # and the only connection between those 2 groups is through the bridge node

    if cluster.launch(prodCount=1, topo="bridge", pnodes=totalProducerNodes,
                      totalNodes=totalNodes, totalProducers=totalProducers,
                      activateIF=activateIF,
                      specificExtraNodeopArgs=specificExtraNodeopArgs,
                      extraNodeopArgs=extraNodeopArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up sys cluster.")
    Print("Validating system accounts after bootstrap")
    cluster.validateAccounts(None)

    prodNode = cluster.getNode(0)
    nonProdNode = cluster.getNode(2)

    accounts=createAccountKeys(2)
    if accounts is None:
        Utils.errorExit("FAILURE - create keys")

    accounts[0].name="tester111111"
    accounts[1].name="tester222222"

    account1PrivKey = accounts[0].activePrivateKey
    account2PrivKey = accounts[1].activePrivateKey

    testWalletName="test"

    Print("Creating wallet \"%s\"." % (testWalletName))
    testWallet=walletMgr.create(testWalletName, [cluster.sysioAccount,accounts[0],accounts[1]])

    # create accounts via sysio as otherwise a bid is needed
    for account in accounts:
        Print("Create new account %s via %s" % (account.name, cluster.sysioAccount.name))
        trans=nonProdNode.createInitializeAccount(account, cluster.sysioAccount, stakedDeposit=0, waitForTransBlock=True, nodeOwner=cluster.carlAccount, stakeNet=1000, stakeCPU=1000, buyRAM=1000, exitOnError=True)
        transferAmount="100000000.0000 {0}".format(CORE_SYMBOL)
        Print("Transfer funds %s from account %s to %s" % (transferAmount, cluster.sysioAccount.name, account.name))
        nonProdNode.transferFunds(cluster.sysioAccount, account, transferAmount, "test transfer", waitForTransBlock=True)

    testSuccessful = prodNode.kill(signal.SIGTERM)
    if not testSuccessful:
        TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, killSysInstances=True, killWallet=True, keepLogs=True, cleanRun=True, dumpErrorDetails=True)
        errorExit("Failed to kill the producer node")

    #Reset test success flag for next check
    testSuccessful=False

    Print("Configure and launch txn generators")
    targetTpsPerGenerator = 10
    testTrxGenDurationSec=60
    trxGeneratorCnt=1
    cluster.launchTrxGenerators(contractOwnerAcctName=cluster.sysioAccount.name, acctNamesList=[accounts[0].name,accounts[1].name],
                                acctPrivKeysList=[account1PrivKey,account2PrivKey], nodeId=nonProdNode.nodeId, tpsPerGenerator=targetTpsPerGenerator,
                                numGenerators=trxGeneratorCnt, durationSec=testTrxGenDurationSec, waitToComplete=False)

    Print("Give txn generator time to spin up and begin producing trxs")
    time.sleep(10)

    Print("Kill non-producer bridge node")
    testSuccessful = nonProdNode.kill(signal.SIGTERM)

    if not testSuccessful:
        TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=True)
        errorExit("Failed to kill the seed node")

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=True)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
