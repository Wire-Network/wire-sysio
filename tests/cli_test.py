#!/usr/bin/env python3

# This script tests that the compiled binaries produce expected output in
# response to the `--help` option. It also contains a couple of additional
# CLI-related checks as well as test cases for CLI bugfixes.

import datetime
import subprocess
import re
import os
import time
import shlex
import shutil
import signal
from pathlib import Path

from TestHarness import Account, Node, ReturnType, Utils, WalletMgr

testSuccessful=False

def nodeop_help_test():
    """Test that nodeop help contains option descriptions"""
    help_text = subprocess.check_output(["./programs/nodeop/nodeop", "--help"])

    assert(re.search(b'Application.*Options', help_text))
    assert(re.search(b'Options for .*_plugin', help_text))


def clio_help_test(args):
    """Test that clio help contains option and subcommand descriptions"""
    help_text = subprocess.check_output(["./programs/clio/clio"] + args)

    assert(b'Options:' in help_text)
    assert(b'Subcommands:' in help_text)


def cli11_bugfix_test():
    """Test that subcommand names can be used as option arguments"""
    completed_process = subprocess.run(
        ['./programs/clio/clio', '--no-auto-kiod', '-u', 'http://localhost:0/',
         'push', 'action', 'accout', 'action', '["data"]', '-p', 'wallet'],
        check=False,
        stderr=subprocess.PIPE)

    # The above command must fail because there is no server running
    # on localhost:0
    assert(completed_process.returncode != 0)

    # Make sure that the command failed because of the connection error,
    # not the command line parsing error.
    assert(b'Failed http request to nodeop' in completed_process.stderr)


def cli11_optional_option_arg_test():
    """Test that options like --password can be specified without a value"""
    chain = 'cf057bbfb72640471fd910bcb67639c22df9f92470936cddc1ade0e2f2e7dc4f'
    key = '5Jgfqh3svgBZvCAQkcnUX8sKmVUkaUekYDGqFakm52Ttkc5MBA4'

    output = subprocess.check_output(['./programs/clio/clio', '--no-auto-kiod', 'sign',
                                      '-c', chain, '-k', '{}'],
                                     input=key.encode(),
                                     stderr=subprocess.DEVNULL)
    assert(b'signatures' in output)

    output = subprocess.check_output(['./programs/clio/clio', '--no-auto-kiod', 'sign',
                                      '-c', chain, '-k', key, '{}'])
    assert(b'signatures' in output)

