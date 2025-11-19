#!/usr/bin/env python3

import json
import copy

from TestHarness import Cluster, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys
from TestHarness.Cluster import PFSetupPolicy
from TestHarness.TestHelper import AppArgs

###############################################################
# nodeop_extra_packed_data_test
#
# Tests nodeop rejects a transaction with extra packed data
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit
cmdError=Utils.cmdError

args = TestHelper.parse_args({"--host","--port","-p","--defproducera_prvt_key","--defproducerb_prvt_key"
                              ,"--dump-error-details","--dont-launch","--keep-logs","-v","--leave-running"
                              ,"--sanity-test","--wallet-port","--unshared"})
server=args.host
port=args.port
debug=args.v
defproduceraPrvtKey=args.defproducera_prvt_key
defproducerbPrvtKey=args.defproducerb_prvt_key
dumpErrorDetails=args.dump_error_details
dontLaunch=args.dont_launch
pnodes=args.p
totalNodes=pnodes+1
sanityTest=args.sanity_test
walletPort=args.wallet_port

Utils.Debug=debug
localTest=True if server == TestHelper.LOCAL_HOST else False
cluster=Cluster(host=server, 
                port=port, 
                defproduceraPrvtKey=defproduceraPrvtKey, 
                defproducerbPrvtKey=defproducerbPrvtKey,
                unshared=args.unshared,
                keepRunning=args.leave_running,
                keepLogs=args.keep_logs)
walletMgr=WalletMgr(True, port=walletPort)
testSuccessful=False
dontBootstrap=sanityTest # intent is to limit the scope of the sanity test to just verifying that nodes can be started

WalletdName=Utils.SysWalletName
ClientName="clio"
timeout = .5 * 12 * 2 + 60 # time for finalization with 1 producer + 60 seconds padding
Utils.setIrreversibleTimeout(timeout)

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)
    Print("SERVER: %s" % (server))
    Print("PORT: %d" % (port))

    if localTest and not dontLaunch:
        Print("Stand up cluster")
        specificExtraNodeopArgs = {}
        associatedNodeLabels = {}
        if pnodes > 1:
            specificExtraNodeopArgs[pnodes - 1] = ""
        if pnodes > 3:
            specificExtraNodeopArgs[pnodes - 2] = ""

        if cluster.launch(totalNodes=totalNodes, 
                          pnodes=pnodes,
                          dontBootstrap=dontBootstrap,
                          activateIF=True,
                          specificExtraNodeopArgs=specificExtraNodeopArgs,
                          associatedNodeLabels=associatedNodeLabels) is False:
            cmdError("launcher")
            errorExit("Failed to stand up sys cluster.")
    else:
        Print("Collecting cluster info.")
        cluster.initializeNodes(defproduceraPrvtKey=defproduceraPrvtKey, defproducerbPrvtKey=defproducerbPrvtKey)
        Print("Stand up %s" % (WalletdName))
        print("Stand up walletd")
        if walletMgr.launch() is False:
            cmdError("%s" % (WalletdName))
            errorExit("Failed to stand up sys walletd.")
    
    if sanityTest:
        testSuccessful=True
        exit(0)

    Print("Validating system accounts after bootstrap")
    cluster.validateAccounts(None)

    accounts=createAccountKeys(2)
    if accounts is None:
        errorExit("FAILURE - create keys")
    testeraAccount=accounts[0]
    testeraAccount.name="testerxxxxxa"
    testerbAccount=accounts[1]
    testerbAccount.name="testerxxxxxb"

    testWalletName="test"
    Print("Creating wallet \"%s\"" % (testWalletName))
    walletAccounts=copy.deepcopy(cluster.defProducerAccounts)
    if dontLaunch:
        del walletAccounts["sysio"]
    testWallet = walletMgr.create(testWalletName, walletAccounts.values())

    Print("Wallet \"%s\" password=%s." % (testWalletName, testWallet.password.encode("utf-8")))

    all_acc = accounts + list( cluster.defProducerAccounts.values() )
    for account in all_acc:
        Print("Importing keys for account %s into wallet %s." % (account.name, testWallet.name))
        if not walletMgr.importKey(account, testWallet):
            cmdError("%s wallet import" % (ClientName))
            errorExit("Failed to import key for account %s" % (account.name))
    
    node=cluster.getNode(0)
    nonProdNode=cluster.getAllNodes()[-1]

    # Make pefproducera privileged so they can create accounts
    Print("Set privileged for account %s" % (cluster.defproduceraAccount.name))
    transId=node.setPriv(cluster.defproduceraAccount, cluster.sysioAccount, waitForTransBlock=True, exitOnError=True)

    Print("Create new account %s via %s" % (testeraAccount.name, cluster.defproduceraAccount.name))
    transId=node.createInitializeAccount(testeraAccount, cluster.defproduceraAccount, stakedDeposit=0, waitForTransBlock=False, exitOnError=True)

    Print("Create new account %s via %s" % (testerbAccount.name, cluster.defproduceraAccount.name))
    transId=nonProdNode.createInitializeAccount(testerbAccount, cluster.defproduceraAccount, stakedDeposit=0, waitForTransBlock=True, exitOnError=True)

    Print("Validating accounts after user accounts creation")
    accounts=[testeraAccount, testerbAccount]
    cluster.validateAccounts(accounts)

    trxNumber = 2 # one with extra data will fail
    for i in range(trxNumber):
        if i == 1:
            node = cluster.getNode(pnodes - 1)
        
        transferAmount="{0}.0 {1}".format(i + 1, CORE_SYMBOL)
        Print("Transfer funds %s from account %s to %s" % (transferAmount, cluster.defproduceraAccount.name, testeraAccount.name))
        trx = node.transferFunds(cluster.defproduceraAccount, testeraAccount, transferAmount, "test transfer", dontSend=True)

        cmdDesc = "convert pack_transaction"
        cmd     = "%s --pack-action-data '%s'" % (cmdDesc, json.dumps(trx))
        exitMsg = "failed to pack transaction: %s" % (trx)
        packedTrx = node.processClioCmd(cmd, cmdDesc, silentErrors=False, exitOnError=True, exitMsg=exitMsg)

        packed_trx_param = packedTrx["packed_trx"]
        if packed_trx_param is None:
            cmdError("packed_trx is None. Json: %s" % (packedTrx))
            errorExit("Can't find packed_trx in packed json")
        
        #adding random trailing data
        if i == 0:
            packedTrx["packed_trx"] = packed_trx_param + "00000000"

        sentTrx = node.processUrllibRequest("chain", "send_transaction2", {"transaction": packedTrx}, silentErrors=True, exitOnError=False)
        Print("sent transaction json: %s" % (sentTrx))
        if i == 0:
            assert sentTrx is None, "Should have failed to send transaction with extra data"
            assert node.findInLog("packed_transaction contains extra data"), "Should have found failed log message"
            continue

        trx_id = sentTrx["payload"]["transaction_id"]
        assert node.waitForTransactionInBlock(trx_id), "Transaction %s didn't get into a block" % (trx_id)

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
