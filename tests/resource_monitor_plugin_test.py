#!/usr/bin/env python3

import random
import subprocess
import signal
import os
import shutil
import re
from datetime import datetime
from datetime import timedelta

from TestHarness import Cluster, TestHelper, Utils

Print=Utils.Print
errorExit=Utils.errorExit

stagingDir="rsmStaging"
dataDir=stagingDir+"/data"
configDir=stagingDir+"/etc"
traceDir=dataDir+"/traceDir"

loggingFile=configDir+"/logging.json"
stderrFile=dataDir + "/stderr.txt"

testNum=0
max_start_time_secs=10 # time nodeop takes to start

# We need debug level to get more information about nodeop process
logging="""{
  "includes": [],
  "appenders": [{
      "name": "stderr",
      "type": "console",
      "args": {
        "stream": "std_error",
        "level_colors": [{
            "level": "debug",
            "color": "green"
          },{
            "level": "warn",
            "color": "brown"
          },{
            "level": "error",
            "color": "red"
          }
        ]
      },
      "enabled": true
    }
  ],
  "loggers": [{
      "name": "default",
      "level": "debug",
      "enabled": true,
      "additivity": false,
      "appenders": [
        "stderr"
      ]
    }
  ]
}"""

def cleanDirectories():
    os.path.exists(stagingDir) and shutil.rmtree(stagingDir)

def prepareDirectories():
    # Prepare own directories so we don't depend on others to make sure
    # tests are repeatable
    cleanDirectories()
    os.makedirs(stagingDir)
    os.makedirs(dataDir)
    os.makedirs(configDir)

    with open(loggingFile, "w") as textFile:
        print(logging,file=textFile)

def runNodeop(extraNodeopArgs, myTimeout):
    """Startup nodeop, wait for timeout (before forced shutdown) and collect output."""
    if debug: Print("Launching nodeop process.")
    cmd="programs/nodeop/nodeop --config-dir rsmStaging/etc -e -p sysio --plugin sysio::chain_api_plugin --data-dir " + dataDir + " "

    cmd=cmd + extraNodeopArgs
    if debug: Print("cmd: %s" % (cmd))
    with open(stderrFile, 'w') as serr:
        proc=subprocess.Popen(cmd.split(), stdout=subprocess.PIPE, stderr=serr)

        try:
            proc.communicate(timeout=myTimeout)
        except (subprocess.TimeoutExpired) as _:
            if debug: Print("Timed out\n")
            proc.send_signal(signal.SIGKILL)

def isMsgInStderrFile(msg):
    msgFound=False
    with open(stderrFile) as errFile:
        for line in errFile:
            if msg in line:
                msgFound=True
                break
    return msgFound

def testCommon(title, extraNodeopArgs, expectedMsgs):
    global testNum
    testNum+=1
    Print("Test %d: %s" % (testNum, title))

    prepareDirectories()

    timeout=max_start_time_secs  # Leave sufficient time such nodeop can start up fully in any platforms
    runNodeop(extraNodeopArgs, timeout)

    for msg in expectedMsgs:
        if not isMsgInStderrFile(msg):
            errorExit ("Log should have contained \"%s\"" % (expectedMsgs))

def extractTimestamp(msg):
    matches = re.compile("\s+([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}.[0-9]{3})\s").search(msg)
    return datetime.strptime(matches.group(0).strip(), '%Y-%m-%dT%H:%M:%S.%f')

intervalTolerance = 0.15 # 15%

def isMsgIntervalValid(msg, expectedInterval):
    pre = ""
    hasMsg = validInterval = False
    with open(stderrFile) as errFile:
        for line in errFile:
            if msg in line:
                if debug: Print(line)
                hasMsg = True
                if pre:
                    curInterval = extractTimestamp(line) - extractTimestamp(pre)
                    if debug: Print(curInterval)
                    if abs((curInterval - timedelta(seconds=expectedInterval)).total_seconds()) <= expectedInterval * intervalTolerance:
                        validInterval = True
                    else:
                        validInterval = False
                        break
                pre = line
    return hasMsg, validInterval

fillerFile = dataDir + '/filler.tmp'

def fillFS(dir, threshold):
    total, used, available = shutil.disk_usage(dir)
    warningAvailable = total * (100 - threshold) // 100
    if available > warningAvailable:
        filesize = (available - warningAvailable) * 1.1 // (1024 * 1024) # add 0.1 redundancy to ensure warning be triggered
        os.system('dd if=/dev/zero of=' + fillerFile + ' count=' + str(filesize) + ' bs=1M')