def clio_sign_test():
    """Test that sign can on both regular and packed transactions"""
    chain = 'cf057bbfb72640471fd910bcb67639c22df9f92470936cddc1ade0e2f2e7dc4f'
    key = '5Jgfqh3svgBZvCAQkcnUX8sKmVUkaUekYDGqFakm52Ttkc5MBA4'

    # regular trasaction
    trx = (
        '{'
        '"expiration": "2019-08-01T07:15:49",'
        '"ref_block_num": 34881,'
        '"ref_block_prefix": 2972818865,'
        '"max_net_usage_words": 0,'
        '"max_cpu_usage_ms": 0,'
        '"delay_sec": 0,'
        '"context_free_actions": [],'
        '"actions": [{'
            '"account": "sysio.token",'
            '"name": "transfer",'
            '"authorization": [{'
            '"actor": "sysio",'
            '"permission": "active"'
        '}'
        '],'
        '"data": "000000000000a6690000000000ea305501000000000000000453595300000000016d"'
       '}'
       '],'
        '"transaction_extensions": [],'
        '"context_free_data": []'
    '}')

    output = subprocess.check_output(['./programs/clio/clio', '--no-auto-kiod', 'sign',
                                      '-c', chain, '-k', key, trx])
    # make sure it is signed
    assert(b'signatures' in output)
    # make sure fields are kept
    assert(b'"expiration": "2019-08-01T07:15:49"' in output)
    assert(b'"ref_block_num": 34881' in output)
    assert(b'"ref_block_prefix": 2972818865' in output)
    assert(b'"account": "sysio.token"' in output)
    assert(b'"name": "transfer"' in output)
    assert(b'"actor": "sysio"' in output)
    assert(b'"permission": "active"' in output)
    assert(b'"data": "000000000000a6690000000000ea305501000000000000000453595300000000016d"' in output)

    packed_trx = ' { "signatures": [], "compression": "none", "packed_context_free_data": "", "packed_trx": "a591425d4188b19d31b1000000000100a6823403eab0c7000000572d3ccdcd010000000000eab0c700000000a8ed323222000000000000a6690000000000ea305501000000000000000453595300000000016d00" } '

    # Test packed transaction is unpacked. Only with options --print-request and --public-key
    # the sign request is dumped to stderr.
    cmd = ['./programs/clio/clio', '--print-request', '--no-auto-kiod', 'sign', '-c', chain, '--public-key', 'SYS8Dq1KosJ9PMn1vKQK3TbiihgfUiDBUsz471xaCE6eYUssPB1KY', packed_trx]
    outs=None
    errs=None
    try:
        popen=subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        outs,errs=popen.communicate()
        popen.wait()
    except subprocess.CalledProcessError as ex:
        print(f"STDOUT: {ex.output}")
        print(f"STDERR: {ex.stderr}")
    # make sure fields are unpacked
    assert(b'"expiration": "2019-08-01T07:15:49"' in errs)
    assert(b'"ref_block_num": 34881' in errs)
    assert(b'"ref_block_prefix": 2972818865' in errs)
    assert(b'"account": "sysio.token"' in errs)
    assert(b'"name": "transfer"' in errs)
    assert(b'"actor": "sysio"' in errs)
    assert(b'"permission": "active"' in errs)
    assert(b'"data": "000000000000a6690000000000ea305501000000000000000453595300000000016d"' in errs)

    # Test packed transaction is signed.
    output = subprocess.check_output(['./programs/clio/clio', '--no-auto-kiod', 'sign',
                                      '-c', chain, '-k', key, packed_trx])
    # Make sure signatures not empty
    assert(b'signatures' in output)
    assert(b'"signatures": []' not in output)

def processClioCommand(cmd):
    outs = None
    errs = None
    try:
        popen=subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        outs, errs = popen.communicate()
        popen.wait()
    except subprocess.CalledProcessError as ex:
        print(f"STDOUT: {ex.output}")
        print(f"STDERR: {ex.stderr}")
    return outs, errs

