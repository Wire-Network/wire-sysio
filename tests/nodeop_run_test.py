#!/usr/bin/env python3

from TestHarness import Account, Cluster, Node, ReturnType, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys
from pathlib import Path

import decimal
import re
import json
import os
import sys
from enum import Enum

###############################################################
# nodeop_run_test
#
# General test that tests a wide range of general use actions around nodeop and kiod
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit
cmdError=Utils.cmdError

args = TestHelper.parse_args({"--host","--port","--prod-count","--defproducera_prvt_key","--defproducerb_prvt_key"
                              ,"--dump-error-details","--dont-launch","--keep-logs","-v","--leave-running","--only-bios"
                              ,"--sanity-test","--wallet-port", "--error-log-path", "--unshared"})
server=args.host
port=args.port
debug=args.v
defproduceraPrvtKey=args.defproducera_prvt_key
defproducerbPrvtKey=args.defproducerb_prvt_key
dumpErrorDetails=args.dump_error_details
dontLaunch=args.dont_launch
prodCount=args.prod_count
onlyBios=args.only_bios
sanityTest=args.sanity_test
walletPort=args.wallet_port

Utils.Debug=debug
localTest=True if server == TestHelper.LOCAL_HOST else False
cluster=Cluster(host=server, port=port, defproduceraPrvtKey=defproduceraPrvtKey, defproducerbPrvtKey=defproducerbPrvtKey,unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
errFileName=f"{cluster.nodeopLogPath}/node_00/stderr.txt"
if args.error_log_path:
    errFileName=args.error_log_path
walletMgr=WalletMgr(True, port=walletPort, keepRunning=args.leave_running, keepLogs=args.keep_logs)
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

        abs_path = os.path.abspath(os.getcwd() + '/unittests/contracts/sysio.token/sysio.token.abi')
        traceNodeopArgs=" --http-max-response-time-ms 990000 --trace-rpc-abi sysio.token=" + abs_path
        extraNodeopArgs=traceNodeopArgs + " --plugin sysio::prometheus_plugin --database-map-mode mapped_private "
        specificNodeopInstances={0: "bin/nodeop"}
        if cluster.launch(totalNodes=2, prodCount=prodCount, onlyBios=onlyBios, dontBootstrap=dontBootstrap, extraNodeopArgs=extraNodeopArgs, specificNodeopInstances=specificNodeopInstances) is False:
            cmdError("launcher")
            errorExit("Failed to stand up sys cluster.")
    else:
        Print("Collecting cluster info.")
        cluster.initializeNodes(defproduceraPrvtKey=defproduceraPrvtKey, defproducerbPrvtKey=defproducerbPrvtKey)
        Print("Stand up %s" % (WalletdName))
        if walletMgr.launch() is False:
            cmdError("%s" % (WalletdName))
            errorExit("Failed to stand up sys walletd.")

    if sanityTest:
        testSuccessful=True
        exit(0)

    Print("Validating system accounts after bootstrap")
    cluster.validateAccounts(None)

    accounts=createAccountKeys(4)
    if accounts is None:
        errorExit("FAILURE - create keys")
    testeraAccount=accounts[0]
    testeraAccount.name="testera11111"
    currencyAccount=accounts[1]
    currencyAccount.name="currency1111"
    exchangeAccount=accounts[2]
    exchangeAccount.name="exchange1111"
    # account to test newaccount with authority
    testerbAccount=accounts[3]
    testerbAccount.name="testerb11111"
    testerbOwner = testerbAccount.ownerPublicKey
    testerbAccount.ownerPublicKey = '{"threshold":1, "accounts":[{"permission":{"actor": "' + testeraAccount.name + '", "permission":"owner"}, "weight": 1}],"keys":[{"key": "' +testerbOwner +  '", "weight": 1}],"waits":[]}'

    PRV_KEY1=testeraAccount.ownerPrivateKey
    PUB_KEY1=testeraAccount.ownerPublicKey
    PRV_KEY2=currencyAccount.ownerPrivateKey
    PUB_KEY2=currencyAccount.ownerPublicKey
    PRV_KEY3=exchangeAccount.activePrivateKey
    PUB_KEY3=exchangeAccount.activePublicKey

    testeraAccount.activePrivateKey=currencyAccount.activePrivateKey=PRV_KEY3
    testeraAccount.activePublicKey=currencyAccount.activePublicKey=PUB_KEY3

    exchangeAccount.ownerPrivateKey=PRV_KEY2
    exchangeAccount.ownerPublicKey=PUB_KEY2

    testWalletName="test"
    Print("Creating wallet \"%s\"." % (testWalletName))
    walletAccounts=[cluster.defproduceraAccount,cluster.defproducerbAccount]
    if not dontLaunch:
        walletAccounts.append(cluster.sysioAccount)
    testWallet=walletMgr.create(testWalletName, walletAccounts)

    Print("Wallet \"%s\" password=%s." % (testWalletName, testWallet.password.encode("utf-8")))

    for account in accounts:
        Print("Importing keys for account %s into wallet %s." % (account.name, testWallet.name))
        if not walletMgr.importKey(account, testWallet):
            cmdError("%s wallet import" % (ClientName))
            errorExit("Failed to import key for account %s" % (account.name))

    defproduceraWalletName="defproducera"
    Print("Creating wallet \"%s\"." % (defproduceraWalletName))
    defproduceraWallet=walletMgr.create(defproduceraWalletName)

    Print("Wallet \"%s\" password=%s." % (defproduceraWalletName, defproduceraWallet.password.encode("utf-8")))

    defproduceraAccount=cluster.defproduceraAccount
    defproducerbAccount=cluster.defproducerbAccount

    Print("Importing keys for account %s into wallet %s." % (defproduceraAccount.name, defproduceraWallet.name))
    if not walletMgr.importKey(defproduceraAccount, defproduceraWallet):
        cmdError("%s wallet import" % (ClientName))
        errorExit("Failed to import key for account %s" % (defproduceraAccount.name))

    Print("Locking wallet \"%s\"." % (testWallet.name))
    if not walletMgr.lockWallet(testWallet):
        cmdError("%s wallet lock" % (ClientName))
        errorExit("Failed to lock wallet %s" % (testWallet.name))

    Print("Unlocking wallet \"%s\"." % (testWallet.name))
    if not walletMgr.unlockWallet(testWallet):
        cmdError("%s wallet unlock" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (testWallet.name))

    Print("Locking all wallets.")
    if not walletMgr.lockAllWallets():
        cmdError("%s wallet lock_all" % (ClientName))
        errorExit("Failed to lock all wallets")

    Print("Unlocking wallet \"%s\"." % (testWallet.name))
    if not walletMgr.unlockWallet(testWallet):
        cmdError("%s wallet unlock" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (testWallet.name))

    Print("Getting open wallet list.")
    wallets=walletMgr.getOpenWallets()
    if len(wallets) == 0 or wallets[0] != testWallet.name or len(wallets) > 1:
        Print("FAILURE - wallet list did not include %s" % (testWallet.name))
        errorExit("Unexpected wallet list: %s" % (wallets))

    Print("Getting wallet keys.")
    actualKeys=walletMgr.getKeys(testWallet)
    expectedkeys=[]
    for account in accounts:
        expectedkeys.append(account.ownerPrivateKey)
        expectedkeys.append(account.activePrivateKey)
    noMatch=list(set(expectedkeys) - set(actualKeys))
    if len(noMatch) > 0:
        errorExit("FAILURE - wallet keys did not include %s" % (noMatch), raw=True)

    Print("Locking all wallets.")
    if not walletMgr.lockAllWallets():
        cmdError("%s wallet lock_all" % (ClientName))
        errorExit("Failed to lock all wallets")

    Print("Unlocking wallet \"%s\"." % (defproduceraWallet.name))
    if not walletMgr.unlockWallet(defproduceraWallet):
        cmdError("%s wallet unlock" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (defproduceraWallet.name))

    Print("Unlocking wallet \"%s\"." % (testWallet.name))
    if not walletMgr.unlockWallet(testWallet):
        cmdError("%s wallet unlock" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (testWallet.name))

    Print("Getting wallet keys.")
    actualKeys=walletMgr.getKeys(defproduceraWallet)
    expectedkeys=[defproduceraAccount.ownerPrivateKey]
    noMatch=list(set(expectedkeys) - set(actualKeys))
    if len(noMatch) > 0:
        errorExit("FAILURE - wallet keys did not include %s" % (noMatch), raw=True)

    node=cluster.getNode(1)

    Print("Validating accounts before user accounts creation")
    cluster.validateAccounts(None)

    # Make pefproducera privileged so they can create accounts
    Print("Set privileged for account %s" % (cluster.defproduceraAccount.name))
    transId=node.setPriv(cluster.defproduceraAccount, cluster.sysioAccount, waitForTransBlock=True, exitOnError=True)

    Print("Create new account %s via %s" % (testeraAccount.name, cluster.defproduceraAccount.name))
    transId=node.createInitializeAccount(testeraAccount, cluster.defproduceraAccount, nodeOwner=cluster.carlAccount, stakedDeposit=0, waitForTransBlock=True, exitOnError=True)

    Print("Create new account %s via %s" % (testerbAccount.name, cluster.defproduceraAccount.name))
    transId=node.createInitializeAccount(testerbAccount, cluster.defproduceraAccount, nodeOwner=cluster.carlAccount, stakedDeposit=0, waitForTransBlock=False, exitOnError=True)

    Print("Create new account %s via %s" % (currencyAccount.name, cluster.defproduceraAccount.name))
    transId=node.createInitializeAccount(currencyAccount, cluster.defproduceraAccount, nodeOwner=cluster.carlAccount, buyRAM=200000, stakedDeposit=5000, exitOnError=True)

    Print("Create new account %s via %s" % (exchangeAccount.name, cluster.defproduceraAccount.name))
    transId=node.createInitializeAccount(exchangeAccount, cluster.defproduceraAccount, nodeOwner=cluster.carlAccount, buyRAM=200000, waitForTransBlock=True, exitOnError=True)

    Print("Validating accounts after user accounts creation")
    accounts=[testeraAccount, currencyAccount, exchangeAccount]
    cluster.validateAccounts(accounts)

    Print("Verify account %s" % (testeraAccount))
    if not node.verifyAccount(testeraAccount):
        errorExit("FAILURE - account creation failed.", raw=True)

    transferAmount="97.5321 {0}".format(CORE_SYMBOL)
    Print("Transfer funds %s from account %s to %s" % (transferAmount, defproduceraAccount.name, testeraAccount.name))
    node.transferFunds(defproduceraAccount, testeraAccount, transferAmount, "test transfer", waitForTransBlock=True)

    expectedAmount=transferAmount
    Print("Verify transfer, Expected: %s" % (expectedAmount))
    actualAmount=node.getAccountSysBalanceStr(testeraAccount.name)
    if expectedAmount != actualAmount:
        cmdError("FAILURE - transfer failed")
        errorExit("Transfer verification failed. Excepted %s, actual: %s" % (expectedAmount, actualAmount))

    transferAmount="0.0100 {0}".format(CORE_SYMBOL)
    Print("Force transfer funds %s from account %s to %s" % (
        transferAmount, defproduceraAccount.name, testeraAccount.name))
    node.transferFunds(defproduceraAccount, testeraAccount, transferAmount, "test transfer", force=True, waitForTransBlock=True)

    expectedAmount="97.5421 {0}".format(CORE_SYMBOL)
    Print("Verify transfer, Expected: %s" % (expectedAmount))
    actualAmount=node.getAccountSysBalanceStr(testeraAccount.name)
    if expectedAmount != actualAmount:
        cmdError("FAILURE - transfer failed")
        errorExit("Transfer verification failed. Excepted %s, actual: %s" % (expectedAmount, actualAmount))

    Print("Validating accounts after some user transactions")
    accounts=[testeraAccount, currencyAccount, exchangeAccount]
    cluster.validateAccounts(accounts)

    Print("Locking all wallets.")
    if not walletMgr.lockAllWallets():
        cmdError("%s wallet lock_all" % (ClientName))
        errorExit("Failed to lock all wallets")

    Print("Unlocking wallet \"%s\"." % (testWallet.name))
    if not walletMgr.unlockWallet(testWallet):
        cmdError("%s wallet unlock" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (testWallet.name))

    transferAmount="97.5311 {0}".format(CORE_SYMBOL)
    Print("Transfer funds %s from account %s to %s" % (
        transferAmount, testeraAccount.name, currencyAccount.name))
    trans=node.transferFunds(testeraAccount, currencyAccount, transferAmount, "test transfer a->b", waitForTransBlock=True)
    transId=Node.getTransId(trans)

    expectedAmount="98.0311 {0}".format(CORE_SYMBOL) # 5000 initial deposit
    Print("Verify transfer, Expected: %s" % (expectedAmount))
    actualAmount=node.getAccountSysBalanceStr(currencyAccount.name)
    if expectedAmount != actualAmount:
        cmdError("FAILURE - transfer failed")
        errorExit("Transfer verification failed. Excepted %s, actual: %s" % (expectedAmount, actualAmount))

    node.waitForTransactionInBlock(transId)

    transaction=node.getTransaction(transId, exitOnError=True, delayedRetry=False)

    typeVal=None
    amountVal=None
    key=""
    try:
        key = "[actions][0][action]"
        typeVal = transaction["actions"][0]["action"]
        key = "[actions][0][params][quantity]"
        amountVal = transaction["actions"][0]["params"]["quantity"]
        amountVal = int(decimal.Decimal(amountVal.split()[0]) * 10000)
    except (TypeError, KeyError) as e:
        Print("transaction%s not found. Transaction: %s" % (key, transaction))
        raise

    if typeVal != "transfer" or amountVal != 975311:
        errorExit("FAILURE - get transaction trans_id failed: %s %s %s" % (transId, typeVal, amountVal), raw=True)

    Print("Currency Contract Tests")
    Print("verify no contract in place")
    Print("Get code hash for account %s" % (currencyAccount.name))
    node=cluster.getNode(0)
    codeHash=node.getAccountCodeHash(currencyAccount.name)
    if codeHash is None:
        cmdError("%s get code currency1111" % (ClientName))
        errorExit("Failed to get code hash for account %s" % (currencyAccount.name))
    hashNum=int(codeHash, 16)
    if hashNum != 0:
        errorExit("FAILURE - get code currency1111 failed", raw=True)

    contractDir="unittests/contracts/sysio.token"
    wasmFile="sysio.token.wasm"
    abiFile="sysio.token.abi"
    Print("Publish contract")
    trans=node.publishContract(currencyAccount, contractDir, wasmFile, abiFile, waitForTransBlock=True)
    if trans is None:
        cmdError("%s set contract currency1111" % (ClientName))
        errorExit("Failed to publish contract.")

    Print("Get code hash for account %s" % (currencyAccount.name))
    codeHash = node.getAccountCodeHash(currencyAccount.name)
    if codeHash is None:
        cmdError("%s get code currency1111" % (ClientName))
        errorExit("Failed to get code hash for account %s" % (currencyAccount.name))
    hashNum = int(codeHash, 16)
    if hashNum == 0:
        errorExit("FAILURE - get code currency1111 failed", raw=True)

    Print("push create action to currency1111 contract")
    contract="currency1111"
    action="create"
    data="{\"issuer\":\"currency1111\",\"maximum_supply\":\"100000.0000 CUR\",\"can_freeze\":\"0\",\"can_recall\":\"0\",\"can_whitelist\":\"0\"}"
    opts="--permission currency1111@active"
    trans=node.pushMessage(contract, action, data, opts)
    try:
        assert(trans)
        assert(trans[0])
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed push create action to currency1111 contract assertion. %s" % (trans))
        raise

    Print("push issue action to currency1111 contract")
    action="issue"
    data="{\"to\":\"currency1111\",\"quantity\":\"100000.0000 CUR\",\"memo\":\"issue\"}"
    opts="--permission currency1111@active"
    trans=node.pushMessage(contract, action, data, opts)
    try:
        assert(trans)
        assert(trans[0])
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed push issue action to currency1111 contract assertion. %s" % (trans))
        raise

    Print("Verify currency1111 contract has proper initial balance (via get table)")
    contract="currency1111"
    table="accounts"
    row0=node.getTableRow(contract, currencyAccount.name, table, 0)
    try:
        assert(row0)
        assert(row0["balance"] == "100000.0000 CUR")
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed get table row assertion. %s" % (row0))
        raise

    Print("Verify currency1111 contract has proper initial balance (via get currency1111 balance)")
    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    expected="100000.0000 CUR"
    actual=amountStr
    if actual != expected:
        errorExit("FAILURE - currency1111 balance check failed. Expected: %s, Recieved %s" % (expected, actual), raw=True)

    Print("Verify currency1111 contract has proper total supply of CUR (via get currency1111 stats)")
    res=node.getCurrencyStats(contract, "CUR", exitOnError=True)
    try:
        assert(res["CUR"]["supply"] == "100000.0000 CUR")
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed get currecy stats assertion. %s" % (res))
        raise

    dupRejected=False
    dupTransAmount=10
    totalTransfer=dupTransAmount
    contract="currency1111"
    action="transfer"
    for _ in range(5):
        Print("push transfer action to currency1111 contract")
        data="{\"from\":\"currency1111\",\"to\":\"defproducera\",\"quantity\":"
        data +="\"00.00%s CUR\",\"memo\":\"test\"}" % (dupTransAmount)
        opts="--permission currency1111@active"
        trans=node.pushMessage(contract, action, data, opts)
        if trans is None or not trans[0]:
            cmdError("%s push message currency1111 transfer" % (ClientName))
            errorExit("Failed to push message to currency1111 contract")
        transId=Node.getTransId(trans[1])

        Print("push duplicate transfer action to currency1111 contract")
        transDuplicate=node.pushMessage(contract, action, data, opts, True)
        if transDuplicate is not None and transDuplicate[0]:
            transDuplicateId=Node.getTransId(transDuplicate[1])
            if transId != transDuplicateId:
                Print("%s push message currency1111 duplicate transfer incorrectly accepted, but they were generated with different transaction ids, this is a timing setup issue, trying again" % (ClientName))
                # add the transfer that wasn't supposed to work
                totalTransfer+=dupTransAmount
                dupTransAmount+=1
                # add the new first transfer that is expected to work
                totalTransfer+=dupTransAmount
                continue
            else:
                cmdError("%s push message currency1111 transfer, \norig: %s \ndup: %s" % (ClientName, trans, transDuplicate))
            errorExit("Failed to reject duplicate message for currency1111 contract")
        else:
            dupRejected=True
            break

    if not dupRejected:
        errorExit("Failed to reject duplicate message for currency1111 contract")

    Print("verify transaction exists")
    if not node.waitForTransactionInBlock(transId):
        cmdError("%s get transaction trans_id" % (ClientName))
        errorExit("Failed to verify push message transaction id.")

    Print("read current contract balance")
    amountStr=node.getTableAccountBalance("currency1111", defproduceraAccount.name)

    expectedDefproduceraBalance="0.00%s CUR" % (totalTransfer)
    actual=amountStr
    if actual != expectedDefproduceraBalance:
        errorExit("FAILURE - Wrong currency1111 balance (expected=%s, actual=%s)" % (expectedDefproduceraBalance, actual), raw=True)

    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    expExtension=100-totalTransfer
    expectedCurrency1111Balance="99999.99%s CUR" % (expExtension)
    actual=amountStr
    if actual != expectedCurrency1111Balance:
        errorExit("FAILURE - Wrong currency1111 balance (expected=%s, actual=%s)" % (expectedCurrency1111Balance, actual), raw=True)

    amountStr=node.getCurrencyBalance("currency1111", currencyAccount.name, "CUR")
    try:
        assert(actual)
        assert(isinstance(actual, str))
        actual=amountStr.strip()
        assert(expectedCurrency1111Balance == actual)
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed get currecy balance assertion. (expected=<%s>, actual=<%s>)" % (expectedCurrency1111Balance, actual))
        raise

    Print("Test for block decoded packed transaction (issue 2932)")
    blockNum=node.getBlockNumByTransId(transId)
    assert(blockNum)
    block=node.getBlock(blockNum, exitOnError=True)

    transactions=None
    try:
        transactions=block["transactions"]
        assert(transactions)
    except (AssertionError, TypeError, KeyError) as _:
        Print("FAILURE - Failed to parse block. %s" % (block))
        raise

    myTrans=None
    for trans in transactions:
        assert(trans)
        try:
            myTransId=trans["trx"]["id"]
            if transId == myTransId:
                myTrans=trans["trx"]["transaction"]
                assert(myTrans)
                break
        except (AssertionError, TypeError, KeyError) as _:
            Print("FAILURE - Failed to parse block transactions. %s" % (trans))
            raise

    assert(myTrans)
    try:
        assert(myTrans["actions"][0]["name"] == "transfer")
        assert(myTrans["actions"][0]["account"] == "currency1111")
        assert(myTrans["actions"][0]["authorization"][0]["actor"] == "currency1111")
        assert(myTrans["actions"][0]["authorization"][0]["permission"] == "active")
        assert(myTrans["actions"][0]["data"]["from"] == "currency1111")
        assert(myTrans["actions"][0]["data"]["to"] == "defproducera")
        assert(myTrans["actions"][0]["data"]["quantity"] == "0.00%s CUR" % (dupTransAmount))
        assert(myTrans["actions"][0]["data"]["memo"] == "test")
    except (AssertionError, TypeError, KeyError) as _:
        Print("FAILURE - Failed to parse block transaction. %s" % (myTrans))
        raise

    Print("Unlocking wallet \"%s\"." % (defproduceraWallet.name))
    if not walletMgr.unlockWallet(defproduceraWallet):
        cmdError("%s wallet unlock" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (defproduceraWallet.name))

    Print("push transfer action to currency1111 contract that would go negative")
    contract="currency1111"
    action="transfer"
    data="{\"from\":\"defproducera\",\"to\":\"currency1111\",\"quantity\":"
    data +="\"00.0151 CUR\",\"memo\":\"test\"}"
    opts="--permission defproducera@active"
    trans=node.pushMessage(contract, action, data, opts, True)
    if trans is None or trans[0]:
        cmdError("%s push message currency1111 transfer should have failed" % (ClientName))
        errorExit("Failed to reject invalid transfer message to currency1111 contract")

    Print("read current contract balance")
    amountStr=node.getTableAccountBalance("currency1111", defproduceraAccount.name)

    actual=amountStr
    if actual != expectedDefproduceraBalance:
        errorExit("FAILURE - Wrong currency1111 balance (expected=%s, actual=%s)" % (expectedDefproduceraBalance, actual), raw=True)

    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    actual=amountStr
    if actual != expectedCurrency1111Balance:
        errorExit("FAILURE - Wrong currency1111 balance (expected=%s, actual=%s)" % (expectedCurrency1111Balance, actual), raw=True)

    Print("push another transfer action to currency1111 contract")
    contract="currency1111"
    action="transfer"
    data="{\"from\":\"defproducera\",\"to\":\"currency1111\",\"quantity\":"
    data +="\"00.00%s CUR\",\"memo\":\"test\"}" % (totalTransfer)
    opts="--permission defproducera@active"
    trans=node.pushMessage(contract, action, data, opts)
    if trans is None or not trans[0]:
        cmdError("%s push message currency1111 transfer" % (ClientName))
        errorExit("Failed to push message to currency1111 contract")
    transId=Node.getTransId(trans[1])

    Print("read current contract balance")
    amountStr=node.getCurrencyBalance("currency1111", defproduceraAccount.name, "CUR")
    expected="0.0000 CUR"
    try:
        actual=amountStr.strip()
        assert(expected == actual or not actual)
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed get currecy balance assertion. (expected=<%s>, actual=<%s>)" % (str(expected), str(actual)))
        raise

    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    expected="100000.0000 CUR"
    actual=amountStr
    if actual != expected:
        errorExit("FAILURE - Wrong currency1111 balance (expected=%s, actual=%s)" % (str(expected), str(actual)), raw=True)

    Print("push transfer action to currency1111 contract that would go negative")
    contract="currency1111"
    action="transfer"
    data="{\"from\":\"defproducera\",\"to\":\"currency1111\",\"quantity\":"
    data +="\"00.0025 CUR\",\"memo\":\"test\"}"
    opts="--permission defproducera@active --use-old-send-rpc" # --use-old-send-rpc flag used to retain a test of /v1/chain/send_transaction
    trans=node.pushMessage(contract, action, data, opts, True)
    if trans is None or trans[0]:
        cmdError("%s push message currency1111 transfer should have failed" % (ClientName))
        errorExit("Failed to reject invalid transfer message to currency1111 contract")

    Print("read current contract balance")
    amountStr=node.getCurrencyBalance("currency1111", defproduceraAccount.name, "CUR")
    expected="0.0000 CUR"
    try:
        actual=amountStr.strip()
        assert(expected == actual or not actual)
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed get currecy balance assertion. (expected=<%s>, actual=<%s>)" % (str(expected), str(actual)))
        raise

    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    expected="100000.0000 CUR"
    actual=amountStr
    if actual != expected:
        errorExit("FAILURE - Wrong currency1111 balance (expected=%s, actual=%s)" % (str(expected), str(actual)), raw=True)

    # Test skip sign with unpacked action data
    Print("push transfer action to currency1111 contract with sign skipping and unpack action data options enabled")
    data="{\"from\":\"currency1111\",\"to\":\"defproducera\",\"quantity\":"
    data +="\"00.0001 CUR\",\"memo\":\"test\"}"
    opts="-s -d -u --permission currency1111@active"
    trans=node.pushMessage(contract, action, data, opts, expectTrxTrace=False)

    try:
        assert(not trans[1]["signatures"])
    except (AssertionError, KeyError) as _:
        Print("ERROR: Expected signatures array to be empty due to skipping option enabled.")
        raise

    try:
        assert(trans[1]["actions"][0]["data"]["from"] == "currency1111")
        assert(trans[1]["actions"][0]["data"]["to"] == "defproducera")
        assert(trans[1]["actions"][0]["data"]["quantity"] == "0.0001 CUR")
        assert(trans[1]["actions"][0]["data"]["memo"] == "test")
    except (AssertionError, KeyError) as _:
        Print("ERROR: Expecting unpacked data fields on push transfer action json result.")
        raise

    result=node.pushTransaction(trans[1], None)

    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    expected="99999.9999 CUR"
    actual=amountStr
    if actual != expected:
        errorExit("FAILURE - Wrong currency1111 balance (expectedgma=%s, actual=%s)" % (str(expected), str(actual)), raw=True)

    # Test skip sign with packed action data
    Print("push transfer action to currency1111 contract with sign skipping option enabled")
    data="{\"from\":\"currency1111\",\"to\":\"defproducera\",\"quantity\":"
    data +="\"00.0002 CUR\",\"memo\":\"test packed\"}"
    opts="-s -d --permission currency1111@active"
    trans=node.pushMessage(contract, action, data, opts, expectTrxTrace=False)

    try:
        assert(not trans[1]["signatures"])
    except (AssertionError, KeyError) as _:
        Print("ERROR: Expected signatures array to be empty due to skipping option enabled.")
        raise

    try:
        data = trans[1]["actions"][0]["data"]
        Print(f"Action data: {data}")
        assert data == "1042081e4d75af4660ae423ad15b974a020000000000000004435552000000000b74657374207061636b6564"
    except (AssertionError, KeyError) as _:
        Print("ERROR: Expecting packed data on push transfer action json result.")
        raise

    result=node.pushTransaction(trans[1], None)

    amountStr=node.getTableAccountBalance("currency1111", currencyAccount.name)

    expected="99999.9997 CUR"
    actual=amountStr
    if actual != expected:
        errorExit("FAILURE - Wrong currency1111 balance (expectedgma=%s, actual=%s)" % (str(expected), str(actual)), raw=True)


    Print("---- Test for signing transaction ----")
    testeraAccountAmountBeforeTrx=node.getAccountSysBalanceStr(testeraAccount.name)
    currencyAccountAmountBeforeTrx=node.getAccountSysBalanceStr(currencyAccount.name)

    xferAmount="1.2345 {0}".format(CORE_SYMBOL)
    unsignedTrxRet = node.transferFunds(currencyAccount, testeraAccount, xferAmount, "unsigned trx", force=False, waitForTransBlock=False, exitOnError=True, reportStatus=False, sign=False, dontSend=True, expiration=None, skipSign=True)
    unsignedTrxJsonFile = "unsigned_trx_file"
    with open(unsignedTrxJsonFile, 'w') as outfile:
        json.dump(unsignedTrxRet, outfile)
    testeraAccountAmountAftrTrx=node.getAccountSysBalanceStr(testeraAccount.name)
    currencyAccountAmountAftrTrx=node.getAccountSysBalanceStr(currencyAccount.name)
    try:
        assert(testeraAccountAmountBeforeTrx == testeraAccountAmountAftrTrx)
        assert(currencyAccountAmountBeforeTrx == currencyAccountAmountAftrTrx)
    except (AssertionError) as _:
        Print("ERROR: Expecting transfer is not executed.")
        raise

    signCmd = "sign --public-key {0} {1} -p".format(currencyAccount.activePublicKey, unsignedTrxJsonFile)
    node.processClioCmd(signCmd, "Sign and push a transaction", False, True)
    os.remove(unsignedTrxJsonFile)

    testeraAccountAmountAfterSign=node.getAccountSysBalanceStr(testeraAccount.name)
    currencyAccountAmountAfterSign=node.getAccountSysBalanceStr(currencyAccount.name)
    try:
        assert(Utils.addAmount(testeraAccountAmountAftrTrx, xferAmount) == testeraAccountAmountAfterSign)
        assert(Utils.deduceAmount(currencyAccountAmountAftrTrx, xferAmount) == currencyAccountAmountAfterSign)
    except (AssertionError) as _:
        Print("ERROR: Expecting transfer has been executed with exact amount.")
        raise

    Print("Locking wallet \"%s\"." % (defproduceraWallet.name))
    if not walletMgr.lockWallet(defproduceraWallet):
        cmdError("%s wallet lock" % (ClientName))
        errorExit("Failed to lock wallet %s" % (defproduceraWallet.name))


    simpleDB = Account("simpledb")
    contractDir="contracts/simpledb"
    wasmFile="simpledb.wasm"
    abiFile="simpledb.abi"
    Print("Setting simpledb contract without simpledb account was causing core dump in %s." % (ClientName))
    Print("Verify %s generates an error, but does not core dump." % (ClientName))
    retMap=node.publishContract(simpleDB, contractDir, wasmFile, abiFile, shouldFail=True)
    if retMap is None:
        errorExit("Failed to publish, but should have returned a details map")
    if retMap["returncode"] == 0 or retMap["returncode"] == 139: # 139 SIGSEGV
        errorExit("FAILURE - set contract simpledb failed", raw=True)
    else:
        Print("Test successful, %s returned error code: %d" % (ClientName, retMap["returncode"]))

    Print("set permission")
    pType="transfer"
    requirement="active"
    trans=node.setPermission(testeraAccount, currencyAccount, pType, requirement, waitForTransBlock=True, exitOnError=True)

    Print("remove permission")
    requirement="NULL"
    trans=node.setPermission(testeraAccount, currencyAccount, pType, requirement, waitForTransBlock=True, exitOnError=True)

    Print("Locking all wallets.")
    if not walletMgr.lockAllWallets():
        cmdError("%s wallet lock_all" % (ClientName))
        errorExit("Failed to lock all wallets")

    Print("Unlocking wallet \"%s\"." % (defproduceraWallet.name))
    if not walletMgr.unlockWallet(defproduceraWallet):
        cmdError("%s wallet unlock defproducera" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (defproduceraWallet.name))

    Print("Get account defproducera")
    account=node.getSysioAccount(defproduceraAccount.name, exitOnError=True)

    Print("Unlocking wallet \"%s\"." % (defproduceraWallet.name))
    if not walletMgr.unlockWallet(testWallet):
        cmdError("%s wallet unlock test" % (ClientName))
        errorExit("Failed to unlock wallet %s" % (testWallet.name))

    Print("Verify non-JSON call works")
    rawAccount = node.getSysioAccount(defproduceraAccount.name, exitOnError=True, returnType=ReturnType.raw)
    coreLiquidBalance = account['core_liquid_balance']
    match = re.search(r'\bliquid:\s*%s\s' % (coreLiquidBalance), rawAccount, re.MULTILINE | re.DOTALL)
    assert match is not None, "did not find the core liquid balance (\"liquid:\") of %d in \"%s\"" % (coreLiquidBalance, rawAccount)

    Print("Get head block num.")
    currentBlockNum=node.getHeadBlockNum()
    Print("CurrentBlockNum: %d" % (currentBlockNum))
    Print("Request blocks 1-%d" % (currentBlockNum))
    start=1
    for blockNum in range(start, currentBlockNum+1):
        block=node.getBlock(blockNum, silentErrors=False, exitOnError=True)

    Print("Request invalid block numbered %d. This will generate an expected error message." % (currentBlockNum+1000))
    currentBlockNum=node.getHeadBlockNum() # If the tests take too long, we could be far beyond currentBlockNum+1000 and that'll cause a block to be found.
    block=node.getBlock(currentBlockNum+1000, silentErrors=True)
    if block is not None:
        errorExit("ERROR: Received block where not expected")
    else:
        Print("Success: No such block found")

    if localTest:
        p = re.compile('Assert')
        assertionsFound=False
        with open(errFileName) as errFile:
            for line in errFile:
                if p.search(line):
                    assertionsFound=True

        if assertionsFound:
            # Too many assertion logs, hard to validate how many are genuine. Make this a warning
            #  for now, hopefully the logs will get cleaned up in future.
            Print(f"WARNING: Asserts in {errFileName}")

    Print("Validating accounts at end of test")
    accounts=[testeraAccount, currencyAccount, exchangeAccount]
    cluster.validateAccounts(accounts)

    # run a get_table_rows API call for a table which has an incorrect abi (missing type), to make sure that
    # the resulting exception in http-plugin is caught and doesn't cause nodeop to crash (leap issue #1372).
    contractDir="unittests/contracts/sysio.token"
    wasmFile="sysio.token.wasm"
    abiFile="sysio.token.bad.abi"
    Print("Publish contract")
    trans=node.publishContract(currencyAccount, contractDir, wasmFile, abiFile, waitForTransBlock=True)
    if trans is None:
        cmdError("%s set contract currency1111" % (ClientName))
        errorExit("Failed to publish contract.")

    contract="currency1111"
    table="accounts"
    row0=node.getTableRow(contract, currencyAccount.name, table, 0)

    # because we set a bad abi (missing type, see "sysio.token.bad.abi") on the contract, the
    # getTableRow() is expected to fail and return None
    try:
        assert(not row0)
    except (AssertionError, KeyError) as _:
        Print("ERROR: Failed get table row assertion. %s" % (row0))
        raise

    # However check that the node is still running and didn't crash when processing getTableRow on a contract
    # with a bad abi. If node does crash and we get an exception during "get info", it means that we did not
    # catch the exception that occured while calling `cb()` in `http_plugin/macros.hpp`.
    currentBlockNum=node.getHeadBlockNum()
    Print("CurrentBlockNum: %d" % (currentBlockNum))

    # Verify "set code" and "set abi" work
    Print("Verify set code and set abi work")
    setCodeAbiAccount = Account("setcodeabi")
    setCodeAbiAccount.ownerPublicKey = cluster.sysioAccount.ownerPublicKey
    setCodeAbiAccount.activePublicKey = cluster.sysioAccount.ownerPublicKey
    cluster.createAccountAndVerify(setCodeAbiAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=100000)
    wasmFile="unittests/test-contracts/payloadless/payloadless.wasm"
    abiFile="unittests/test-contracts/payloadless/payloadless.abi"
    assert(node.setCodeOrAbi(setCodeAbiAccount, "code", wasmFile))
    assert(node.setCodeOrAbi(setCodeAbiAccount, "abi", abiFile))

    Print("Verify root tracking works")
    wasmFile="unittests/test-contracts/settlewns/settlewns.wasm"
    abiFile="unittests/test-contracts/settlewns/settlewns.abi"

    testUtlAccount = Account("test.utl")
    testUtlAccount.ownerPublicKey = cluster.sysioAccount.ownerPublicKey
    testUtlAccount.activePublicKey = cluster.sysioAccount.ownerPublicKey
    cluster.createAccountAndVerify(testUtlAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=100000)
    assert(node.setCodeOrAbi(testUtlAccount, "code", wasmFile))
    assert(node.setCodeOrAbi(testUtlAccount, "abi", abiFile))

    funUtlAccount = Account("fun.utl")
    funUtlAccount.ownerPublicKey = cluster.sysioAccount.ownerPublicKey
    funUtlAccount.activePublicKey = cluster.sysioAccount.ownerPublicKey
    cluster.createAccountAndVerify(funUtlAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=100000)
    assert(node.setCodeOrAbi(funUtlAccount, "code", wasmFile))
    assert(node.setCodeOrAbi(funUtlAccount, "abi", abiFile))

    notUtlAccount = Account("notutl")
    notUtlAccount.ownerPublicKey = cluster.sysioAccount.ownerPublicKey
    notUtlAccount.activePublicKey = cluster.sysioAccount.ownerPublicKey
    cluster.createAccountAndVerify(notUtlAccount, cluster.sysioAccount, nodeOwner=cluster.carlAccount, buyRAM=100000)
    assert(node.setCodeOrAbi(notUtlAccount, "code", wasmFile))
    abiTrans=node.setCodeOrAbi(notUtlAccount, "abi", abiFile, returnTrans=True)
    node.waitForTransactionInBlock(abiTrans["transaction_id"])


    def sendAction(contract, action, data, opts, transArr, ids):
        trans=node.pushMessage(contract, action, data, opts)
        assert(trans[0])
        transArr.append(trans[1])
        Print(f"push {action} action to contract {contract}: {json.dumps(transArr[-1], indent=4)}")
        ids.append(transArr[-1]["transaction_id"])


    testTrans=[]
    testIds=[]

    funTrans=[]
    funIds=[]

    notTrans=[]
    notIds=[]

    sendAction(testUtlAccount.name, "batchw", "{\"batch\": 1, \"withdrawals\": [] }", "--permission test.utl@active", testTrans, testIds)
    sendAction(funUtlAccount.name, "batchw", "{\"batch\": 1, \"withdrawals\": [] }", "--permission fun.utl@active", funTrans, funIds)
    regTrans=node.regproducer(notUtlAccount, url="", location=0)
    sendAction(notUtlAccount.name, "batchw", "{\"batch\": 1, \"withdrawals\": [] }", "--permission notutl@active", notTrans, notIds)
    sendAction(testUtlAccount.name, "snoop", "{ }", "--permission test.utl@active", testTrans, testIds)
    sendAction(testUtlAccount.name, "cancelbatch", "{\"batch\": 1 }", "--permission test.utl@active", testTrans, testIds)
    sendAction(testUtlAccount.name, "selfwithd", "{\"user\": \"bigguy\", \"utxos\": [] }", "--permission test.utl@active", testTrans, testIds)
    unregTrans=node.unregprod(notUtlAccount, silentErrors=False)
    Print(f"push unregprod action to contract system: {json.dumps(unregTrans, indent=4)}")

    currentBlockNum=node.getHeadBlockNum()
    node.waitForBlock(currentBlockNum + 3)
    sendAction(funUtlAccount.name, "snoop", "{ }", "--permission fun.utl@active", funTrans, funIds)

    currentBlockNum=node.getHeadBlockNum()
    node.waitForBlock(currentBlockNum + 3)
    sendAction(funUtlAccount.name, "cancelbatch", "{\"batch\": 1 }", "--permission fun.utl@active", funTrans, funIds)
    sendAction(notUtlAccount.name, "snoop", "{ }", "--permission notutl@active", notTrans, notIds)

    class RootContract(Enum):
        ONE = 0
        TWO = 1
        THREE = 2
    class SRootCounter:
        def __init__(self):
            self.sRootHeaders={}
            self.contracts=[]
            for contract in RootContract:
               self.contracts.append([])
            self.maxCount=len(self.contracts)

        def add(self, blockNum, rootContract):
            notPresent=True if blockNum not in self.sRootHeaders else False
            if notPresent:
                self.sRootHeaders[blockNum] = 0
            contract=self.contracts[rootContract.value]
            # only need to increment if this contract wasn't already accounted for in this block
            if blockNum not in contract:
                self.sRootHeaders[blockNum]+=1
                contract.append(blockNum)

        def zero(self, blockNum):
            notPresent=True if blockNum not in self.sRootHeaders else False
            if notPresent:
                self.sRootHeaders[blockNum] = 0

        def validatedNoSRootBlock(self):
            if 0 not in self.sRootHeaders.values():
                # test transactions are not ensuring there is a block that is proving there is no state
                # root header generated when there should not be, so need to reconfigure test scenario
                Print(f"ERROR: Test setup failed to be setup to verify that a block has no state root header.")
                assert(False)

        def keys(self):
            return self.sRootHeaders.keys()
         
        def count(self, blockNum):
            return self.sRootHeaders[blockNum]

    sRootCounter=SRootCounter()

    blockNum=node.getBlockNumByTransId(testIds[0])
    sRootCounter.add(blockNum, RootContract.ONE)

    # if the 2nd action was in a different block, then each will have its own
    # state root header in both blocks
    blockNum=node.getBlockNumByTransId(testIds[1])
    sRootCounter.add(blockNum, RootContract.ONE)

    # if the 3rd action was in a different block, we need to make sure that
    # block doesn't have a state root header
    blockNum=node.getBlockNumByTransId(testIds[2])
    sRootCounter.zero(blockNum)

    # if the 4th action was in a different block, we need to make sure that
    # block doesn't have a state root header
    blockNum=node.getBlockNumByTransId(testIds[3])
    sRootCounter.zero(blockNum)

    blockNum=node.getBlockNumByTransId(funIds[0])
    sRootCounter.add(blockNum, RootContract.TWO)

    blockNum=node.getBlockNumByTransId(funIds[1])
    sRootCounter.add(blockNum, RootContract.TWO)

    blockNum=node.getBlockNumByTransId(funIds[2])
    sRootCounter.zero(blockNum)

    blockNum=node.getBlockNumByTransId(funIds[2])
    sRootCounter.zero(blockNum)

    blockNum=node.getBlockNumByTransId(regTrans["transaction_id"])
    sRootCounter.add(blockNum, RootContract.THREE)

    blockNum=node.getBlockNumByTransId(unregTrans["transaction_id"])
    sRootCounter.add(blockNum, RootContract.THREE)

    blockNum=node.getBlockNumByTransId(notIds[0])
    sRootCounter.zero(blockNum)

    blockNum=node.getBlockNumByTransId(notIds[1])
    sRootCounter.zero(blockNum)

    blocks={}

    for key in sRootCounter.keys():
        blocks[key] = node.getBlock(key, headerState=True, exitOnError=True)
        headerStr="header"
        Print(f"block header content: {json.dumps(blocks[key][headerStr], indent=4)}")

    def getSHeaders(block):
        headerStr="header"
        headerExtensionsStr="header_extensions"
        blockNum=block["block_num"]
        assert(headerStr in block)
        sHeaderCount=0
        if headerExtensionsStr not in block[headerStr].keys():
            raise Exception(f"No \"{headerExtensionsStr}\" found in block")

        headerExtensions = block[headerStr][headerExtensionsStr]
        sHeadersContent=[]
        if headerExtensions is None:
            return sHeadersContent

        for headerExtension in headerExtensions:
            assert(headerExtension is not None)
            assert(len(headerExtension) == 2)
            STATE_ROOT_HEADER_EXTENSION_ID=2 # the extension id that indicates this header extension is a state root
            ID_SLOT=0
            DATA_SLOT=1
            if headerExtension[ID_SLOT] != STATE_ROOT_HEADER_EXTENSION_ID:
                Print(f"Skipping headerExtension id: ${headerExtension[ID_SLOT]}")
                continue
            data=headerExtension[DATA_SLOT]
            assert(data is not None)
            sHeadersContent.append(data)  # later add parsing
        return sHeadersContent

    sHeaders={}    
    
    for block_num, block in blocks.items():
        sHeadersContent=getSHeaders(block)
        count=sRootCounter.count(block_num)
        if count == 0:
            assert(sHeadersContent is None or len(sHeadersContent) == 0)
            continue

        # should be at most maxCount state root headers per block, otherwise the test is not setup correctly
        assert(count <= sRootCounter.maxCount)
        assert(count == len(sHeadersContent))
        Print(f"NEED TO ADD PARSING OF sHeadersContent: {sHeadersContent}")

    sRootCounter.validatedNoSRootBlock()

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
