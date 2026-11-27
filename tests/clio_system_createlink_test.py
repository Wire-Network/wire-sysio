#!/usr/bin/env python3

"""Test for ``clio system createlink``, which links an Ethereum or Solana key to an account: it signs the
sysio.authex::createlink ownership proof with that key the way the key's wallet would, then sends createlink.

On a one-node chain whose bootstrap deploys sysio.authex and a privileged sysio.dclaim:

  * The proof clio puts in the action (built with ``-s -d -u`` and a fixed ``--nonce``, so nothing is sent) matches
    known answers from implementations independent of libfc. For a Solana (``PVT_ED_``) key the whole signature is
    pinned (Python ``cryptography``; Ed25519 is deterministic) and verifies over the wallet payload, the lowercase
    hex of the printable-mapped sha256 of the message. For an Ethereum key, as ``PVT_EM_`` and as the raw ``0x`` hex
    MetaMask exports, ``clio sign em_digest_recover`` recovers the key from the signature over the reference
    keccak256 digest of the message. (libfc's EIP-191 signer is pinned to eth_account by clio_em_key_test.)
  * The proof binds the account and the nonce, and the nonce defaults to the head block time.
  * Sent, an ED link (key given as an option) and an EM link (raw hex typed at the prompt) each add the links row.
  * Refused before anything is sent: a K1 key and an invalid account name. Refused by the contract: a second link
    on a chain the account already linked, and a nonce older than 10 minutes.

Whether the contract accepts every proof shape is covered by contracts/tests/sysio.authex_tests.cpp, which signs
through the same host helper (sysio::opp::depot::authex_link) as this command.

Vectors (regenerate only deliberately, from the reference implementation):
    account "alice", nonce 1767225600000 (2026-01-01T00:00:00Z)
    ED key: the ed25519 seed 0x01..0x20
    EM key: the secret 0x46 * 32 (the EIP-155 example key)
"""

import datetime
import hashlib
import json
import re
import subprocess

from TestHarness import Cluster, Node, TestHelper, Utils, WalletMgr, createAccountKeys

Print = Utils.Print
errorExit = Utils.errorExit

ACCOUNT = "alice"
OTHER_ACCOUNT = "bob"
NONCE = 1767225600000

AUTHEX = "sysio.authex"
CREATELINK = "createlink"
LINKS_TABLE = "links"
CHAIN_KIND_EVM = "CHAIN_KIND_EVM"
CHAIN_KIND_SVM = "CHAIN_KIND_SVM"

ED_PRIVATE = "PVT_ED_2Ana1pUpv2ZbMVkwF5FXapYeBEjdxDatLn7nvJkhgTSdZd8hbDHTd21as7EAsg7ypityqfsw2pMQKJcVDVcAEsd"
ED_PUBLIC = "PUB_ED_9C6hybhQ6Aycep9jaUnP6uL9ZYvDjUp1aSkFWPUFJtpj"
ED_MESSAGE = "PUB_ED_9C6hybhQ6Aycep9jaUnP6uL9ZYvDjUp1aSkFWPUFJtpj|alice|3|1767225600000|createlink auth"
ED_SIGNATURE = ("SIG_ED_ivgGc8ismkQ6a83ZZQbDcigJvHZd8vpuDcjDsQUAGXeC1SPSWycUW2d4LFSE6GNhy8tvUM4n2z3azdGFUivSVLmfxUZ9"
                "iBWbgwPCG9TU1JpJDWitWUg6DwH8L5fReUAWJ2v")

EM_SECRET = "0x4646464646464646464646464646464646464646464646464646464646464646"
EM_PRIVATE = "PVT_EM_" + EM_SECRET
EM_DIGEST = "53d6ccc910ded561976f11ec3863e5e1eeb8ed93f84a33d0381734e893b336c8"

K1_PRIVATE = "PVT_K1_Xx4wsMynSZ39WiwEn8wzRL9trcKK33ZzwaZKJmymYx5UTjKvv"

BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
ED_PUBLIC_KEY_BYTES = 32
ED_PRINTABLE_MIN = 33
ED_PRINTABLE_RANGE = 94
MS_PER_SECOND = 1000

args = TestHelper.parse_args({"-v", "--dump-error-details", "--leave-running", "--keep-logs", "--unshared"})
Utils.Debug = args.v

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)
testSuccessful = False


def run_clio(node, *cmd, stdin=None):
    """Run clio against ``node`` and its wallet; return the ``subprocess.CompletedProcess``."""
    full = [Utils.SysClientPath, *node.sysClientArgs().split(), *cmd]
    if Utils.Debug:
        Print("cmd: %s" % " ".join(a if not a.startswith("PVT_") else "PVT_(hidden)" for a in full))
    return subprocess.run(full, capture_output=True, text=True, input=stdin)


