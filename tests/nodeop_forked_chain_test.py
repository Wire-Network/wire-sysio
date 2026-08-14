#!/usr/bin/env python3

from datetime import datetime
from datetime import timedelta
import time
import json
import signal
import os

from TestHarness import Cluster, Node, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys
from TestHarness.TestHelper import AppArgs

###############################################################
# nodeop_forked_chain_test
# 
# This test sets up 2 producing nodes and one "bridge" node using test_control_api_plugin.
#   One producing node has 11 of the elected producers and the other has 10 of the elected producers.
#   All the producers are named in alphabetical order, so that the 11 producers, in the one production node, are
#       scheduled first, followed by the 10 producers in the other producer node. Each producing node is only connected
#       to the other producing node via the "bridge" node.
#   The bridge node has the test_control_api_plugin, which exposes a restful interface that the test script uses to kill
#       the "bridge" node at a specific producer in the production cycle. This is used to fork the producer network
#       precisely when the 11 producer node has finished producing and the other producing node is about to produce.
#   The fork in the producer network results in one fork of the block chain that advances with 10 producers with a LIB
#      that has advanced, since all of the previous blocks were confirmed and the producer that was scheduled for that
#      slot produced it, and one with 11 producers with a LIB that has not advanced.  This situation is validated by
#      the test script.
#   After both chains are allowed to produce, the "bridge" node is turned back on.
#   Time is allowed to progress so that the "bridge" node can catchup and both producer nodes to come to consensus
#   The block log is then checked for both producer nodes to verify that the 10 producer fork is selected and that
#       both nodes are in agreement on the block log.
#   This test also runs a state_history_plugin (SHiP) on node 0 and uses ship_streamer to verify all blocks are received
#   across the fork.
#
###############################################################

Print=Utils.Print

# Block timestamps advance one production slot at a time, so the gap between two of them is a whole
# number of slots.  Only the slot interval is needed here; the chain's timestamp epoch is not, since
# the schedule grid is anchored to an observed block rather than to an absolute slot number.
slotTime=0.5

def slotsBetween(earlier, later):
    """Return the number of production slots from one parsed block timestamp to another."""
    return round((later - earlier).total_seconds() / slotTime)

