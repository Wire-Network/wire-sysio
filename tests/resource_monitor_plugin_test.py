#!/usr/bin/env python3

import random
import subprocess
import signal
import os
import shutil
import shlex
import re
from datetime import datetime
from datetime import timedelta

from TestHarness import Cluster, TestHelper, Utils

Print=Utils.Print
errorExit=Utils.errorExit

stagingDir=os.path.join(Utils.DataPath, "resource-monitor")
dataDir=stagingDir+"/data"
configDir=stagingDir+"/etc"
traceDir=dataDir+"/trace"
stateHistoryDir=os.path.join(dataDir, "state-history")

loggingFile=configDir+"/logging.json"
stderrFile=dataDir + "/stderr.txt"

testNum=0
max_start_time_secs=10 # time nodeop takes to start

# We need debug level to get more information about nodeop process
logging="""{
  "includes": [],
  "sinks": [{
    "name": "stderr_color",
    "type": "console_sink",
    "args": {
      "output_type": "stderr",
      "color": true,
      "level_colors": [{
        "level": "debug",
        "color": "green"
      },{
        "level": "info",
        "color": "reset"
      },{
        "level": "warn",
        "color": "yellow"
      },{
        "level": "error",
        "color": "red"
      }
      ]
    }
  }
  ],
  "loggers": [{
      "name": "default",
      "level": "debug",
      "enabled": true,
      "sinks": [
        "stderr_color"
      ]
    }
  ]
}"""

def cleanDirectories():
    """Remove resource monitor staging directories created by this test."""
    os.path.exists(stagingDir) and shutil.rmtree(stagingDir)

def cleanTestLogDirectory():
    """Remove this test's harness log directory after a successful run."""
    if not args.keep_logs:
        shutil.rmtree(Utils.DataPath, ignore_errors=True)

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
    cmd = (f"programs/nodeop/nodeop --config-dir {shlex.quote(configDir)} -e -p sysio "
           f"--plugin sysio::chain_api_plugin --data-dir {shlex.quote(dataDir)} "
           f"--http-server-address 127.0.0.1:{Utils.getPort(Utils.PortNodeHttp)} "
           f"--p2p-listen-endpoint 127.0.0.1:{Utils.getPort(Utils.PortP2P)} ")

    cmd=cmd + extraNodeopArgs
    if debug: Print("cmd: %s" % (cmd))
    with open(stderrFile, 'w') as serr:
        proc=subprocess.Popen(shlex.split(cmd), stdout=subprocess.PIPE, stderr=serr)

        try:
            proc.communicate(timeout=myTimeout)
        except (subprocess.TimeoutExpired) as _:
            if debug: Print("Timed out\n")
            proc.kill()
            proc.communicate()  # Reap before another case reuses its paths and lease.

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
    matches = re.compile(r"\s+([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}.[0-9]{6})\s").search(msg)
    return datetime.strptime(matches.group(0).strip(), '%Y-%m-%dT%H:%M:%S.%f')


# A warning may recur this many times its interval after the previous one before the test fails.
maxIntervalFactor = 2

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
                    # Never early; late by up to one interval for a loaded host (exact tick counts: test_resmon_plugin).
                    if timedelta(seconds=expectedInterval) <= curInterval <= timedelta(seconds=expectedInterval * maxIntervalFactor):
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
        subprocess.run(['dd', 'if=/dev/zero', f'of={fillerFile}',
                        f'count={int(filesize)}', 'bs=1M'], check=True)

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
            errorExit ("Log containing \"%s\" should recur every %d to %d seconds" % (msg, interval, interval * maxIntervalFactor))

def testAll():
    testCommon("Resmon enabled: all arguments", "--plugin  sysio::resource_monitor_plugin --resource-monitor-space-threshold=85 --resource-monitor-interval-seconds=5 --resource-monitor-not-shutdown-on-threshold-exceeded", ["threshold set to 85", "interval set to 5", "Shutdown flag when threshold exceeded set to false"])

    # default arguments and default directories to be monitored
    testCommon("Resmon not enabled: no arguments", "", ["interval set to 2", "threshold set to 90", "Shutdown flag when threshold exceeded set to true", "snapshots's file system to be monitored", "blocks's file system to be monitored", "state's file system to be monitored"])
    
    # default arguments with registered directories
    testCommon("Resmon not enabled: Producer, Chain, State History and Trace Api", f"--plugin sysio::state_history_plugin --state-history-endpoint 127.0.0.1:{Utils.getPort(Utils.PortStateHistory)} --state-history-dir={shlex.quote(stateHistoryDir)} --disable-replay-opts --plugin sysio::trace_api_plugin --trace-dir={shlex.quote(traceDir)}", ["interval set to 2", "threshold set to 90", "Shutdown flag when threshold exceeded set to true", "snapshots's file system to be monitored", "blocks's file system to be monitored", "state's file system to be monitored", "state-history's file system to be monitored", "trace's file system to be monitored"])

    testCommon("Resmon enabled: Producer, Chain, State History and Trace Api", f"--plugin  sysio::resource_monitor_plugin --plugin sysio::state_history_plugin --state-history-endpoint 127.0.0.1:{Utils.getPort(Utils.PortStateHistory)} --state-history-dir={shlex.quote(stateHistoryDir)} --disable-replay-opts --plugin sysio::trace_api_plugin --trace-dir={shlex.quote(traceDir)} --resource-monitor-space-threshold=80 --resource-monitor-interval-seconds=3", ["snapshots's file system to be monitored", "blocks's file system to be monitored", "state's file system to be monitored", "state-history's file system to be monitored", "trace's file system to be monitored", "threshold set to 80", "interval set to 3", "Shutdown flag when threshold exceeded set to true"])

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
    if testSuccessful:
        cleanTestLogDirectory()

exitCode = 0 if testSuccessful else 1
if debug: Print("Exiting test, exit value %d." % (exitCode))
exit(exitCode)