testIntervalMaxTimeout = 300 # Assume nodeop at most runs 300 sec for this test

def testInterval(title, extraNodeopArgs, interval, expectedMsgs, warningThreshold):
    global testNum
    testNum += 1
    Print("Test %d: %s" % (testNum, title))

    prepareDirectories()
    fillFS(dataDir, warningThreshold)

    timeout = max_start_time_secs + interval * 2 # Leave sufficient time so nodeop can start up fully in any platforms, and at least two warnings can be output
    if timeout > testIntervalMaxTimeout: 
        errorExit ("Max timeout for testInterval is %d sec" % (testIntervalMaxTimeout))
    runNodeop(extraNodeopArgs, timeout)

    for msg in expectedMsgs:
        hasMsg, validInterval = isMsgIntervalValid(msg, interval)
        if not hasMsg:
            errorExit ("Log should have contained \"%s\"" % (msg))
        if not validInterval:
            errorExit ("Log containing \"%s\" should be output every %d seconds" % (msg, interval))

def testAll():
    testCommon("Resmon enabled: all arguments", "--plugin  sysio::resource_monitor_plugin --resource-monitor-space-threshold=85 --resource-monitor-interval-seconds=5 --resource-monitor-not-shutdown-on-threshold-exceeded", ["threshold set to 85", "interval set to 5", "Shutdown flag when threshold exceeded set to false"])

    # default arguments and default directories to be monitored
    testCommon("Resmon not enabled: no arguments", "", ["interval set to 2", "threshold set to 90", "Shutdown flag when threshold exceeded set to true", "snapshots's file system to be monitored", "blocks's file system to be monitored", "state's file system to be monitored"])
    
    # default arguments with registered directories
    testCommon("Resmon not enabled: Producer, Chain, State History and Trace Api", "--plugin sysio::state_history_plugin --state-history-dir=/tmp/state-history --disable-replay-opts --plugin sysio::trace_api_plugin --trace-dir=/tmp/trace --trace-no-abis", ["interval set to 2", "threshold set to 90", "Shutdown flag when threshold exceeded set to true", "snapshots's file system to be monitored", "blocks's file system to be monitored", "state's file system to be monitored", "state-history's file system to be monitored", "trace's file system to be monitored"])

    testCommon("Resmon enabled: Producer, Chain, State History and Trace Api", "--plugin  sysio::resource_monitor_plugin --plugin sysio::state_history_plugin --state-history-dir=/tmp/state-history --disable-replay-opts --plugin sysio::trace_api_plugin --trace-dir=/tmp/trace --trace-no-abis --resource-monitor-space-threshold=80 --resource-monitor-interval-seconds=3", ["snapshots's file system to be monitored", "blocks's file system to be monitored", "state's file system to be monitored", "state-history's file system to be monitored", "trace's file system to be monitored", "threshold set to 80", "interval set to 3", "Shutdown flag when threshold exceeded set to true"])

    # Only test minimum warning threshold (i.e. 6) to trigger warning as much as possible
    testInterval("Resmon enabled: set warning interval", 
        "--plugin sysio::resource_monitor_plugin --resource-monitor-space-threshold=6 --resource-monitor-warning-interval=5 --resource-monitor-not-shutdown-on-threshold-exceeded",
        2 * 5, # Default monitor interval is 2 sec
        ["Space usage warning"],
        6)

    testInterval("Resmon enabled: default warning interval", 
        "--plugin sysio::resource_monitor_plugin --resource-monitor-space-threshold=6 --resource-monitor-interval-seconds=1 --resource-monitor-not-shutdown-on-threshold-exceeded",
        1 * 30, # Default warning interval is 30
        ["Space usage warning"],
        6)

args = TestHelper.parse_args({"--keep-logs","--dump-error-details","-v","--leave-running","--unshared"})
debug=args.v
pnodes=1
topo="mesh"
delay=1
chainSyncStrategyStr=Utils.SyncResyncTag
total_nodes = pnodes
killCount=1
killSignal=Utils.SigKillTag

dumpErrorDetails=args.dump_error_details

seed=1
Utils.Debug=debug
testSuccessful=False

try:
    TestHelper.printSystemInfo("BEGIN")

    testAll()

    testSuccessful=True
finally:
    if debug: Print("Cleanup in finally block.")
    cleanDirectories()

exitCode = 0 if testSuccessful else 1
if debug: Print("Exiting test, exit value %d." % (exitCode))
exit(exitCode)