def censusProductionCycle(getBlock, firstBlockNum, windowPhase, windowSize, cycleLength, scheduledProducers):
    """Attribute one full rotation of producers to the windows of a single schedule grid.

    Each producer owns exactly one contiguous window of windowSize slots, and a slot that goes
    unproduced leaves a hole inside its window: it never moves a window boundary and it never hands a
    slot to the next producer.  The cycle is therefore derived from the window each block falls in
    rather than from runs of consecutive blocks by the same producer.  Runs are only a proxy for the
    schedule, and a loaded test machine breaks the proxy: a node that stalls past its slot leaves a
    short run, or no run at all, with nothing wrong with the schedule.  Window boundaries are the
    property actually under test and they do not move when a block is late.

    getBlock(blockNum) supplies a block as (producer, slot), slots counted from an arbitrary origin.
    windowPhase says which slots begin a window, modulo windowSize.  The grid the chain is running on
    is not known ahead of time, so the caller tries each phase in turn: rather than exit on a
    violation this returns it in "error", leaving the caller to decide whether some other grid
    explains the same blocks.  "error" is None when this one explains all of them, and the rest of
    the result then describes the rotation:

      cycle          the producers in the order their windows come round
      counts         how many blocks each of them signed
      silentWindows  cycle positions whose producer signed nothing at all
      blocksExamined how far the walk got, which ranks the grids against each other when they all
                     fail and the most informative violation has to be picked out
    """
    windowOfSlot=lambda slot: (slot - windowPhase) // windowSize
    cycle=[]
    counts={}
    windowToProducer={}
    producerToWindow={}
    blocksExamined=0
    result={"cycle":cycle, "counts":counts, "silentWindows":[], "blocksExamined":0, "error":None}

    def fail(message):
        result["blocksExamined"]=blocksExamined
        result["error"]=message
        return result

    blockNum=firstBlockNum
    producer,slot=getBlock(blockNum)
    firstWindow=windowOfSlot(slot)
    endWindow=firstWindow + cycleLength   # exclusive, one full rotation
    lastWindow=firstWindow

    while True:
        window=windowOfSlot(slot)
        if window >= endWindow:
            # This look-ahead block is the first at or past the wrap, and it is checked rather than
            # discarded: a producer granted a thirteenth consecutive slot would put its own block
            # here, and only comparing against the producer that owns the window this cycle started
            # on catches that.
            if window == endWindow:
                wrapped=windowToProducer.get(firstWindow)
                if wrapped is not None and producer != wrapped:
                    return fail("Block %d opens the next cycle at position %d but was produced by %s, where the cycle "
                                "began with %s.  A cycle of %d producers must wrap onto the producer it started with." %
                                (blockNum, window-firstWindow, producer, wrapped, cycleLength))
            break

        if producer not in scheduledProducers:
            return fail("Producer %s, of block %d, was not one of the voted on producers" % (producer, blockNum))

        if window < lastWindow:
            return fail("Block %d, produced by %s, is at cycle position %d, behind position %d of the block before "
                        "it." % (blockNum, producer, window-firstWindow, lastWindow-firstWindow))

        owner=windowToProducer.get(window)
        if owner is None:
            if producer in producerToWindow:
                return fail("Producer %s owns cycle position %d, but also produced block %d at position %d.  "
                            "A producer is scheduled at most once per %d producer cycle." %
                            (producer, producerToWindow[producer]-firstWindow, blockNum, window-firstWindow,
                             cycleLength))
            windowToProducer[window]=producer
            producerToWindow[producer]=window
            counts[producer]=0
            cycle.append(producer)
        elif owner != producer:
            return fail("Block %d, %d slots into the walk, was produced by %s, but %s already produced at cycle "
                        "position %d.  A window belongs to exactly one producer." %
                        (blockNum, slot, producer, owner, window-firstWindow))

        counts[producer]+=1
        blocksExamined+=1
        lastWindow=window

        precedingProducer=producer
        precedingSlot=slot
        blockNum+=1
        producer,slot=getBlock(blockNum)
        if slot-precedingSlot==1 and producer!=precedingProducer and (slot - windowPhase) % windowSize != 0:
            # A handover into the very next slot can only happen on a window boundary, so this block
            # has to sit at offset 0 of its window.  Landing anywhere else means this handover and
            # this grid disagree about where the boundaries are, which is a producer having been
            # given a run of the wrong length.
            return fail("Producer handover from %s to %s at block %d landed %d slots into a %d slot window instead of "
                        "at its start.  Producer runs are not aligned to this window grid." %
                        (precedingProducer, producer, blockNum, (slot - windowPhase) % windowSize, windowSize))

    result["silentWindows"]=[position for position in range(cycleLength)
                             if firstWindow+position not in windowToProducer]
    result["blocksExamined"]=blocksExamined
    return result

def analyzeBPs(bps0, bps1, expectDivergence):
    start=0
    index=None
    length=len(bps0)
    firstDivergence=None
    errorInDivergence=False
    analysysPass=0
    bpsStr=None
    bpsStr0=None
    bpsStr1=None
    while start < length:
        analysysPass+=1
        bpsStr=None
        for i in range(start,length):
            bp0=bps0[i]
            bp1=bps1[i]
            if bpsStr is None:
                bpsStr=""
            else:
                bpsStr+=", "
            blockNum0=bp0["blockNum"]
            prod0=bp0["prod"]
            blockNum1=bp1["blockNum"]
            prod1=bp1["prod"]
            numDiff=True if blockNum0!=blockNum1 else False
            prodDiff=True if prod0!=prod1 else False
            if numDiff or prodDiff:
                index=i
                if firstDivergence is None:
                    firstDivergence=min(blockNum0, blockNum1)
                if not expectDivergence:
                    errorInDivergence=True
                break
            bpsStr+=str(blockNum0)+"->"+prod0

        if index is None:
            if expectDivergence:
                errorInDivergence=True
                break
            return None

        bpsStr0=None
        bpsStr2=None
        start=length
        for i in range(index,length):
            if bpsStr0 is None:
                bpsStr0=""
                bpsStr1=""
            else:
                bpsStr0+=", "
                bpsStr1+=", "
            bp0=bps0[i]
            bp1=bps1[i]
            blockNum0=bp0["blockNum"]
            prod0=bp0["prod"]
            blockNum1=bp1["blockNum"]
            prod1=bp1["prod"]
            numDiff="*" if blockNum0!=blockNum1 else ""
            prodDiff="*" if prod0!=prod1 else ""
            if not numDiff and not prodDiff:
                start=i
                index=None
                if expectDivergence:
                    errorInDivergence=True
                break
            bpsStr0+=str(blockNum0)+numDiff+"->"+prod0+prodDiff
            bpsStr1+=str(blockNum1)+numDiff+"->"+prod1+prodDiff
        if errorInDivergence:
            break

    if errorInDivergence:
        msg="Failed analyzing block producers - "
        if expectDivergence:
            msg+="nodes do not indicate different block producers for the same blocks, but they are expected to diverge at some point."
        else:
            msg+="did not expect nodes to indicate different block producers for the same blocks."
        msg+="\n  Matching Blocks= %s \n  Diverging branch node0= %s \n  Diverging branch node1= %s" % (bpsStr,bpsStr0,bpsStr1)
        Utils.errorExit(msg)

    return firstDivergence

