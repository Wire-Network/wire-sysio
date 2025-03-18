#!/usr/bin/env python3

import os
import shutil
import time
import signal
from TestHarness import Node, TestHelper, Utils

node_id = 1
nodeop = Node(TestHelper.LOCAL_HOST, TestHelper.DEFAULT_PORT, node_id)
data_dir = Utils.getNodeDataDir(node_id)
config_dir = Utils.getNodeConfigDir(node_id)
if os.path.exists(data_dir):
    shutil.rmtree(data_dir)
os.makedirs(data_dir)
if not os.path.exists(config_dir):
    os.makedirs(config_dir)

try:
    start_nodeop_cmd = f"{Utils.SysServerPath} -e -p sysio --data-dir={data_dir} --config-dir={config_dir} --blocks-log-stride 10" \
                        " --plugin=sysio::http_plugin --plugin=sysio::chain_api_plugin --http-server-address=localhost:8888"

    nodeop.launchCmd(start_nodeop_cmd, node_id)
    time.sleep(2)
    nodeop.waitForBlock(30)
    nodeop.kill(signal.SIGTERM)

    nodeop.relaunch(chainArg="--replay-blockchain")

    time.sleep(2)
    assert nodeop.waitForBlock(31)
finally:
    # clean up
    Node.killAllNodeop()
