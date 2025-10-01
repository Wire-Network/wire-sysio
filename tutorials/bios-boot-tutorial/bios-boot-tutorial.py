#!/usr/bin/env python3

import argparse
import json
import numpy
import os
import random
import re
import subprocess
import sys
import time

args = None
logFile = None

unlockTimeout = 999999999
fastUnstakeSystem = './fast.refund/sysio.system/sysio.system.wasm'

systemAccounts = [
    'sysio.bpay',
    'sysio.msig',
    'sysio.names',
    'sysio.saving',
    'sysio.token',
    'sysio.vpay',
    'sysio.acct'
]

def jsonArg(a):
    return " '" + json.dumps(a) + "' "

def run(args):
    print('bios-boot-tutorial.py:', args)
    logFile.write(args + '\n')
    if subprocess.call(args, shell=True):
        print('bios-boot-tutorial.py: exiting because of error')
        sys.exit(1)

def retry(args):
    while True:
        print('bios-boot-tutorial.py: ', args)
        logFile.write(args + '\n')
        if subprocess.call(args, shell=True):
            print('*** Retry')
        else:
            break

def background(args):
    print('bios-boot-tutorial.py:', args)
    logFile.write(args + '\n')
    return subprocess.Popen(args, shell=True)

def getOutput(args):
    print('bios-boot-tutorial.py:', args)
    logFile.write(args + '\n')
    proc = subprocess.Popen(args, shell=True, stdout=subprocess.PIPE)
    return proc.communicate()[0].decode('utf-8')

def getJsonOutput(args):
    return json.loads(getOutput(args))

def sleep(t):
    print('sleep', t, '...')
    time.sleep(t)
    print('resume')

def startWallet():
    run('rm -rf ' + os.path.abspath(args.wallet_dir))
    run('mkdir -p ' + os.path.abspath(args.wallet_dir))
    background(args.kiod + ' --unlock-timeout %d --http-server-address 0.0.0.0:6666 --http-max-response-time-ms 99999 --wallet-dir %s' % (unlockTimeout, os.path.abspath(args.wallet_dir)))
    sleep(.4)
    run(args.clio + 'wallet create -f wallet.pwd')

def importKeys():
    run(args.clio + 'wallet import --private-key ' + args.private_key)
    keys = {}
    for a in accounts:
        key = a['pvt']
        if not key in keys:
            if len(keys) >= args.max_user_keys:
                break
            keys[key] = True
            run(args.clio + 'wallet import --private-key ' + key)
    for i in range(firstProducer, firstProducer + numProducers):
        a = accounts[i]
        key = a['pvt']
        if not key in keys:
            keys[key] = True
            run(args.clio + 'wallet import --private-key ' + key)

def startNode(nodeIndex, account):
    dir = args.nodes_dir + ('%02d-' % nodeIndex) + account['name'] + '/'
    run('rm -rf ' + dir)
    run('mkdir -p ' + dir)
    otherOpts = ''.join(list(map(lambda i: '    --p2p-peer-address localhost:' + str(9000 + i), range(nodeIndex))))
    if not nodeIndex: otherOpts += (
        '    --plugin sysio::trace_api_plugin --trace-no-abis'
    )
    cmd = (
        args.nodeop +
        '    --max-irreversible-block-age -1'
        # max-transaction-time must be less than block time
        # (which is defined in .../chain/include/sysio/chain/config.hpp
        # as block_interval_ms = 500)
        '    --max-transaction-time=200'
        '    --contracts-console'
        '    --genesis-json ' + os.path.abspath(args.genesis) +
        '    --blocks-dir ' + os.path.abspath(dir) + '/blocks'
        '    --config-dir ' + os.path.abspath(dir) +
        '    --data-dir ' + os.path.abspath(dir) +
        '    --chain-state-db-size-mb 1024'
        '    --http-server-address 0.0.0.0:' + str(args.http_port + nodeIndex) +
        '    --p2p-listen-endpoint 0.0.0.0:' + str(9000 + nodeIndex) +
        '    --max-clients ' + str(maxClients) +
        '    --p2p-max-nodes-per-host ' + str(maxClients) +
        '    --enable-stale-production'
        '    --http-validate-host=false'
        '    --access-control-allow-origin=*'
        '    --access-control-allow-headers="Origin, X-Requested-With, Content-Type, Accept"'
        '    --producer-name ' + account['name'] +
        '    --signature-provider ' + account['pub'] + '=KEY:' + account['pvt'] +
        # '    --s-chain-contract settle.wns'
        # '    --s-chain-actions batchw'
        # '    --s-chain-actions initcontract'
        '    --plugin sysio::http_plugin'
        '    --plugin sysio::chain_api_plugin'
        '    --plugin sysio::chain_plugin'
        '    --plugin sysio::producer_api_plugin'
        '    --plugin sysio::producer_plugin' +
        otherOpts)
    with open(dir + 'stderr', mode='w') as f:
        f.write(cmd + '\n\n')
    background(cmd + '    2>>' + dir + 'stderr')