def getMinHeadAndLib(prodNodes):
    info0=prodNodes[0].getInfo(exitOnError=True)
    info1=prodNodes[1].getInfo(exitOnError=True)
    headBlockNum=min(int(info0["head_block_num"]),int(info1["head_block_num"]))
    libNum=min(int(info0["last_irreversible_block_num"]), int(info1["last_irreversible_block_num"]))
    return (headBlockNum, libNum)


appArgs = AppArgs()
extraArgs = appArgs.add(flag="--num-ship-clients", type=int, help="How many ship_streamers should be started", default=2)
args = TestHelper.parse_args({"--prod-count","--activate-if","--dump-error-details","--keep-logs","-v","--leave-running",
                              "--wallet-port","--unshared"}, applicationSpecificArgs=appArgs)
Utils.Debug=args.v
totalProducerNodes=2
totalNonProducerNodes=1
totalNodes=totalProducerNodes+totalNonProducerNodes
maxActiveProducers=21
totalProducers=maxActiveProducers
activateIF=args.activate_if
dumpErrorDetails=args.dump_error_details
prodCount=args.prod_count
walletPort=args.wallet_port
num_clients=args.num_ship_clients
cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)

walletMgr=WalletMgr(True, port=walletPort)
testSuccessful=False

WalletdName=Utils.SysWalletName
ClientName="clio"

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)
    Print("Stand up cluster")
    specificExtraNodeopArgs={}
    shipNodeNum = 0
    specificExtraNodeopArgs[shipNodeNum]=(
        "--plugin sysio::state_history_plugin "
        f"--state-history-endpoint 127.0.0.1:{Utils.getPort(Utils.PortStateHistory)}"
    )

    # producer nodes will be mapped to 0 through totalProducerNodes-1, so the number totalProducerNodes will be the non-producing node
    specificExtraNodeopArgs[totalProducerNodes]="--plugin sysio::test_control_api_plugin"
    # test expects split network to advance with single producer
    extraNodeopArgs=" --production-pause-vote-timeout-ms 0 "

    # ***   setup topogrophy   ***

    # bridge_consecutive_shape.json shape connects defproducera through defproducerk (consecutive in node0) to each other
    # and defproducerl through defproduceru (consecutive in node01)
    # and the only connection between those 2 groups is through the bridge node

    if cluster.launch(prodCount=prodCount, topo="./tests/bridge_consecutive_shape.json", pnodes=totalProducerNodes,
                      totalNodes=totalNodes, totalProducers=totalProducers, activateIF=activateIF, biosFinalizer=False,
                      specificExtraNodeopArgs=specificExtraNodeopArgs, extraNodeopArgs=extraNodeopArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up sys cluster.")
    Print("Validating system accounts after bootstrap")
    cluster.validateAccounts(None)


    # ***   create accounts to vote in desired producers   ***

    accounts=createAccountKeys(5)
    if accounts is None:
        Utils.errorExit("FAILURE - create keys")
    accounts[0].name="tester111111"
    accounts[1].name="tester222222"
    accounts[2].name="tester333333"
    accounts[3].name="tester444444"
    accounts[4].name="tester555555"

    testWalletName="test"

    Print("Creating wallet \"%s\"." % (testWalletName))
    testWallet=walletMgr.create(testWalletName, [cluster.sysioAccount,accounts[0],accounts[1],accounts[2],accounts[3],accounts[4]])

    for _, account in cluster.defProducerAccounts.items():
        walletMgr.importKey(account, testWallet, ignoreDupKeyWarning=True)

    Print("Wallet \"%s\" password=%s." % (testWalletName, testWallet.password.encode("utf-8")))


    # ***   identify each node (producers and non-producing node)   ***

    nonProdNode=None
    prodNodes=[]
    producers=[]
    for i in range(0, totalNodes):
        node=cluster.getNode(i)
        node.producers=Cluster.parseProducers(i)
        numProducers=len(node.producers)
        Print("node has producers=%s" % (node.producers))
        if numProducers==0:
            if nonProdNode is None:
                nonProdNode=node
                nonProdNode.nodeNum=i
            else:
                Utils.errorExit("More than one non-producing nodes")
        else:
            for prod in node.producers:
                trans=node.regproducer(cluster.defProducerAccounts[prod], "http::/mysite.com", 0, waitForTransBlock=False, exitOnError=True)

            prodNodes.append(node)
            producers.extend(node.producers)


    node=nonProdNode
    # create accounts via sysio as otherwise a bid is needed
    for account in accounts:
        Print("Create new account %s via %s" % (account.name, cluster.sysioAccount.name))
        trans=node.createInitializeAccount(account, cluster.sysioAccount, stakedDeposit=0, waitForTransBlock=True, stakeNet=1000, stakeCPU=1000, buyRAM=1000, exitOnError=True)
        transferAmount="100000000.0000 {0}".format(CORE_SYMBOL)
        Print("Transfer funds %s from account %s to %s" % (transferAmount, cluster.sysioAccount.name, account.name))
        node.transferFunds(cluster.sysioAccount, account, transferAmount, "test transfer", waitForTransBlock=True)

    #verify nodes are in sync and advancing
    cluster.waitOnClusterSync(blockAdvancing=5)

    # ***   Identify a block where production is stable   ***

    #verify nodes are in sync and advancing
    cluster.waitOnClusterSync(blockAdvancing=5)
    blockNum=node.getNextCleanProductionCycle(trans)
    blockProducer=node.getBlockProducerByNum(blockNum)
    Print("Validating blockNum=%s, producer=%s" % (blockNum, blockProducer))
    cluster.biosNode.kill(signal.SIGTERM)

    # Every slot that goes unproduced pushes the next block a further slotTime out in wall clock, and
    # the walk below is deliberately tolerant of missed slots, so the wait for each block has to
    # outlast a stall rather than abort on it.  The original three second leeway is shorter than
    # stalls that have actually been seen on loaded CI machines, so when caught up to the head this
    # gave the walk four seconds and it aborted before the gap it is now meant to record.  The leeway
    # is what bounds how long the chain may go dark before the test declares it dead.
    stalledChainLeewaySeconds=60

    class HeadWaiter:
        def __init__(self, node, leewaySeconds):
            self.node=node
            self.leewaySeconds=leewaySeconds
            self.cachedHeadBlockNum=node.getBlockNum()

        def waitIfNeeded(self, blockNum):
            delta=self.cachedHeadBlockNum-blockNum
            if delta >= 0:
                return
            previousHeadBlockNum=self.cachedHeadBlockNum
            delta=-1*delta
            timeout=(delta+1)/2 + self.leewaySeconds # round up to nearest second, plus the stall leeway
            self.node.waitForBlock(blockNum, timeout=timeout)
            self.cachedHeadBlockNum=node.getBlockNum()
            if blockNum > self.cachedHeadBlockNum:
                Utils.errorExit("Failed to advance from block number %d to %d in %d seconds.  Only got to block number %d" % (previousHeadBlockNum, blockNum, timeout, self.cachedHeadBlockNum))

        def getBlock(self, blockNum):
            self.waitIfNeeded(blockNum)
            return self.node.getBlock(blockNum)

    waiter=HeadWaiter(node, stalledChainLeewaySeconds)
    inRowCountPerProducer=12

    class BlockReader:
        """Reads the walked blocks as (producer, slot), fetching each of them exactly once.

        The census is replayed against one candidate window grid after another, so the same blocks
        are read several times over and holding them is what keeps that from costing a refetch.  It
        is also where the missed slots are recorded, since a walk that gets repeated is no place to
        count anything.  Slots are counted from the block the walk begins on: the grid is anchored to
        an observed block rather than to an absolute slot number, so the chain's timestamp epoch
        never enters into it.
        """
        def __init__(self, waiter, firstBlockNum):
            self.waiter=waiter
            self.nextBlockNum=firstBlockNum
            self.blocks={}
            self.missedSlotAfter=[]
            self.originTimestamp=None
            self.lastTimestamp=None

        def get(self, blockNum):
            entry=self.blocks.get(blockNum)
            if entry is not None:
                return entry
            if blockNum != self.nextBlockNum:
                Utils.errorExit("Block %d was read before block %d, but the walk only ever moves forward." %
                                (blockNum, self.nextBlockNum))
            block=self.waiter.getBlock(blockNum)
            producer=Node.getBlockAttribute(block, "producer", blockNum)
            timestampStr=Node.getBlockAttribute(block, "timestamp", blockNum)
            timestamp=datetime.strptime(timestampStr, Utils.TimeFmt)
            if self.originTimestamp is None:
                self.originTimestamp=timestamp
            else:
                slotsDiff=slotsBetween(self.lastTimestamp, timestamp)
                if slotsDiff != 1:
                    slotTimeDelta=timedelta(seconds=slotTime)
                    missed=(self.lastTimestamp + slotTimeDelta).strftime(Utils.TimeFmt)
                    if slotsDiff > 2:
                        missed+= " thru " + (timestamp - slotTimeDelta).strftime(Utils.TimeFmt)
                    self.missedSlotAfter.append("%d (%s)" % (blockNum-1, missed))
            self.lastTimestamp=timestamp
            entry=(producer, slotsBetween(self.originTimestamp, timestamp))
            self.blocks[blockNum]=entry
            self.nextBlockNum=blockNum+1
            return entry

    reader=BlockReader(waiter, blockNum)

    shipNode = cluster.getNode(0)
    block_range = 1000
    start_block_num = blockNum
    end_block_num = start_block_num + block_range

    shipClient = "tests/ship_streamer"
    cmd = (
        f"{shipClient} --socket-address 127.0.0.1:{Utils.getPort(Utils.PortStateHistory)} "
        f"--start-block-num {start_block_num} --end-block-num {end_block_num} "
        "--fetch-block --fetch-traces --fetch-deltas"
    )
    if Utils.Debug: Utils.Print(f"cmd: {cmd}")
    clients = []
    files = []
    shipTempDir = os.path.join(Utils.DataDir, "ship")
    os.makedirs(shipTempDir, exist_ok = True)
    shipClientFilePrefix = os.path.join(shipTempDir, "client")

    starts = []
    for i in range(0, num_clients):
        start = time.perf_counter()
        outFile = open(f"{shipClientFilePrefix}{i}.out", "w")
        errFile = open(f"{shipClientFilePrefix}{i}.err", "w")
        Print(f"Start client {i}")
        popen=Utils.delayedCheckOutput(cmd, stdout=outFile, stderr=errFile)
        starts.append(time.perf_counter())
        clients.append((popen, cmd))
        files.append((outFile, errFile))
        Utils.Print(f"Client {i} started, Ship node head is: {shipNode.getBlockNum()}")

    # ***   Identify what the production cycle is   ***

    # Which slots begin a producer's window is not known ahead of time, so the walk is run against
    # each of the inRowCountPerProducer grids in turn and the first that explains every block is the
    # one the chain is running on.  Deriving a single grid up front instead, from how the earliest
    # runs of consecutive blocks sit, does not work: it asks the same question the walk itself asks,
    # only of fewer blocks, so the answer can be ambiguous and committing to one of the survivors
    # rejects chains the surviving alternative accepts.  It also has nowhere to stop, since a
    # producer that never yields leaves the run count standing still while the chain advances happily
    # underneath it.
    #
    # Grids the chain is not running on are rejected almost immediately: a producer's run straddles
    # their boundaries and it ends up owning two windows, which the walk refuses.
    census=None
    failedCensuses=[]
    for windowPhase in range(inRowCountPerProducer):
        attempt=censusProductionCycle(reader.get, blockNum, windowPhase, inRowCountPerProducer,
                                      maxActiveProducers, producers)
        if attempt["error"] is None:
            census=attempt
            break
        failedCensuses.append(attempt)

    if census is None:
        # Every grid failed, so the schedule really is not running in windows of this size.  The one
        # that got furthest is the chain's own grid, and so the one whose complaint describes what is
        # actually wrong.
        furthest=max(failedCensuses, key=lambda attempt: attempt["blocksExamined"])
        Utils.errorExit("No grid of %d slot windows accounts for the blocks produced over a full cycle, so the "
                        "producer schedule is not running in windows of %d.  The grid that accounted for the most "
                        "blocks, %d of them, reports: %s" %
                        (inRowCountPerProducer, inRowCountPerProducer, furthest["blocksExamined"], furthest["error"]))

    # Unproduced slots are a liveness property of the machine the test is running on, not of the
    # schedule, and the schedule was verified window by window above.  Report them, do not fail on
    # them.
    if len(reader.missedSlotAfter) > 0:
        Print("WARNING: slots went unproduced after the following blocks: %s" % (", ".join(reader.missedSlotAfter)))

    if len(census["silentWindows"]) > 0:
        Print("WARNING: %d of the %d producers in the cycle produced no block in their window, at cycle positions: %s" %
              (len(census["silentWindows"]), maxActiveProducers,
               ", ".join(str(position) for position in census["silentWindows"])))

    productionCycle=census["cycle"]
    Print("ProductionCycle ->> {\n%s\n}" %
          (", ".join(blockProducer+":"+str(census["counts"][blockProducer]) for blockProducer in productionCycle)))

    #retrieve the info for all the nodes to report the status for each
    for node in cluster.getNodes():
        node.getInfo()
    cluster.reportStatus()


    # ***   Killing the "bridge" node   ***

    Print("Sending command to kill \"bridge\" node to separate the 2 producer groups.")
    # block number to start expecting node killed after
    preKillBlockNum=nonProdNode.getBlockNum()
    preKillBlockProducer=nonProdNode.getBlockProducerByNum(preKillBlockNum)
    if preKillBlockProducer == "defproducerj" or preKillBlockProducer == "defproducerk":
        # wait for defproduceri so there is plenty of time to send kill before defproducerk
        nonProdNode.waitForProducer("defproduceri")
        preKillBlockNum=nonProdNode.getBlockNum()
        preKillBlockProducer=nonProdNode.getBlockProducerByNum(preKillBlockNum)
    Print("preKillBlockProducer = {}".format(preKillBlockProducer))
    # kill at last block before defproducerl, since the block it is killed on will get propagated
    killAtProducer="defproducerk"
    nonProdNode.killNodeOnProducer(producer=killAtProducer, whereInSequence=(inRowCountPerProducer-1))


    # ***   Identify a highest block number to check while we are trying to identify where the divergence will occur   ***

    # will search full cycle after the current block, since we don't know how many blocks were produced since retrieving
    # block number and issuing kill command
    postKillBlockNum=prodNodes[1].getBlockNum()
    blockProducers0=[]
    blockProducers1=[]
    libs0=[]
    libs1=[]
    lastBlockNum=max([preKillBlockNum,postKillBlockNum])+2*maxActiveProducers*inRowCountPerProducer
    actualLastBlockNum=None
    prodChanged=False
    nextProdChange=False
    #identify the earliest LIB to start identify the earliest block to check if divergent branches eventually reach concensus
    (headBlockNum, libNumAroundDivergence)=getMinHeadAndLib(prodNodes)
    Print("Tracking block producers from %d till divergence or %d. Head block is %d and lowest LIB is %d" % (preKillBlockNum, lastBlockNum, headBlockNum, libNumAroundDivergence))
    transitionCount=0
    missedTransitionBlock=None
    for blockNum in range(preKillBlockNum,lastBlockNum + 1):
        #avoiding getting LIB until my current block passes the head from the last time I checked
        if blockNum>headBlockNum:
            (headBlockNum, libNumAroundDivergence)=getMinHeadAndLib(prodNodes)

        # track the block number and producer from each producing node
        # we use timeout 70 here because of case when chain break, call to getBlockProducerByNum
        # and call of producer_plugin::schedule_delayed_production_loop happens nearly immediately
        # for 10 producers wait cycle is 10 * (12*0.5) = 60 seconds.
        # for 11 producers wait cycle is 11 * (12*0.5) = 66 seconds.
        blockProducer0=prodNodes[0].getBlockProducerByNum(blockNum, timeout=70)
        blockProducer1=prodNodes[1].getBlockProducerByNum(blockNum, timeout=70)
        Print("blockNum = {} blockProducer0 = {} blockProducer1 = {}".format(blockNum, blockProducer0, blockProducer1))
        blockProducers0.append({"blockNum":blockNum, "prod":blockProducer0})
        blockProducers1.append({"blockNum":blockNum, "prod":blockProducer1})

        #in the case that the preKillBlockNum was also produced by killAtProducer, ensure that we have
        #at least one producer transition before checking for killAtProducer
        if not prodChanged:
            if preKillBlockProducer!=blockProducer0:
                prodChanged=True
                Print("prodChanged = True")

        #since it is killing for the last block of killAtProducer, we look for the next producer change
        if not nextProdChange and prodChanged and blockProducer1==killAtProducer:
            nextProdChange=True
            Print("nextProdChange = True")
        elif nextProdChange and blockProducer1!=killAtProducer:
            Print("nextProdChange = False")
            if blockProducer0!=blockProducer1:
                Print("Divergence identified at block %s, node_00 producer: %s, node_01 producer: %s" % (blockNum, blockProducer0, blockProducer1))
                actualLastBlockNum=blockNum
                break
            else:
                missedTransitionBlock=blockNum
                transitionCount+=1
                Print("missedTransitionBlock = {} transitionCount = {}".format(missedTransitionBlock, transitionCount))
                # allow this to transition twice, in case the script was identifying an earlier transition than the bridge node received the kill command
                if transitionCount>1:
                    Print("At block %d and have passed producer: %s %d times and we have not diverged, stopping looking and letting errors report" % (blockNum, killAtProducer, transitionCount))
                    actualLastBlockNum=blockNum
                    break

        #if we diverge before identifying the actualLastBlockNum, then there is an ERROR
        if blockProducer0!=blockProducer1:
            extra="" if transitionCount==0 else " Diverged after expected killAtProducer transition at block %d." % (missedTransitionBlock)
            Utils.errorExit("Groups reported different block producers for block number %d.%s %s != %s." % (blockNum,extra,blockProducer0,blockProducer1))

    #verify that the non producing node is not alive (and populate the producer nodes with current getInfo data to report if
    #an error occurs)
    if nonProdNode.verifyAlive():
        Utils.errorExit("Expected the non-producing node to have shutdown.")

    Print("Analyzing the producers leading up to the block after killing the non-producing node, expecting divergence at %d" % (blockNum))

    firstDivergence=analyzeBPs(blockProducers0, blockProducers1, expectDivergence=True)
    # Nodes should not have diverged till the last block
    if firstDivergence!=blockNum:
        Utils.errorExit("Expected to diverge at %s, but diverged at %s." % (firstDivergence, blockNum))
    blockProducers0=[]
    blockProducers1=[]

    for prodNode in prodNodes:
        info=prodNode.getInfo()
        Print("node info: %s" % (info))

    killBlockNum=blockNum
    lastBlockNum=killBlockNum+(maxActiveProducers - 1)*inRowCountPerProducer+1  # allow 1st testnet group to produce just 1 more block than the 2nd

    Print("Tracking the blocks from the divergence till there are 10*12 blocks on one chain and 10*12+1 on the other, from block %d to %d" % (killBlockNum, lastBlockNum))

    for blockNum in range(killBlockNum,lastBlockNum):
        blockProducer0=prodNodes[0].getBlockProducerByNum(blockNum, timeout=70)
        blockProducer1=prodNodes[1].getBlockProducerByNum(blockNum, timeout=70)
        blockProducers0.append({"blockNum":blockNum, "prod":blockProducer0})
        blockProducers1.append({"blockNum":blockNum, "prod":blockProducer1})


    Print("Analyzing the producers from the divergence to the lastBlockNum and verify they stay diverged, expecting divergence at block %d" % (killBlockNum))

    firstDivergence=analyzeBPs(blockProducers0, blockProducers1, expectDivergence=True)
    if firstDivergence!=killBlockNum:
        Utils.errorExit("Expected to diverge at %s, but diverged at %s." % (firstDivergence, killBlockNum))
    blockProducers0=[]
    blockProducers1=[]

    for prodNode in prodNodes:
        info=prodNode.getInfo()
        Print("node info: %s" % (info))

    Print("Relaunching the non-producing bridge node to connect the producing nodes again")

    if not nonProdNode.relaunch():
        Utils.errorExit("Failure - (non-production) node %d should have restarted" % (nonProdNode.nodeNum))


    Print("Waiting to allow forks to resolve")

    for prodNode in prodNodes:
        info=prodNode.getInfo()
        Print("node info: %s" % (info))

    #ensure that the nodes have enough time to get in concensus, so wait for 3 producers to produce their complete round
    time.sleep(inRowCountPerProducer * 3 / 2)
    remainingChecks=60
    match=False
    checkHead=False
    checkMatchBlock=killBlockNum
    forkResolved=False
    while remainingChecks>0:
        if checkMatchBlock == killBlockNum and checkHead:
            checkMatchBlock = prodNodes[0].getBlockNum()
        blockProducer0=prodNodes[0].getBlockProducerByNum(checkMatchBlock)
        blockProducer1=prodNodes[1].getBlockProducerByNum(checkMatchBlock)
        match=blockProducer0==blockProducer1
        if match:
            if checkHead:
                forkResolved=True
                break
            else:
                checkHead=True
                continue
        Print("Fork has not resolved yet, wait a little more. Block %s has producer %s for node_00 and %s for node_01.  Original divergence was at block %s. Wait time remaining: %d" % (checkMatchBlock, blockProducer0, blockProducer1, killBlockNum, remainingChecks))
        time.sleep(1)
        remainingChecks-=1
    
    assert forkResolved, "fork was not resolved in a reasonable time. node_00 lib {} head {} node_01 lib {} head {}".format(
                                                                                  prodNodes[0].getIrreversibleBlockNum(), 
                                                                                          prodNodes[0].getHeadBlockNum(), 
                                                                                                          prodNodes[1].getIrreversibleBlockNum(), 
                                                                                                                 prodNodes[1].getHeadBlockNum()) 

    for prodNode in prodNodes:
        info=prodNode.getInfo()
        Print("node info: %s" % (info))

    # ensure all blocks from the lib before divergence till the current head are now in consensus
    endBlockNum=max(prodNodes[0].getBlockNum(), prodNodes[1].getBlockNum())

    Print("Identifying the producers from the saved LIB to the current highest head, from block %d to %d" % (libNumAroundDivergence, endBlockNum))

    for blockNum in range(libNumAroundDivergence,endBlockNum):
        blockProducer0=prodNodes[0].getBlockProducerByNum(blockNum)
        blockProducer1=prodNodes[1].getBlockProducerByNum(blockNum)
        blockProducers0.append({"blockNum":blockNum, "prod":blockProducer0})
        blockProducers1.append({"blockNum":blockNum, "prod":blockProducer1})


    Print("Analyzing the producers from the saved LIB to the current highest head and verify they match now")

    analyzeBPs(blockProducers0, blockProducers1, expectDivergence=False)

    resolvedKillBlockProducer=None
    for prod in blockProducers0:
        if prod["blockNum"]==killBlockNum:
            resolvedKillBlockProducer = prod["prod"]
    if resolvedKillBlockProducer is None:
        Utils.errorExit("Did not find find block %s (the original divergent block) in blockProducers0, test setup is wrong.  blockProducers0: %s" % (killBlockNum, ", ".join(blockProducers0)))
    Print("Fork resolved and determined producer %s for block %s" % (resolvedKillBlockProducer, killBlockNum))

    Print(f"Stopping all {num_clients} clients")
    for index, (popen, _), (out, err), start in zip(range(len(clients)), clients, files, starts):
        popen.wait()
        Print(f"Stopped client {index}.  Ran for {time.perf_counter() - start:.3f} seconds.")
        out.close()
        err.close()
        outFile = open(f"{shipClientFilePrefix}{index}.out", "r")
        data = json.load(outFile)
        block_num = start_block_num
        for i in data:
            # fork can cause block numbers to be repeated
            this_block_num = i['get_blocks_result_v1']['this_block']['block_num']
            if this_block_num < block_num:
                block_num = this_block_num
            assert block_num == this_block_num, f"{block_num} != {this_block_num}"
            assert isinstance(i['get_blocks_result_v1']['block'], str) # verify block in result
            block_num += 1
        assert block_num-1 == end_block_num, f"{block_num-1} != {end_block_num}"

    blockProducers0=[]
    blockProducers1=[]

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

# Too much output for ci/cd
#     if not testSuccessful:
#         Print(Utils.FileDivider)
#         Print("Compare Blocklog")
#         cluster.compareBlockLogs()
#         Print(Utils.FileDivider)
#         Print("Print Blocklog")
#         cluster.printBlockLog()
#         Print(Utils.FileDivider)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