def clio_abi_file_test():
    """Test option --abi-file """
    token_abi_path = os.path.abspath(os.getcwd() + '/contracts/sysio.token/sysio.token.abi')
    system_abi_path = os.path.abspath(os.getcwd() + '/contracts/sysio.system/sysio.system.abi')
    token_abi_file_arg = 'sysio.token' + ':' + token_abi_path
    system_abi_file_arg = 'sysio' + ':' + system_abi_path

    # no option --abi-file
    account = 'sysio.token'
    action = 'transfer'
    unpacked_action_data = '{"from":"aaa","to":"bbb","quantity":"10.0000 SYS","memo":"hello"}'
    # use URL http://127.0.0.1:12345 to make sure clio not to connect to any running nodeop
    cmd = ['./programs/clio/clio', '-u', 'http://127.0.0.1:12345', 'convert', 'pack_action_data', account, action, unpacked_action_data]
    outs, errs = processClioCommand(cmd)
    assert(b'Failed http request to nodeop' in errs)

    # invalid option --abi-file
    invalid_abi_arg = 'sysio.token' + ' ' + token_abi_path
    cmd = ['./programs/clio/clio', '-u', 'http://127.0.0.1:12345', '--abi-file', invalid_abi_arg, 'convert', 'pack_action_data', account, action, unpacked_action_data]
    outs, errs = processClioCommand(cmd)
    assert(b'please specify --abi-file in form of <contract name>:<abi file path>.' in errs)

    # pack token transfer data
    account = 'sysio.token'
    action = 'transfer'
    unpacked_action_data = '{"from":"aaa","to":"bbb","quantity":"10.0000 SYS","memo":"hello"}'
    packed_action_data = '0000000000008c31000000000000ce39a08601000000000004535953000000000568656c6c6f'
    cmd = ['./programs/clio/clio', '-u','http://127.0.0.1:12345', '--abi-file', token_abi_file_arg, 'convert', 'pack_action_data', account, action, unpacked_action_data]
    outs, errs = processClioCommand(cmd)
    actual = outs.strip()
    assert(actual.decode('utf-8') == packed_action_data)

    # unpack token transfer data
    cmd = ['./programs/clio/clio', '-u','http://127.0.0.1:12345', '--abi-file', token_abi_file_arg, 'convert', 'unpack_action_data', account, action, packed_action_data]
    outs, errs = processClioCommand(cmd)
    assert(b'"from": "aaa"' in outs)
    assert(b'"to": "bbb"' in outs)
    assert(b'"quantity": "10.0000 SYS"' in outs)
    assert(b'"memo": "hello"' in outs)

    # pack account create data
    account = 'sysio'
    action = 'newaccount'

    unpacked_action_data = """{
        "creator": "sysio",
        "name": "bob",
        "owner": {
          "threshold": 1,
          "keys": [{
              "key": "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV",
              "weight": 1
            }
          ],
          "accounts": [],
          "waits": []
        },
        "active": {
          "threshold": 1,
          "keys": [{
              "key": "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV",
              "weight": 1
            }
          ],
          "accounts": [],
          "waits": []
        }
    }"""

    cmd = ['./programs/clio/clio', '-u','http://127.0.0.1:12345', '--abi-file', system_abi_file_arg, 'convert', 'pack_action_data', account, action, unpacked_action_data]
    # the packed data hex changed because of the change to sysio in unpacked_action_data above
    packed_action_data = '0000000000eab0c70000000000000e3d01000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf0100000001000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf01000000'
    outs, errs = processClioCommand(cmd)
    actual = outs.strip()
    assert(actual.decode('utf-8') == packed_action_data)

    # unpack account create data
    cmd = ['./programs/clio/clio', '-u','http://127.0.0.1:12345', '--abi-file', system_abi_file_arg, 'convert', 'unpack_action_data', account, action, packed_action_data]
    outs, errs = processClioCommand(cmd)
    assert(b'"creator": "sysio"' in outs)
    assert(b'"name": "bob"' in outs)

    # pack transaction
    unpacked_trx = """{
        "expiration": "2021-07-18T04:21:14",
        "ref_block_num": 494,
        "ref_block_prefix": 2118878731,
        "max_net_usage_words": 0,
        "max_cpu_usage_ms": 0,
        "delay_sec": 0,
        "context_free_actions": [],
        "actions": [{
            "account": "sysio",
            "name": "newaccount",
            "authorization": [{
                "actor": "sysio",
                "permission": "active"
                }
            ],
            "data": {
                "creator": "sysio",
                "name": "bob",
                "owner": {
                "threshold": 1,
                "keys": [{
                    "key": "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV",
                    "weight": 1
                    }
                ],
                "accounts": [],
                "waits": []
                },
                "active": {
                "threshold": 1,
                "keys": [{
                    "key": "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV",
                    "weight": 1
                    }
                ],
                "accounts": [],
                "waits": []
                }
            },
            "hex_data": "0000000000ea30550000000000000e3d01000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf0100000001000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf01000000"
            },
            {
            "account": "sysio.token",
            "name": "transfer",
            "authorization": [{
                "actor": "aaa",
                "permission": "active"
                }
            ],
            "data": {
                "from": "aaa",
                "to": "bbb",
                "quantity": "10.0000 SYS",
                "memo": "hello"
            },
            "hex_data": "0000000000008c31000000000000ce39a08601000000000004535953000000000568656c6c6f"
            }
        ],
        "signatures": [
            "SIG_K1_K3LfbB7ZV2DNBu67iSn3yUMseTdiwoT49gAcwSZVT1QTvGXVHjkcvKqhentCW4FJngZJ1H9gBRSWgo9UPiWEXWHyKpXNCZ"
        ],
        "context_free_data": []
    }"""

    expected_output = b'3aacf360ee010b864b7e00000000020000000000eab0c700409e9a2264b89a010000000000eab0c700000000a8ed3232660000000000eab0c70000000000000e3d01000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf0100000001000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf0100000000a6823403eab0c7000000572d3ccdcd010000000000008c3100000000a8ed3232260000000000008c31000000000000ce39a08601000000000004535953000000000568656c6c6f00'
    cmd = ['./programs/clio/clio', '-u','http://127.0.0.1:12345', '--abi-file', system_abi_file_arg, token_abi_file_arg, 'convert', 'pack_transaction', '--pack-action-data', unpacked_trx]
    outs, errs = processClioCommand(cmd)
    assert(expected_output in outs)

    # unpack transaction
    packed_trx = """{
        "signatures": [
            "SIG_K1_K3LfbB7ZV2DNBu67iSn3yUMseTdiwoT49gAcwSZVT1QTvGXVHjkcvKqhentCW4FJngZJ1H9gBRSWgo9UPiWEXWHyKpXNCZ"
        ],
        "compression": "none",
        "packed_context_free_data": "",
        "packed_trx": "3aacf360ee010b864b7e00000000020000000000eab0c700409e9a2264b89a010000000000eab0c700000000a8ed3232660000000000eab0c70000000000000e3d01000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf0100000001000000010002c0ded2bc1f1305fb0faac5e6c03ee3a1924234985427b6167ca569d13df435cf0100000000a6823403eab0c7000000572d3ccdcd010000000000008c3100000000a8ed3232260000000000008c31000000000000ce39a08601000000000004535953000000000568656c6c6f00"
    }"""
    cmd = ['./programs/clio/clio', '-u','http://127.0.0.1:12345', '--abi-file', system_abi_file_arg, token_abi_file_arg, 'convert', 'unpack_transaction', '--unpack-action-data', packed_trx]
    outs, errs = processClioCommand(cmd)
    assert(b'"creator": "sysio"' in outs)
    assert(b'"name": "bob"' in outs)

    assert(b'"from": "aaa"' in outs)
    assert(b'"to": "bbb"' in outs)
    assert(b'"quantity": "10.0000 SYS"' in outs)
    assert(b'"memo": "hello"' in outs)