def createlink(node, account, private_key, *options, via_prompt=False):
    """Run ``system createlink`` for ``account``; ``via_prompt`` types the key at the prompt."""
    cmd = ["system", CREATELINK, account, *options]
    if via_prompt:
        return run_clio(node, *cmd, stdin=private_key + "\n")
    return run_clio(node, *cmd, "--private-key", private_key)


def proof(node, private_key, account=ACCOUNT, nonce=NONCE):
    """The createlink action clio builds, unsigned and unsent (``-s -d -u -j``). ``nonce=None`` takes the default."""
    options = ["-s", "-d", "-u", "-j"] + ([] if nonce is None else ["--nonce", str(nonce)])
    res = createlink(node, account, private_key, *options)
    assert res.returncode == 0, "createlink -d failed: stderr: %s\nstdout: %s" % (res.stderr, res.stdout)
    actions = json.loads(res.stdout)["actions"]
    assert len(actions) == 1, actions
    action = actions[0]
    assert action["account"] == AUTHEX and action["name"] == CREATELINK, action
    assert action["authorization"] == [{"actor": account, "permission": "active"}], action
    data = action["data"]
    data["nonce"] = int(data["nonce"])   # the ABI serializer prints a uint64 as a string
    return data


def links(node):
    """Every sysio.authex links row, unwrapped from the KV {"value": row} wrapper."""
    rows = node.getTableRows(AUTHEX, AUTHEX, LINKS_TABLE)
    assert rows is not None, "could not read %s %s" % (AUTHEX, LINKS_TABLE)
    return [row["value"] if "value" in row else row for row in rows]


def links_of(node, account):
    """The chain kinds and public keys ``account`` has linked."""
    return sorted((row["chain_kind"], row["pub_key"]) for row in links(node) if row["username"] == account)


def assert_sent(node, res):
    """Assert createlink was accepted and wait until its transaction is in a block."""
    assert res.returncode == 0, "createlink failed: stderr: %s\nstdout: %s" % (res.stderr, res.stdout)
    trans_id = Node.getTransId(json.loads(res.stdout))
    assert node.waitForTransactionInBlock(trans_id), "createlink %s never reached a block" % trans_id


def assert_refused(res, reason, by_contract=False):
    """Assert createlink failed with ``reason`` in its output. A refusal by clio itself sends nothing; one by the
    contract comes back as the failed transaction's trace."""
    output = res.stdout + res.stderr
    assert res.returncode != 0, "expected a refusal: %s" % output
    assert reason in output, "expected %r in: %s" % (reason, output)
    assert ("transaction_id" in res.stdout) == by_contract, res.stdout


def head_block_time_ms(node):
    """The node's head block time, in milliseconds since the Unix epoch (the chain prints it in UTC)."""
    head = datetime.datetime.fromisoformat(node.getInfo()["head_block_time"]).replace(tzinfo=datetime.timezone.utc)
    return int(head.timestamp() * MS_PER_SECOND)


def new_ed_key():
    """A fresh Solana key from clio: (private, public)."""
    out = subprocess.run([Utils.SysClientPath, "--no-auto-kiod", "create", "key", "--sol", "--to-console"],
                         capture_output=True, text=True, check=True).stdout
    return re.search(r"Private key: (\S+)", out).group(1), re.search(r"Public key: (\S+)", out).group(1)


