#!/usr/bin/env python3

import errno
import pathlib
import shutil
import signal
import socket

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

    # Explicit, because waitForBool raises instead of returning False at its 60 s default.
    waitTimeoutSeconds = 30
    p2pPort = Utils.getPort(Utils.PortP2P)
    pathlib.Path(Utils.ConfigDir).mkdir(parents=True, exist_ok=True)
    pathlib.Path(Utils.ConfigDir, 'config.ini').write_text(
        f'p2p-listen-endpoint = 127.0.0.1:{p2pPort}\n')

    cmd = [
        Utils.SysServerPath,
        '-e',
        '-p',
        'sysio',
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

    assert Utils.waitForBool(node.checkPulse, timeout=waitTimeoutSeconds), 'Positive-control HTTP API did not become ready'
    assert node.waitForBlock(5, timeout=waitTimeoutSeconds), 'Positive-control node did not produce blocks'
    with socket.create_connection(('127.0.0.1', p2pPort), timeout=5):
        pass
    assert node.kill(signal.SIGTERM), 'Positive-control node did not stop'

    # Same node, same leased port: the empty command-line endpoint must override the working config-file listener.
    cmd += ['--p2p-listen-endpoint', '']
    node = Node('localhost', TestHelper.DEFAULT_PORT, '00', data_dir=pathlib.Path(Utils.DataDir),
                config_dir=pathlib.Path(Utils.ConfigDir), cmd=cmd)
    assert Utils.waitForBool(node.checkPulse, timeout=waitTimeoutSeconds), 'HTTP API with P2P disabled did not become ready'
    assert node.waitForHeadToAdvance(), 'Node with P2P disabled did not continue producing'

    with socket.socket() as s:
        s.settimeout(5)
        err = s.connect_ex(('127.0.0.1', p2pPort))
    assert err == errno.ECONNREFUSED, f'Connection to port {p2pPort} must be refused; errno={err}'

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
