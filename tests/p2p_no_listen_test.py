#!/usr/bin/env python3

import errno
import pathlib
import shutil
import signal
import socket
import time

from TestHarness import Node, TestHelper, Utils

###############################################################
# p2p_no_listen_test
#
# Test nodeop disabling p2p
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"--keep-logs","-v","--leave-running","--unshared"})
debug=args.v

Utils.Debug=debug
testSuccessful=False

try:
    TestHelper.printSystemInfo("BEGIN")

    cmd = [
        Utils.SysServerPath,
        '-e',
        '-p',
        'sysio',
        '--p2p-listen-endpoint',
        '',
        '--plugin',
        'sysio::chain_api_plugin',
        '--config-dir',
        Utils.ConfigDir,
        '--data-dir',
        Utils.DataDir,
        '--http-server-address',
        f'localhost:{TestHelper.DEFAULT_PORT}'
    ]
    node = Node('localhost', TestHelper.DEFAULT_PORT, '00', data_dir=pathlib.Path(Utils.DataDir),
                config_dir=pathlib.Path(Utils.ConfigDir), cmd=cmd)

    time.sleep(1)
    if not node.verifyAlive():
        raise RuntimeError
    time.sleep(10)
    node.waitForBlock(5)

    s = socket.socket()
    p2pPort = Utils.shardPort(9876)
    err = s.connect_ex(('localhost', p2pPort))
    assert err == errno.ECONNREFUSED, f'Connection to port {p2pPort} must be refused'

    testSuccessful=True
finally:
    Utils.ShuttingDown=True

    if not args.leave_running:
        node.kill(signal.SIGTERM)

    if not (args.leave_running or args.keep_logs or not testSuccessful):
        shutil.rmtree(Utils.DataPath, ignore_errors=True)

    if testSuccessful:
        Utils.Print("Test succeeded.")
    else:
        Utils.Print("Test failed.")

exitCode = 0 if testSuccessful else 1
exit(exitCode)