def abi_file_with_nodeop_test():
    # push action token transfer with option `--abi-file`
    global testSuccessful
    try:
        biosDir = os.path.abspath(os.getcwd() + "/libraries/testing/contracts/sysio.bios")
        contractDir = os.path.abspath(os.getcwd() + "/contracts/sysio.token")
        # make a malicious abi file by switching 'from' and 'to' in sysio.token.abi
        token_abi_path = os.path.abspath(os.getcwd() + '/contracts/sysio.token/sysio.token.abi')
        token_abi_file_arg = 'sysio.token' + ':' + token_abi_path
        malicious_token_abi_path = os.path.abspath(os.getcwd() + '/contracts/sysio.token/malicious.sysio.token.abi')
        shutil.copyfile(token_abi_path, malicious_token_abi_path)
        replaces = [["from", "malicious"], ["to", "from"], ["malicious", "to"]]
        for replace in replaces:
            with open(malicious_token_abi_path, 'r+') as f:
                abi = f.read()
                abi = re.sub(replace[0], replace[1], abi)
                f.seek(0)
                f.write(abi)
                f.truncate()

        tries = 30
        while not Utils.arePortsAvailable(set(range(8888, 8889))):
            Utils.Print("ERROR: Another process is listening on nodeop test port 8888. wait...")
            if tries == 0:
                assert False
            tries -= 1
            time.sleep(2)
        nodeId = 'bios'
        data_dir = Utils.getNodeDataDir(nodeId)
        os.makedirs(data_dir, exist_ok=True)
        walletMgr = WalletMgr(True)
        walletMgr.launch()
        cmd = "./programs/nodeop/nodeop -e -p sysio --plugin sysio::trace_api_plugin --trace-no-abis --plugin sysio::producer_plugin --plugin sysio::producer_api_plugin --plugin sysio::chain_api_plugin --plugin sysio::chain_plugin --plugin sysio::http_plugin --access-control-allow-origin=* --http-validate-host=false --max-transaction-time=-1 --resource-monitor-not-shutdown-on-threshold-exceeded " + "--data-dir " + data_dir + " --config-dir " + data_dir
        node = Node('localhost', 8888, nodeId, data_dir=Path(data_dir), config_dir=Path(data_dir), cmd=shlex.split(cmd), launch_time=datetime.datetime.now().strftime('%Y_%m_%d_%H_%M_%S'), walletMgr=walletMgr)
        time.sleep(5)
        node.waitForBlock(1)
        accountNames = ["sysio", "sysio.token", "alice", "bob"]
        accounts = []
        for name in accountNames:
            account = Account(name)
            account.ownerPrivateKey = account.activePrivateKey = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
            account.ownerPublicKey = account.activePublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
            accounts.append(account)
        walletMgr.create('sysio', [accounts[0]])
        node.createAccount(accounts[1], accounts[0], stakedDeposit=0)
        node.publishContract(accounts[0], biosDir, 'sysio.bios.wasm', 'sysio.bios.abi')
        node.publishContract(accounts[1], contractDir, 'sysio.token.wasm', 'sysio.token.abi')
        node.setPriv(accounts[1], accounts[0], waitForTransBlock=True)
        account = 'sysio.token'
        action = 'create'
        data = '{"issuer":"sysio.token","maximum_supply":"100000.0000 SYS","can_freeze":"0","can_recall":"0","can_whitelist":"0"}'
        permission = '--permission sysio.token@active'
        node.pushMessage(account, action, data, permission)
        action = 'issue'
        data = '{"from":"sysio.token","to":"sysio.token","quantity":"100000.0000 SYS","memo":"issue"}'
        node.pushMessage(account, action, data, permission)
        node.createAccount(accounts[2], accounts[0], stakedDeposit=0)
        node.createAccount(accounts[3], accounts[0], stakedDeposit=0)

        node.transferFunds(accounts[1], accounts[2], '100.0000 SYS')

        node.processClioCmd('set abi sysio.token ' + malicious_token_abi_path, 'set malicious sysio.token abi', returnType=ReturnType.raw)

        cmdArr = node.transferFundsCmdArr(accounts[2], accounts[3], '25.0000 SYS', 'm', None, False, False, 90, False)
        cmdArr.insert(6, '--print-request')
        cmdArr.insert(7, '--abi-file')
        cmdArr.insert(8, token_abi_file_arg)
        Utils.runCmdArrReturnStr(cmdArr)
        balance = node.getCurrencyBalance('sysio.token', 'alice')
        assert balance == '75.0000 SYS\n'
        testSuccessful=True
    except Exception as e:
        testSuccessful=False
        Utils.Print(e.args)
    finally:
        if testSuccessful:
            Utils.Print("Test succeeded.")
        else:
            Utils.Print("Test failed.")
        if node:
            if not node.killed:
                node.kill(signal.SIGKILL)
        if testSuccessful:
            Utils.Print("Cleanup nodeop data.")
            shutil.rmtree(Utils.DataPath)

        if malicious_token_abi_path:
            if os.path.exists(malicious_token_abi_path):
                os.remove(malicious_token_abi_path)

        walletMgr.testFailed = not testSuccessful

nodeop_help_test()

clio_help_test(['--help'])
clio_help_test(['system', '--help'])
clio_help_test(['version', '--help'])
clio_help_test(['wallet', '--help'])

cli11_bugfix_test()

cli11_optional_option_arg_test()
clio_sign_test()

clio_abi_file_test()
abi_file_with_nodeop_test()

errorCode = 0 if testSuccessful else 1
exit(errorCode)