def startProducers(b, e):
    for i in range(b, e):
        startNode(i - b + 1, accounts[i])

def createSystemAccounts():
    for a in systemAccounts:
        run(args.clio + 'create account sysio ' + a + ' ' + args.public_key)

def intToCurrency(i):
    return '%d.%04d %s' % (i // 10000, i % 10000, args.symbol)

def allocateFunds(b, e):
    dist = numpy.random.pareto(1.161, e - b).tolist() # 1.161 = 80/20 rule
    dist.sort()
    dist.reverse()
    factor = 1_000_000_000 / sum(dist)
    total = 0
    for i in range(b, e):
        funds = round(factor * dist[i - b] * 10000)
        if i >= firstProducer and i < firstProducer + numProducers:
            funds = max(funds, round(args.min_producer_funds * 10000))
        total += funds
        accounts[i]['funds'] = funds
    return total

def createAccounts(b, e):
    for i in range(b, e):
        a = accounts[i]
        print('#' * 80)
        print('# %d/%d %s' % (i, e, a['name']))
        print('#' * 80)
        retry(args.clio + 'system newaccount sysio %s %s ' %
            (a['name'], a['pub']))

def regProducers(b, e):
    for i in range(b, e):
        a = accounts[i]
        retry(args.clio + 'system regproducer ' + a['name'] + ' ' + a['pub'] + ' https://' + a['name'] + '.com' + '/' + a['pub'])

def listProducers():
    run(args.clio + 'system listproducers')

def claimRewards():
    table = getJsonOutput(args.clio + 'get table sysio sysio producers -l 100')
    times = []
    for row in table['rows']:
        if row['unpaid_blocks'] and not row['last_claim_time']:
            times.append(getJsonOutput(args.clio + 'system claimrewards -j ' + row['owner'])['processed']['elapsed'])
    print('Elapsed time for claimrewards:', times)

def updateAuth(account, permission, parent, controller):
    run(args.clio + 'push action sysio updateauth' + jsonArg({
        'account': account,
        'permission': permission,
        'parent': parent,
        'auth': {
            'threshold': 1, 'keys': [], 'waits': [],
            'accounts': [{
                'weight': 1,
                'permission': {'actor': controller, 'permission': 'active'}
            }]
        }
    }) + '-p ' + account + '@' + permission)

def resign(account, controller):
    updateAuth(account, 'owner', '', controller)
    updateAuth(account, 'active', 'owner', controller)
    sleep(1)
    run(args.clio + 'get account ' + account)

def randomTransfer(b, e):
    for j in range(20):
        src = accounts[random.randint(b, e - 1)]['name']
        dest = src
        while dest == src:
            dest = accounts[random.randint(b, e - 1)]['name']
        run(args.clio + 'transfer -f ' + src + ' ' + dest + ' "0.0001 ' + args.symbol + '"' + ' || true')

def msigProposeReplaceSystem(proposer, proposalName):
    requestedPermissions = []
    for i in range(firstProducer, firstProducer + numProducers):
        requestedPermissions.append({'actor': accounts[i]['name'], 'permission': 'active'})
    trxPermissions = [{'actor': 'sysio', 'permission': 'active'}]
    with open(fastUnstakeSystem, mode='rb') as f:
        setcode = {'account': 'sysio', 'vmtype': 0, 'vmversion': 0, 'code': f.read().hex()}
    run(args.clio + 'multisig propose ' + proposalName + jsonArg(requestedPermissions) +
        jsonArg(trxPermissions) + 'sysio setcode' + jsonArg(setcode) + ' -p ' + proposer)

def msigApproveReplaceSystem(proposer, proposalName):
    for i in range(firstProducer, firstProducer + numProducers):
        run(args.clio + 'multisig approve ' + proposer + ' ' + proposalName +
            jsonArg({'actor': accounts[i]['name'], 'permission': 'active'}) +
            '-p ' + accounts[i]['name'])

def msigExecReplaceSystem(proposer, proposalName):
    retry(args.clio + 'multisig exec ' + proposer + ' ' + proposalName + ' -p ' + proposer)

def produceNewAccounts():
    with open('newusers', 'w') as f:
        for i in range(120_000, 200_000):
            x = getOutput(args.clio + 'create key --to-console')
            r = re.match('Private key: *([^ \n]*)\nPublic key: *([^ \n]*)', x, re.DOTALL | re.MULTILINE)
            name = 'user'
            for j in range(7, -1, -1):
                name += chr(ord('a') + ((i >> (j * 4)) & 15))
            print(i, name)
            f.write('        {"name":"%s", "pvt":"%s", "pub":"%s"},\n' % (name, r[1], r[2]))

def stepStartWallet():
    startWallet()
    importKeys()
def stepStartBoot():
    startNode(0, {'name': 'sysio', 'pvt': args.private_key, 'pub': args.public_key})
    sleep(10.0)
def stepInstallSystemContracts():
    run(args.clio + 'set contract sysio.token ' + args.contracts_dir + '/sysio.token/')
    run(args.clio + 'set contract sysio.msig ' + args.contracts_dir + '/sysio.msig/')
def stepCreateTokens():
    run(args.clio + 'push action sysio setpriv' + jsonArg(['sysio.token', 1]) + '-p sysio@active')
    run(args.clio + 'push action sysio.token create \'["sysio", "10000000000.0000 %s"]\' -p sysio.token' % (args.symbol))
    totalAllocation = allocateFunds(0, len(accounts))
    run(args.clio + 'push action sysio.token issue \'["sysio", "%s", "memo"]\' -p sysio' % intToCurrency(totalAllocation))
    sleep(1)
def stepSetSystemContract():
    # All of the protocol upgrade features introduced in v1.8 first require a special protocol 
    # feature (codename PREACTIVATE_FEATURE) to be activated and for an updated version of the system 
    # contract that makes use of the functionality introduced by that feature to be deployed. 

    # activate PREACTIVATE_FEATURE before installing sysio.boot
    retry('curl -X POST http://0.0.0.0:%d' % args.http_port +
        '/v1/producer/schedule_protocol_feature_activations ' +
        '-d \'{"protocol_features_to_activate": ["0ec7e080177b2c02b278d5088611686b49d739925a92d9bfcacd7fc6b74053bd"]}\'')
    sleep(3)

    # install sysio.boot which supports the native actions and activate
    # action that allows activating desired protocol features prior to 
    # deploying a system contract with more features such as sysio.bios
    # or sysio.system
    retry(args.clio + 'set contract sysio ' + args.bios_contract_dir + '/sysio.bios/')
    sleep(3)

    # activate remaining features
    # RESERVED_FIRST_PROTOCOL_FEATURE
    retry(args.clio + 'push action sysio activate \'["30df9517cb8808f198723c030f597bb64645d52315f2e8f1ea77424ea33d896a"]\' -p sysio@active')
    sleep(1)

    # install sysio.system latest version
    retry(args.clio + 'set contract sysio ' + args.contracts_dir + '/sysio.system/')
    # setpriv is only available after sysio.system is installed
    run(args.clio + 'push action sysio setpriv' + jsonArg(['sysio.msig', 1]) + '-p sysio@active')
    sleep(3)

def stepInitSystemContract():
    run(args.clio + 'push action sysio init' + jsonArg(['0', '4,' + args.symbol]) + '-p sysio@active')
    sleep(1)
def stepCreateAccounts():
    createAccounts(0, len(accounts))
def stepRegProducers():
    regProducers(firstProducer, firstProducer + numProducers)
    sleep(1)
    listProducers()
def stepStartProducers():
    startProducers(firstProducer, firstProducer + numProducers)
    sleep(args.producer_sync_delay)
def stepResign():
    resign('sysio', 'sysio.prods')
    for a in systemAccounts:
        resign(a, 'sysio')
def stepTransfer():
    while True:
        randomTransfer(0, args.num_senders)
def stepLog():
    run('tail -n 60 ' + args.nodes_dir + '00-sysio/stderr')

# Command Line Arguments

parser = argparse.ArgumentParser()

commands = [
    ('w', 'wallet',             stepStartWallet,            True,    "Start kiod, create wallet, fill with keys"),
    ('b', 'boot',               stepStartBoot,              True,    "Start boot node"),
    ('s', 'sys',                createSystemAccounts,       True,    "Create system accounts (sysio.*)"),
    ('c', 'contracts',          stepInstallSystemContracts, True,    "Install system contracts (token, msig)"),
    ('S', 'sys-contract',       stepSetSystemContract,      True,    "Set system contract"),
    ('t', 'tokens',             stepCreateTokens,           True,    "Create tokens"),
    ('I', 'init-sys-contract',  stepInitSystemContract,     True,    "Initialiaze system contract"),
    ('T', 'stake',              stepCreateAccounts,         True,    "Create accounts"),
    ('p', 'reg-prod',           stepRegProducers,           True,    "Register producers"),
    ('P', 'start-prod',         stepStartProducers,         True,    "Start producers"),
    ('R', 'claim',              claimRewards,               True,    "Claim rewards"),
    ('q', 'resign',             stepResign,                 True,    "Resign sysio"),
    ('X', 'xfer',               stepTransfer,               False,   "Random transfer tokens (infinite loop)"),
    ('l', 'log',                stepLog,                    True,    "Show tail of node's log"),
]

parser.add_argument('--public-key', metavar='', help="SYSIO Public Key", default='SYS8Znrtgwt8TfpmbVpTKvA2oB8Nqey625CLN8bCN3TEbgx86Dsvr', dest="public_key")
parser.add_argument('--private-Key', metavar='', help="SYSIO Private Key", default='5K463ynhZoCDDa4RDcr63cUwWLTnKqmdcoTKTHBjqoKfv4u5V7p', dest="private_key")
parser.add_argument('--clio', metavar='', help="Clio command", default='../../build/programs/clio/clio --wallet-url http://0.0.0.0:6666 ')
parser.add_argument('--nodeop', metavar='', help="Path to nodeop binary", default='../../build/programs/nodeop/nodeop')
parser.add_argument('--kiod', metavar='', help="Path to kiod binary", default='../../build/programs/kiod/kiod')
parser.add_argument('--contracts-dir', metavar='', help="Path to latest contracts directory", default='../../build/contracts/')
parser.add_argument('--bios-contract-dir', metavar='', help="Path to sysio.bios contract directory", default='../../build/contracts/')
parser.add_argument('--nodes-dir', metavar='', help="Path to nodes directory", default='./nodes/')
parser.add_argument('--genesis', metavar='', help="Path to genesis.json", default="./genesis.json")
parser.add_argument('--wallet-dir', metavar='', help="Path to wallet directory", default='./wallet/')
parser.add_argument('--log-path', metavar='', help="Path to log file", default='./output.log')
parser.add_argument('--symbol', metavar='', help="The sysio.system symbol", default='SYS')
parser.add_argument('--user-limit', metavar='', help="Max number of users. (0 = no limit)", type=int, default=3000)
parser.add_argument('--max-user-keys', metavar='', help="Maximum user keys to import into wallet", type=int, default=10)
parser.add_argument('--producer-limit', metavar='', help="Maximum number of producers. (0 = no limit)", type=int, default=0)
parser.add_argument('--min-producer-funds', metavar='', help="Minimum producer funds", type=float, default=1000.0000)
parser.add_argument('--num-senders', metavar='', help="Number of users to transfer funds randomly", type=int, default=10)
parser.add_argument('--producer-sync-delay', metavar='', help="Time (s) to sleep to allow producers to sync", type=int, default=80)
parser.add_argument('-a', '--all', action='store_true', help="Do everything marked with (*)")
parser.add_argument('-H', '--http-port', type=int, default=8000, metavar='', help='HTTP port for clio')

for (flag, command, function, inAll, help) in commands:
    prefix = ''
    if inAll: prefix += '*'
    if prefix: help = '(' + prefix + ') ' + help
    if flag:
        parser.add_argument('-' + flag, '--' + command, action='store_true', help=help, dest=command)
    else:
        parser.add_argument('--' + command, action='store_true', help=help, dest=command)
        
args = parser.parse_args()

# Leave a space in front of --url in case the user types clio alone
args.clio += ' --url http://127.0.0.1:%d ' % args.http_port

logFile = open(args.log_path, 'a')

logFile.write('\n\n' + '*' * 80 + '\n\n\n')

with open('accounts.json') as f:
    a = json.load(f)
    if args.user_limit:
        del a['users'][args.user_limit:]
    if args.producer_limit:
        del a['producers'][args.producer_limit:]
    firstProducer = len(a['users'])
    numProducers = len(a['producers'])
    accounts = a['users'] + a['producers']

maxClients = numProducers + 10

haveCommand = False
for (flag, command, function, inAll, help) in commands:
    if getattr(args, command) or inAll and args.all:
        if function:
            haveCommand = True
            function()
if not haveCommand:
    print('bios-boot-tutorial.py: Tell me what to do. -a does almost everything. -h shows options.')