def base58_decode(text):
    """Decode a base58 (bitcoin alphabet) string to bytes."""
    number = 0
    for char in text:
        number = number * 58 + BASE58_ALPHABET.index(char)
    body = number.to_bytes((number.bit_length() + 7) // 8, "big")
    return b"\0" * (len(text) - len(text.lstrip("1"))) + body


def ed_wallet_payload(message):
    """The bytes a Solana wallet signs for ``message``: the hex text of its printable-mapped sha256."""
    mapped = bytes(b % ED_PRINTABLE_RANGE + ED_PRINTABLE_MIN for b in hashlib.sha256(message.encode()).digest())
    return mapped.hex().encode()


def test_ed_proof_matches_reference(node):
    """A Solana key signs the reference proof, and the signature verifies independently of libfc."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    data = proof(node, ED_PRIVATE)
    assert data == {"chain_kind": CHAIN_KIND_SVM, "account": ACCOUNT, "sig": ED_SIGNATURE, "pub_key": ED_PUBLIC,
                    "nonce": NONCE}, data

    raw = base58_decode(ED_SIGNATURE[len("SIG_ED_"):])
    public_key, signature = raw[:ED_PUBLIC_KEY_BYTES], raw[ED_PUBLIC_KEY_BYTES:]
    assert public_key == base58_decode(ED_PUBLIC[len("PUB_ED_"):])
    Ed25519PublicKey.from_public_bytes(public_key).verify(signature, ed_wallet_payload(ED_MESSAGE))


def test_em_proof_matches_reference(node):
    """An Ethereum key, as PVT_EM_ or raw hex, signs the reference message, recoverable over its keccak digest."""
    datas = [proof(node, EM_PRIVATE), proof(node, EM_SECRET)]
    assert datas[0] == datas[1], datas
    data = datas[0]
    assert data["chain_kind"] == CHAIN_KIND_EVM and data["nonce"] == NONCE, data
    assert data["sig"].startswith("SIG_EM_"), data

    res = run_clio(node, "sign", "em_digest_recover", data["sig"], EM_DIGEST)
    assert res.returncode == 0, res.stderr
    assert re.search(r"Public key: (\S+)", res.stdout).group(1) == data["pub_key"], res.stdout


def test_proof_binds_account_and_nonce(node):
    """A different account or nonce gives a different signature, and the nonce defaults to the head block time."""
    base = proof(node, ED_PRIVATE)
    for other in (proof(node, ED_PRIVATE, account=OTHER_ACCOUNT), proof(node, ED_PRIVATE, nonce=NONCE + 1)):
        assert other["sig"] != base["sig"], other

    before_ms = head_block_time_ms(node)
    nonce = proof(node, ED_PRIVATE, nonce=None)["nonce"]
    after_ms = head_block_time_ms(node)
    assert before_ms <= nonce <= after_ms, (before_ms, nonce, after_ms)


def test_links_are_sent(node):
    """An ED link with the key as an option and an EM link with the key at the prompt both land."""
    assert_sent(node, createlink(node, ACCOUNT, ED_PRIVATE, "-j"))
    assert links_of(node, ACCOUNT) == [(CHAIN_KIND_SVM, ED_PUBLIC)], links_of(node, ACCOUNT)

    assert_sent(node, createlink(node, OTHER_ACCOUNT, EM_SECRET, "-j", via_prompt=True))
    kinds = [kind for kind, _ in links_of(node, OTHER_ACCOUNT)]
    assert kinds == [CHAIN_KIND_EVM], links_of(node, OTHER_ACCOUNT)


def test_refusals(node):
    """clio refuses a K1 key and an invalid account before sending; the contract refuses a second link on one chain
    and a stale nonce."""
    assert_refused(createlink(node, ACCOUNT, K1_PRIVATE, "-j"), "createlink links only")
    assert_refused(createlink(node, "Not_A_Name", ED_PRIVATE, "-j"), "Not_A_Name")

    other_ed_private, _ = new_ed_key()
    assert_refused(createlink(node, ACCOUNT, other_ed_private, "-j"), "Account already has a link for this chain",
                   by_contract=True)
    assert_refused(createlink(node, OTHER_ACCOUNT, other_ed_private, "--nonce", str(NONCE), "-j"), "Invalid nonce",
                   by_contract=True)
    assert links_of(node, ACCOUNT) == [(CHAIN_KIND_SVM, ED_PUBLIC)], links_of(node, ACCOUNT)
    assert [kind for kind, _ in links_of(node, OTHER_ACCOUNT)] == [CHAIN_KIND_EVM], links_of(node, OTHER_ACCOUNT)


try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)
    if cluster.launch(pnodes=1, totalNodes=1, totalProducers=1, activateIF=True) is False:
        errorExit("Failed to stand up cluster.")
    node = cluster.getNode(0)

    accounts = createAccountKeys(2)
    accounts[0].name, accounts[1].name = ACCOUNT, OTHER_ACCOUNT
    wallet = walletMgr.create("ignition", [cluster.sysioAccount])
    for account in accounts:
        walletMgr.importKey(account, wallet, ignoreDupKeyWarning=True)
        # Not waitForTransBlock: it asks for --retry-num-blocks, which this node does not enable.
        trans = node.createAccount(account, cluster.sysioAccount, stakedDeposit=0)
        if trans is None or not node.waitForTransactionInBlock(Node.getTransId(trans)):
            errorExit("Failed to create account %s" % account.name)

    test_ed_proof_matches_reference(node)
    test_em_proof_matches_reference(node)
    test_proof_binds_account_and_nonce(node)
    test_links_are_sent(node)
    test_refusals(node)
    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exit(0 if testSuccessful else 1)
