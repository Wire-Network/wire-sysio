#!/usr/bin/env python3
import json
import os
import shutil
import time
import unittest

from testUtils import Utils
from testUtils import Account
from TestHelper import TestHelper
from Node import Node
from WalletMgr import WalletMgr

class PluginHttpTest(unittest.TestCase):
    sleep_s = 2
    base_node_cmd_str = ("curl http://%s:%s/v1/") % (TestHelper.LOCAL_HOST, TestHelper.DEFAULT_PORT)
    base_wallet_cmd_str = ("curl http://%s:%s/v1/") % (TestHelper.LOCAL_HOST, TestHelper.DEFAULT_WALLET_PORT)
    kiod = WalletMgr(True, TestHelper.DEFAULT_PORT, TestHelper.LOCAL_HOST, TestHelper.DEFAULT_WALLET_PORT, TestHelper.LOCAL_HOST)
    node_id = 1
    nodeop = Node(TestHelper.LOCAL_HOST, TestHelper.DEFAULT_PORT, node_id, walletMgr=kiod)
    data_dir = Utils.getNodeDataDir(node_id)
    http_post_str = " -X POST -d "
    http_post_invalid_param = " '{invalid}' "
    empty_content_str = " ' { } '  "
    SYSIO_ACCT_PRIVATE_DEFAULT_KEY = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
    SYSIO_ACCT_PUBLIC_DEFAULT_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

    # make a fresh data dir
    def createDataDir(self):
        if os.path.exists(self.data_dir):
            shutil.rmtree(self.data_dir)
        os.makedirs(self.data_dir)

    # kill nodeop and kiod and clean up dir
    def cleanEnv(self) :
        self.kiod.killall(True)
        WalletMgr.cleanup()
        Node.killAllNodeop()
        if os.path.exists(self.data_dir):
            shutil.rmtree(self.data_dir)
        time.sleep(self.sleep_s)

    # start kiod and nodeop
    def startEnv(self) :
        self.createDataDir(self)
        self.kiod.launch()
        nodeop_plugins = (" --plugin %s --plugin %s --plugin %s --plugin %s --plugin %s --plugin %s --plugin %s"
                          " --plugin %s --plugin %s --plugin %s --plugin %s ") % ( "sysio::trace_api_plugin",
                                                                                   "sysio::test_control_api_plugin",
                                                                                   "sysio::test_control_plugin",
                                                                                   "sysio::net_plugin",
                                                                                   "sysio::net_api_plugin",
                                                                                   "sysio::producer_plugin",
                                                                                   "sysio::producer_api_plugin",
                                                                                   "sysio::chain_api_plugin",
                                                                                   "sysio::http_plugin",
                                                                                   "sysio::db_size_api_plugin")
        nodeop_flags = (" --data-dir=%s --trace-dir=%s --trace-no-abis --access-control-allow-origin=%s "
                        "--contracts-console --http-validate-host=%s --verbose-http-errors --s-chain-contract grace --s-chain-actions dance "
                        "--p2p-peer-address localhost:9011 --resource-monitor-not-shutdown-on-threshold-exceeded ") % (self.data_dir, self.data_dir, "\'*\'", "false")
        start_nodeop_cmd = ("%s -e -p sysio %s %s ") % (Utils.SysServerPath, nodeop_plugins, nodeop_flags)
        self.nodeop.launchCmd(start_nodeop_cmd, self.node_id)
        time.sleep(self.sleep_s)

    def activateAllBuiltinProtocolFeatures(self):
        self.nodeop.activatePreactivateFeature()

        contract = "sysio.bios"
        contractDir = "unittests/contracts/old_versions/v1.7.0-develop-preactivate_feature/%s" % (contract)
        wasmFile = "%s.wasm" % (contract)
        abiFile = "%s.abi" % (contract)

        sysioAccount = Account("sysio")
        sysioAccount.ownerPrivateKey = sysioAccount.activePrivateKey = self.SYSIO_ACCT_PRIVATE_DEFAULT_KEY
        sysioAccount.ownerPublicKey = sysioAccount.activePublicKey = self.SYSIO_ACCT_PUBLIC_DEFAULT_KEY

        testWalletName = "test"
        walletAccounts = [sysioAccount]
        self.kiod.create(testWalletName, walletAccounts)

        retMap = self.nodeop.publishContract(sysioAccount.name, contractDir, wasmFile, abiFile, waitForTransBlock=True)

        self.nodeop.preactivateAllBuiltinProtocolFeature()

    # test all chain api
    def test_Simple(self) :
        cmd_base = self.base_node_cmd_str + "chain/"

        # get_info without parameter
        default_cmd = cmd_base + "get_info"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("server_version", ret_json)
        # get_info with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("server_version", ret_json)
        # get_info with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)

        # activate the builtin protocol features and get some useful data
        self.activateAllBuiltinProtocolFeatures()

        grace = Account("grace")
        grace.ownerPrivateKey = self.SYSIO_ACCT_PRIVATE_DEFAULT_KEY
        grace.ownerPublicKey = self.SYSIO_ACCT_PUBLIC_DEFAULT_KEY
        self.nodeop.createAccount(grace, self.nodeop.getSysioAccount(), stakedDeposit=1000, waitForTransBlock=True)
        self.nodeop.publishContract(grace.name, "unittests/test-contracts/dancer", "dancer.wasm", "dancer.abi", waitForTransBlock=True)

        # execute a dance transaction on the grace contract
        danceAction = {"account": grace.name, "name": "dance", "authorization": [{"actor": grace.name, "permission": "active"}], "data": ""}
        danceTxn = {
            "actions": [{"account": "grace","name": "dance",
                         "authorization": [{"actor": "grace","permission": "active"}],
                         "compression": "none"}]
        }
        danceTrxId = self.nodeop.pushTransaction(danceTxn, permissions=grace.name)

        print("*****************")
        print(danceTrxId)
        print("*****************")


        raise Exception("Not implemented yet")


        # get_block with empty parameter
        default_cmd = cmd_base + "get_block"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"block_num_or_id\":1}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["block_num"], 1)

        # get_block_info with empty parameter
        default_cmd = cmd_base + "get_block_info"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block_info with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block_info with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block_info with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"block_num\":1}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["block_num"], 1)

        # get_block_header_state with empty parameter
        default_cmd = cmd_base + "get_block_header_state"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_block_header_state with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_block_header_state with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_block_header_state with valid parameter, the irreversible is not available, unknown block number
        valid_cmd = default_cmd + self.http_post_str + "'{\"block_num_or_id\":1}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3100002)

        # get_account with empty parameter
        default_cmd = cmd_base + "get_account"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_account with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_account with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_account with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"account_name\":\"default\"}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_code with empty parameter
        default_cmd = cmd_base + "get_code"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_code with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_code with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_code with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"account_name\":\"default\"}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_code_hash with empty parameter
        default_cmd = cmd_base + "get_code_hash"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_code_hash with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_code_hash with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_code_hash with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"account_name\":\"default\"}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_abi with empty parameter
        default_cmd = cmd_base + "get_abi"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_abi with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_abi with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_abi with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"account_name\":\"default\"}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_raw_code_and_abi with empty parameter
        default_cmd = cmd_base + "get_raw_code_and_abi"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_raw_code_and_abi with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_raw_code_and_abi with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_raw_code_and_abi with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"account_name\":\"default\"}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_raw_abi with empty parameter
        default_cmd = cmd_base + "get_raw_abi"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_raw_abi with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_raw_abi with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_raw_abi with valid parameter
        valid_cmd = default_cmd + self.http_post_str + "'{\"account_name\":\"default\"}'"
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_table_rows with empty parameter
        default_cmd = cmd_base + "get_table_rows"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_table_rows with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_table_rows with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_table_rows with valid parameter
        valid_cmd = ("%s%s '{%s,%s,%s,%s,%s,%s,%s,%s}'") % (  default_cmd,
                                                              self.http_post_str,
                                                              "\"json\":true",
                                                              "\"code\":\"cancancan345\"",
                                                              "\"scope\":\"cancancan345\"",
                                                              "\"table\":\"vote\"",
                                                              "\"index_position\":2",
                                                              "\"key_type\":\"i128\"",
                                                              "\"lower_bound\":\"0x0000000000000000D0F2A472A8EB6A57\"",
                                                              "\"upper_bound\":\"0xFFFFFFFFFFFFFFFFD0F2A472A8EB6A57\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_table_by_scope with empty parameter
        default_cmd = cmd_base + "get_table_by_scope"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_table_by_scope with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_table_by_scope with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_table_by_scope with valid parameter
        valid_cmd = ("%s%s '{%s,%s,%s,%s,%s}'") % (   default_cmd,
                                                      self.http_post_str,
                                                      "\"code\":\"cancancan345\"",
                                                      "\"table\":\"vote\"",
                                                      "\"index_position\":2",
                                                      "\"lower_bound\":\"0x0000000000000000D0F2A472A8EB6A57\"",
                                                      "\"upper_bound\":\"0xFFFFFFFFFFFFFFFFD0F2A472A8EB6A57\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_currency_balance with empty parameter
        default_cmd = cmd_base + "get_currency_balance"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_currency_balance with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_currency_balance with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_currency_balance with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"code\":\"sysio.token\", \"account\":\"unknown\"}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_currency_stats with empty parameter
        default_cmd = cmd_base + "get_currency_stats"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_currency_stats with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_currency_stats with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_currency_stats with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"code\":\"sysio.token\",\"symbol\":\"SYS\"}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_producers with empty parameter
        default_cmd = cmd_base + "get_producers"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_producers with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_producers with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_producers with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"json\":true,\"lower_bound\":\"\"}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(type(ret_json["rows"]), list)

        # get_producer_schedule with empty parameter
        default_cmd = cmd_base + "get_producer_schedule"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(type(ret_json["active"]), dict)
        # get_producer_schedule with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(type(ret_json["active"]), dict)
        # get_producer_schedule with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # abi_json_to_bin with empty parameter
        default_cmd = cmd_base + "abi_json_to_bin"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # abi_json_to_bin with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # abi_json_to_bin with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # abi_json_to_bin with valid parameter
        valid_cmd = ("%s%s '{%s,%s,%s}'") % (default_cmd,
                                             self.http_post_str,
                                             "\"code\":\"sysio.token\"",
                                             "\"action\":\"issue\"",
                                             "\"args\":{\"to\":\"sysio.token\", \"quantity\":\"1.0000\\%20SYS\",\"memo\":\"m\"}")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # abi_bin_to_json with empty parameter
        default_cmd = cmd_base + "abi_bin_to_json"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # abi_bin_to_json with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # abi_bin_to_json with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # abi_bin_to_json with valid parameter
        valid_cmd = ("%s%s '{%s,%s,%s}'") % (default_cmd,
                                             self.http_post_str,
                                             "\"code\":\"sysio.token\"",
                                             "\"action\":\"issue\"",
                                             "\"args\":\"ee6fff5a5c02c55b6304000000000100a6823403ea3055000000572d3ccdcd0100000000007015d600000000a8ed32322a00000000007015d6000000005c95b1ca102700000000000004454f53000000000968656c6c6f206d616e00\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_required_keys with empty parameter
        default_cmd = cmd_base + "get_required_keys"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_required_keys with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_required_keys with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_required_keys with valid parameter
        valid_cmd = ("%s%s '{\"transaction\":{%s,%s,%s,%s,%s,%s,%s},\"available_keys\":[%s,%s,%s]}'") % \
                    (default_cmd,
                     self.http_post_str,
                     "\"ref_block_num\":\"100\"",
                     "\"ref_block_prefix\": \"137469861\"",
                     "\"expiration\": \"2020-09-25T06:28:49\"",
                     "\"scope\":[\"initb\", \"initc\"]",
                     "\"actions\": [{\"code\": \"currency\",\"type\":\"transfer\",\"recipients\": [\"initb\", \"initc\"],\"authorization\": [{\"account\": \"initb\", \"permission\": \"active\"}],\"data\":\"000000000041934b000000008041934be803000000000000\"}]",
                     "\"signatures\": []",
                     "\"authorizations\": []",
                     "\"SYS4toFS3YXEQCkuuw1aqDLrtHim86Gz9u3hBdcBw5KNPZcursVHq\"",
                     "\"SYS7d9A3uLe6As66jzN8j44TXJUqJSK3bFjjEEqR4oTvNAB3iM9SA\"",
                     "\"SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_transaction_id with empty parameter
        default_cmd = cmd_base + "get_transaction_id"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_transaction_id with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_transaction_id with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_transaction_id with valid parameter
        valid_cmd = ("%s%s '{%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s}'") % \
                    (default_cmd,
                     self.http_post_str,
                     "\"expiration\":\"2020-08-01T07:15:49\"",
                     "\"ref_block_num\": 34881",
                     "\"ref_block_prefix\":2972818865",
                     "\"max_net_usage_words\":0",
                     "\"max_cpu_usage_ms\":0",
                     "\"delay_sec\":0",
                     "\"context_free_actions\":[]",
                     "\"actions\":[{\"account\":\"sysio.token\",\"name\": \"transfer\",\"authorization\": [{\"actor\": \"han\",\"permission\": \"active\"}],\"data\": \"000000000000a6690000000000ea305501000000000000000453595300000000016d\"}]",
                     "\"transaction_extensions\": []",
                     "\"signatures\": [\"SIG_K1_KeqfqiZu1GwUxQb7jzK9Fdks6HFaVBQ9AJtCZZj56eG9qGgvVMVtx8EerBdnzrhFoX437sgwtojf2gfz6S516Ty7c22oEp\"]",
                     "\"context_free_data\": []")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        # verify the returned transaction id.
        self.assertEqual(ret_str, "\"9b807e8b37cfb9e30aa9d68f257b3170addf5805b8bd1df2455f8893977c9f85\"")

        # push_block with empty parameter
        default_cmd = cmd_base + "push_block"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_block with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_block with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_block with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"block\":\"signed_block\"}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(len(ret_json), 0)

        # push_transaction with empty parameter
        default_cmd = cmd_base + "push_transaction"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_transaction with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_transaction with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_transaction with valid parameter
        valid_cmd = ("%s%s '{%s, %s, %s, %s}'") % (default_cmd,
                                                   self.http_post_str,
                                                   "\"signatures\":[\"SIG_K1_KeqfqiZu1GwUxQb7jzK9Fdks6HFaVBQ9AJtCZZj56eG9qGgvVMVtx8EerBdnzrhFoX437sgwtojf2gfz6S516Ty7c22oEp\"]",
                                                   "\"compression\": true",
                                                   "\"packed_context_free_data\": \"context_free_data\"",
                                                   "\"packed_trx\": \"packed_trx\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # push_transactions with empty parameter
        default_cmd = cmd_base + "push_transactions"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_transactions with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_transactions with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # push_transactions with valid parameter
        valid_cmd = ("%s%s '[{%s, %s, %s, %s}]'") % (default_cmd,
                                                   self.http_post_str,
                                                   "\"signatures\":[\"SIG_K1_KeqfqiZu1GwUxQb7jzK9Fdks6HFaVBQ9AJtCZZj56eG9qGgvVMVtx8EerBdnzrhFoX437sgwtojf2gfz6S516Ty7c22oEp\"]",
                                                   "\"compression\": true",
                                                   "\"packed_context_free_data\": \"context_free_data\"",
                                                   "\"packed_trx\": \"packed_trx\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn("transaction_id", ret_json[0])

        # send_transaction with empty parameter
        default_cmd = cmd_base + "send_transaction"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # send_transaction with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # send_transaction with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # send_transaction with valid parameter
        valid_cmd = ("%s%s '{%s, %s, %s, %s}'") % (default_cmd,
                                                   self.http_post_str,
                                                   "\"signatures\":[\"SIG_K1_KeqfqiZu1GwUxQb7jzK9Fdks6HFaVBQ9AJtCZZj56eG9qGgvVMVtx8EerBdnzrhFoX437sgwtojf2gfz6S516Ty7c22oEp\"]",
                                                   "\"compression\": true",
                                                   "\"packed_context_free_data\": \"context_free_data\"",
                                                   "\"packed_trx\": \"packed_trx\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)


    # test all net api
    def notest_NetApi(self) :
        cmd_base = self.base_node_cmd_str + "net/"

        # connect with empty parameter
        default_cmd = cmd_base + "connect"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # connect with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # connect with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # connect with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"localhost\"'")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        self.assertEqual("\"added connection\"", ret_str)

        # disconnect with empty parameter
        default_cmd = cmd_base + "disconnect"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # disconnect with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # disconnect with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # disconnect with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"localhost123\"'")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        self.assertEqual("\"no known connection for host\"", ret_str)

        # status with empty parameter
        default_cmd = cmd_base + "status"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # status with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # status with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # status with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"localhost\"'")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        self.assertEqual(ret_str, "null")

        # connections with empty parameter
        default_cmd = cmd_base + "connections"
        ret_str = Utils.runCmdReturnStr(default_cmd)
        self.assertIn("\"peer\":\"localhost:9011\"", ret_str)
        # connections with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_str = Utils.runCmdReturnStr(default_cmd)
        self.assertIn("\"peer\":\"localhost:9011\"", ret_str)
        # connections with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

    # test all producer api
    def notest_ProducerApi(self) :
        cmd_base = self.base_node_cmd_str + "producer/"

        # pause with empty parameter
        default_cmd = cmd_base + "pause"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["result"], "ok")
        # pause with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["result"], "ok")
        # pause with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # resume with empty parameter
        default_cmd = cmd_base + "resume"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["result"], "ok")
        # resume with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["result"], "ok")
        # resume with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # paused with empty parameter
        default_cmd = cmd_base + "paused"
        ret_str = Utils.runCmdReturnStr(default_cmd)
        self.assertEqual(ret_str, "false")
        # paused with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_str = Utils.runCmdReturnStr(default_cmd)
        self.assertEqual(ret_str, "false")
        # paused with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # get_runtime_options with empty parameter
        default_cmd = cmd_base + "get_runtime_options"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("max_transaction_time", ret_json)
        # get_runtime_options with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("max_transaction_time", ret_json)
        # get_runtime_options with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # update_runtime_options with empty parameter
        default_cmd = cmd_base + "update_runtime_options"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # update_runtime_options with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # update_runtime_options with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # update_runtime_options with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{%s, %s, %s, %s, %s, %s, %s, %s}'") % ("\"max_transaction_time\":30",
                                                                                                 "\"max_irreversible_block_age\":1",
                                                                                                 "\"produce_time_offset_us\":10000",
                                                                                                 "\"last_block_time_offset_us\":0",
                                                                                                 "\"max_scheduled_transaction_time_per_block_ms\":10000",
                                                                                                 "\"subjective_cpu_leeway_us\":0",
                                                                                                 "\"incoming_defer_ratio\":1.0",
                                                                                                 "\"greylist_limit\":100")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn(ret_json["result"], "ok")

        # add_greylist_accounts with empty parameter
        default_cmd = cmd_base + "add_greylist_accounts"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # add_greylist_accounts with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # add_greylist_accounts with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # add_greylist_accounts with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"accounts\":[\"test1\", \"test2\"]}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn(ret_json["result"], "ok")

        # remove_greylist_accounts with empty parameter
        default_cmd = cmd_base + "remove_greylist_accounts"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # remove_greylist_accounts with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # remove_greylist_accounts with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # remove_greylist_accounts with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"accounts\":[\"test1\", \"test2\"]}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn(ret_json["result"], "ok")

        # get_greylist with empty parameter
        default_cmd = cmd_base + "get_greylist"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("accounts", ret_json)
        # get_greylist with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("accounts", ret_json)
        # get_greylist with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # get_whitelist_blacklist with empty parameter
        default_cmd = cmd_base + "get_whitelist_blacklist"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("actor_whitelist", ret_json)
        self.assertIn("actor_blacklist", ret_json)
        self.assertIn("contract_whitelist", ret_json)
        self.assertIn("contract_blacklist", ret_json)
        self.assertIn("action_blacklist", ret_json)
        self.assertIn("key_blacklist", ret_json)
        # get_whitelist_blacklist with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("actor_whitelist", ret_json)
        self.assertIn("actor_blacklist", ret_json)
        self.assertIn("contract_whitelist", ret_json)
        self.assertIn("contract_blacklist", ret_json)
        self.assertIn("action_blacklist", ret_json)
        self.assertIn("key_blacklist", ret_json)
        # get_whitelist_blacklist with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # set_whitelist_blacklist with empty parameter
        default_cmd = cmd_base + "set_whitelist_blacklist"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # set_whitelist_blacklist with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # set_whitelist_blacklist with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # set_whitelist_blacklist with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{%s, %s, %s, %s, %s, %s}'") % ("\"actor_whitelist\":[\"test2\"]",
                                                                                         "\"actor_blacklist\":[\"test3\"]",
                                                                                         "\"contract_whitelist\":[\"test4\"]",
                                                                                         "\"contract_blacklist\":[\"test5\"]",
                                                                                         "\"action_blacklist\":[]",
                                                                                         "\"key_blacklist\":[]")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn(ret_json["result"], "ok")

        # get_integrity_hash with empty parameter
        default_cmd = cmd_base + "get_integrity_hash"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("head_block_id", ret_json)
        self.assertIn("integrity_hash", ret_json)
        # get_integrity_hash with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("head_block_id", ret_json)
        self.assertIn("integrity_hash", ret_json)
        # get_integrity_hash with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # create_snapshot with empty parameter
        default_cmd = cmd_base + "create_snapshot"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("head_block_id", ret_json)
        self.assertIn("snapshot_name", ret_json)

        # get_scheduled_protocol_feature_activations with empty parameter
        default_cmd = cmd_base + "get_scheduled_protocol_feature_activations"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("protocol_features_to_activate", ret_json)
        # get_scheduled_protocol_feature_activations with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("protocol_features_to_activate", ret_json)
        # get_scheduled_protocol_feature_activations with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # schedule_protocol_feature_activations with empty parameter
        default_cmd = cmd_base + "schedule_protocol_feature_activations"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # schedule_protocol_feature_activations with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # schedule_protocol_feature_activations with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # schedule_protocol_feature_activations with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"protocol_features_to_activate\":[]}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn(ret_json["result"], "ok")

        # get_supported_protocol_features with empty parameter
        default_cmd = cmd_base + "get_supported_protocol_features"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("feature_digest", ret_json[0])
        self.assertIn("subjective_restrictions", ret_json[0])
        # get_supported_protocol_features with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("feature_digest", ret_json[0])
        self.assertIn("subjective_restrictions", ret_json[0])
        # get_supported_protocol_features with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_supported_protocol_features with 1st parameter
        params_1st_cmd = default_cmd + self.http_post_str + ("'{\"exclude_disabled\":true}'")
        ret_json = Utils.runCmdReturnJson(params_1st_cmd)
        self.assertIn("feature_digest", ret_json[0])
        self.assertIn("subjective_restrictions", ret_json[0])
        # get_supported_protocol_features with 2nd parameter
        params_2nd_cmd = default_cmd + self.http_post_str + ("'{\"exclude_unactivatable\":true}'")
        ret_json = Utils.runCmdReturnJson(params_2nd_cmd)
        self.assertIn("feature_digest", ret_json[0])
        self.assertIn("subjective_restrictions", ret_json[0])
        # get_supported_protocol_features with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"exclude_disabled\":true, \"exclude_unactivatable\":true}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn("feature_digest", ret_json[0])
        self.assertIn("subjective_restrictions", ret_json[0])

        # get_account_ram_corrections with empty parameter
        default_cmd = cmd_base + "get_account_ram_corrections"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_account_ram_corrections with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_account_ram_corrections with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # get_account_ram_corrections with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"lower_bound\":\"\", \"upper_bound\":\"\", \"limit\":1, \"reverse\":false}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertIn("rows", ret_json)

    # test all wallet api
    def notest_WalletApi(self) :
        cmd_base = self.base_wallet_cmd_str + "wallet/"

        # set_timeout with empty parameter
        default_cmd = cmd_base + "set_timeout"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # set_timeout with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # set_timeout with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # set_timeout with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"1234567\"'")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        self.assertEqual("{}", ret_str)

        # sign_transaction with empty parameter
        default_cmd = cmd_base + "sign_transaction"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # sign_transaction with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # sign_transaction with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # sign_transaction with valid parameter
        signed_transaction = ("{%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s}") % ("\"expiration\":\"2020-08-01T07:15:49\"",
                                                                        "\"ref_block_num\": 34881",
                                                                        "\"ref_block_prefix\":2972818865",
                                                                        "\"max_net_usage_words\":0",
                                                                        "\"max_cpu_usage_ms\":0",
                                                                        "\"delay_sec\":0",
                                                                        "\"context_free_actions\":[]",
                                                                        "\"actions\": []",
                                                                        "\"transaction_extensions\": []",
                                                                        "\"signatures\": [\"SIG_K1_KeqfqiZu1GwUxQb7jzK9Fdks6HFaVBQ9AJtCZZj56eG9qGgvVMVtx8EerBdnzrhFoX437sgwtojf2gfz6S516Ty7c22oEp\"]",
                                                                        "\"context_free_data\": []")
        valid_cmd = default_cmd + self.http_post_str + ("'[%s, %s, %s]'") % (signed_transaction,
                                                                             "[\"SYS696giL6VxeJhtEgKtWPK8aQeT8YXNjw2a7vE5wHunffhfa5QSQ\"]",
                                                                             "\"cf057bbfb72640471fd910bcb67639c22df9f92470936cddc1ade0e2f2e7dc4f\"")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)
        self.assertEqual(ret_json["error"]["code"], 3120004)

        # create with empty parameter
        default_cmd = cmd_base + "create"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"test1\"'")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        self.assertTrue( ret_str)

        # open with empty parameter
        default_cmd = cmd_base + "open"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # open with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"fakeacct\"'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # lock_all with empty parameter
        default_cmd = cmd_base + "lock_all"
        ret_str = Utils.runCmdReturnStr(default_cmd)
        self.assertEqual("{}", ret_str)
        # lock_all with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_str = Utils.runCmdReturnStr(default_cmd)
        self.assertEqual("{}", ret_str)
        # lock_all with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # lock with empty parameter
        default_cmd = cmd_base + "lock"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # lock with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # lock with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # lock with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'\"name\":\"auser\"'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # unlock with empty parameter
        default_cmd = cmd_base + "unlock"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # unlock with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # unlock with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # unlock with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'[\"auser\", \"nopassword\"]'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # import_key with empty parameter
        default_cmd = cmd_base + "import_key"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # import_key with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # import_key with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # import_key with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'[\"auser\", \"nokey\"]'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # remove_key with empty parameter
        default_cmd = cmd_base + "remove_key"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # remove_key with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # remove_key with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # remove_key with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'[\"auser\", \"none\", \"none\"]'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # create_key with empty parameter
        default_cmd = cmd_base + "remove_key"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create_key with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create_key with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # create_key with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'[\"auser\", \"none\", \"none\"]'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # list_wallets with empty parameter
        default_cmd = cmd_base + "list_wallets"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(type(ret_json), list)
        # list_wallets with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(type(ret_json), list)
        # list_wallets with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

        # list_keys with empty parameter
        default_cmd = cmd_base + "list_keys"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # list_keys with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # list_keys with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # list_keys with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'[\"auser\", \"unknownkey\"]'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 500)

        # get_public_keys with empty parameter
        default_cmd = cmd_base + "get_public_keys"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(type(ret_json), dict)
        # get_public_keys with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(type(ret_json), dict)
        # list_wallets with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)

    # test all test control api
    def notest_TestControlApi(self) :
        cmd_base = self.base_node_cmd_str + "test_control/"

        # kill_node_on_producer with empty parameter
        default_cmd = cmd_base + "kill_node_on_producer"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # kill_node_on_producer with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # kill_node_on_producer with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        self.assertEqual(ret_json["error"]["code"], 3200006)
        # kill_node_on_producer with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"name\":\"auser\", \"where_in_sequence\":12, \"based_on_lib\":true}'")
        ret_str = Utils.runCmdReturnStr(valid_cmd)
        self.assertIn("{}", ret_str)

    # test all trace api
    def notest_TraceApi(self) :
        cmd_base = self.base_node_cmd_str + "trace_api/"

        # get_block with empty parameter
        default_cmd = cmd_base + "get_block"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)
        # get_block with valid parameter
        valid_cmd = default_cmd + self.http_post_str + ("'{\"block_num\":1}'")
        ret_json = Utils.runCmdReturnJson(valid_cmd)
        self.assertEqual(ret_json["code"], 404)
        self.assertEqual(ret_json["error"]["code"], 0)

    # test all db_size api
    def notest_DbSizeApi(self) :
        cmd_base = self.base_node_cmd_str + "db_size/"

        # get with empty parameter
        default_cmd = cmd_base + "get"
        ret_json = Utils.runCmdReturnJson(default_cmd)
        self.assertIn("free_bytes", ret_json)
        self.assertIn("used_bytes", ret_json)
        self.assertIn("size", ret_json)
        self.assertIn("indices", ret_json)
        # get with empty content parameter
        empty_content_cmd = default_cmd + self.http_post_str + self.empty_content_str
        ret_json = Utils.runCmdReturnJson(empty_content_cmd)
        self.assertIn("free_bytes", ret_json)
        self.assertIn("used_bytes", ret_json)
        self.assertIn("size", ret_json)
        self.assertIn("indices", ret_json)
        # get with invalid parameter
        invalid_cmd = default_cmd + self.http_post_str + self.http_post_invalid_param
        ret_json = Utils.runCmdReturnJson(invalid_cmd)
        self.assertEqual(ret_json["code"], 400)


    @classmethod
    def setUpClass(self):
        self.cleanEnv(self)
        self.startEnv(self)

    @classmethod
    def tearDownClass(self):
        self.cleanEnv(self)

if __name__ == "__main__":
    unittest.main()
