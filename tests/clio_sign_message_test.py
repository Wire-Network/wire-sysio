#!/usr/bin/env python3

"""Offline test for ``clio sign em_message`` and ``clio sign ed_message``, which sign text the way an external
wallet signs a message, and for ``clio sign`` itself still signing a transaction next to its subcommands.

Exercises, with no running node:

  * ``sign ed_message`` with a Solana key, as ``PVT_ED_`` and as the bare base58 secret: the signature is ed25519
    over the raw UTF-8 bytes (a wallet's signMessage). It equals a known answer from Python ``cryptography``, for the
    key given as an option and at the prompt; Ed25519 is deterministic, so the vector pins the whole signature.
  * ``sign em_message`` with an Ethereum key, as ``PVT_EM_`` and as the raw ``0x`` hex MetaMask exports: the
    signature is EIP-191 personal_sign over the whole message, byte-identical to ethers 5.8 ``Wallet.signMessage``
    (RFC 6979).
  * The message is signed as given: a newline and a non-ASCII character are kept, and the EIP-191 length is the
    UTF-8 byte count.
  * stdout carries only the JSON, also when the key is typed at the prompt.
  * Error paths: each command refuses every key type but its own (K1, and the other wallet's key), and a missing
    message. A key that does not parse is reported as "Invalid private key", and none of its text reaches stdout or
    stderr, by default or with ``-v``.
  * The commands sign with the key they are given, so they never start the wallet daemon (kiod), also without
    ``--no-auto-kiod``.
  * ``clio sign <transaction>`` without a subcommand still signs the transaction, and refuses to run without one.

Vectors (regenerate only deliberately, from the reference implementations):
    message "Wire sign_message test\\nline two: café"
    ED key: the ed25519 seed 0x01..0x20
    EM key: the secret 0x46 * 32 (the EIP-155 example key)
"""

import hashlib
import json
import os
import pathlib
import subprocess
import tempfile

from TestHarness import Utils

Print = Utils.Print

MESSAGE = "Wire sign_message test\nline two: café"

ED_MESSAGE_COMMAND = "ed_message"
EM_MESSAGE_COMMAND = "em_message"

ED_PRIVATE_PREFIX = "PVT_ED_"
ED_SECRET = "2Ana1pUpv2ZbMVkwF5FXapYeBEjdxDatLn7nvJkhgTSdZd8hbDHTd21as7EAsg7ypityqfsw2pMQKJcVDVcAEsd"
ED_PRIVATE = ED_PRIVATE_PREFIX + ED_SECRET
ED_PUBLIC = "PUB_ED_9C6hybhQ6Aycep9jaUnP6uL9ZYvDjUp1aSkFWPUFJtpj"
ED_SIGNATURE = ("SIG_ED_ivgGc8ismkQ6a83ZZQbDcigJvHZd8vpuDcjDsQUAGXeB7zs1FiHjzNpEqToDkoQsPbZPzVmmnGTzdNgiSZ9xpcbLVv4yX9"
                "gV3kVhEeWzQcpKM8PUX7HcSNTP7ZhqecSSdZb")

EM_SECRET = "0x4646464646464646464646464646464646464646464646464646464646464646"
EM_PRIVATE = "PVT_EM_" + EM_SECRET
EM_PUBLIC = ("PUB_EM_044bc2a31265153f07e70e0bab08724e6b85e217f8cd628ceb62974247bb493382"
             "ce28cab79ad7119ee1ad3ebcdb98a16805211530ecc6cfefa1b88e6dff99232a")
# ethers 5.8 Wallet(EM_SECRET).signMessage(MESSAGE): r || s || v, which is fc's SIG_EM_ layout.
EM_SIGNATURE = ("SIG_EM_0x048958eaa74c92c14a5cf59ea38ab47269bbb3ceb51553f0a12d9ea9303200c5"
                "4c00b59f91c91bfd8c9fff68b2ac27708ce525dff7bb1e7757e972805a46424b1b")

K1_PRIVATE = "PVT_K1_Xx4wsMynSZ39WiwEn8wzRL9trcKK33ZzwaZKJmymYx5UTjKvv"

# The reference keys with their last characters replaced by ones their encoding cannot hold ('0' is not base58).
BAD_ED_SECRET = ED_SECRET[:-1] + "0"
BAD_EM_SECRET = EM_SECRET[:-2] + "zz"
INVALID_KEY = "Invalid private key"
# Long enough that a match can only be the secret, short enough to catch a truncated echo.
SECRET_CHUNK = 12
VERBOSE = "-v"
NO_AUTO_KIOD = "--no-auto-kiod"
WALLET_URL_ENV = "WALLET_URL"

# A transaction for `clio sign`: any chain id works offline, and the key signs whatever it is given.
CHAIN_ID = "a" * 64
UNSIGNED_TRANSACTION = {
    "expiration": "2026-01-01T00:00:00", "ref_block_num": 0, "ref_block_prefix": 0,
    "max_net_usage_words": 0, "max_cpu_usage_ms": 0, "delay_sec": 0,
    "context_free_actions": [], "actions": [], "transaction_extensions": [], "signatures": [],
}

testSuccessful = False


def run_clio(*args, stdin=None, auto_kiod=False, env=None):
    """Run clio offline with ``args`` and return the ``subprocess.CompletedProcess``. Unless ``auto_kiod``, clio is
    told not to start kiod."""
    cmd = [Utils.SysClientPath, *([] if auto_kiod else [NO_AUTO_KIOD]), *args]
    if Utils.Debug:
        Print("cmd: %s" % " ".join(a if not a.startswith("PVT_") else "PVT_(hidden)" for a in cmd))
    return subprocess.run(cmd, capture_output=True, text=True, input=stdin, env=env)


def sign_message(command, private_key, message=MESSAGE, via_prompt=False):
    """Run ``sign <command>`` and return its JSON output. ``via_prompt`` feeds the key on stdin.

    The whole of stdout must be the JSON: the prompt goes to stderr."""
    args = ["sign", command, "--message", message]
    res = run_clio(*args, stdin=private_key + "\n") if via_prompt else run_clio(*args, "--private-key", private_key)
    assert res.returncode == 0, "clio failed: stderr: %s\nstdout: %s" % (res.stderr, res.stdout)
    return json.loads(res.stdout)


def assert_refused(*args):
    """Assert that clio exits non-zero for ``args`` and prints no signature."""
    res = run_clio(*args)
    assert res.returncode != 0, "expected a refusal: %s\nstdout: %s" % (args, res.stdout)
    assert "SIG_" not in res.stdout, res.stdout


def secret_chunks(secret):
    """Every SECRET_CHUNK-character run of ``secret``: an output that shows any part of it contains one."""
    return [secret[i:i + SECRET_CHUNK] for i in range(len(secret) - SECRET_CHUNK + 1)]


def test_ed_signs_the_raw_bytes():
    """A Solana key signs the UTF-8 bytes themselves, as signMessage does, at the option and at the prompt, given as
    PVT_ED_ or as the bare base58 secret."""
    expected = {"pub_key": ED_PUBLIC, "sig": ED_SIGNATURE, "message": MESSAGE}
    for key in (ED_PRIVATE, ED_SECRET):
        for via_prompt in (False, True):
            out = sign_message(ED_MESSAGE_COMMAND, key, via_prompt=via_prompt)
            assert out == expected, out


def test_em_is_personal_sign():
    """An Ethereum key, as PVT_EM_ or raw hex, gives the personal_sign signature ethers gives, also at the prompt."""
    expected = {"pub_key": EM_PUBLIC, "sig": EM_SIGNATURE, "message": MESSAGE}
    for key in (EM_PRIVATE, EM_SECRET):
        for via_prompt in (False, True):
            out = sign_message(EM_MESSAGE_COMMAND, key, via_prompt=via_prompt)
            assert out == expected, out


def test_signature_depends_on_every_byte():
    """Changing one character, or dropping the non-ASCII one, changes both signatures."""
    for command, key, reference in ((ED_MESSAGE_COMMAND, ED_PRIVATE, ED_SIGNATURE),
                                    (EM_MESSAGE_COMMAND, EM_PRIVATE, EM_SIGNATURE)):
        for other in (MESSAGE.replace("two", "Two"), MESSAGE.replace("é", "e")):
            assert sign_message(command, key, message=other)["sig"] != reference, other


def test_error_handling():
    """Each command takes only its own key type, and needs a message."""
    for key in (K1_PRIVATE, ED_PRIVATE, ED_SECRET):
        assert_refused("sign", EM_MESSAGE_COMMAND, "--message", MESSAGE, "--private-key", key)
    for key in (K1_PRIVATE, EM_PRIVATE, EM_SECRET):
        assert_refused("sign", ED_MESSAGE_COMMAND, "--message", MESSAGE, "--private-key", key)
    for command, key in ((EM_MESSAGE_COMMAND, EM_PRIVATE), (ED_MESSAGE_COMMAND, ED_PRIVATE)):
        assert_refused("sign", command, "--private-key", key)


def test_errors_never_show_the_key():
    """A key that does not parse fails as "Invalid private key" and no part of it is printed, also with -v."""
    for command, key, secret in ((ED_MESSAGE_COMMAND, ED_PRIVATE_PREFIX + BAD_ED_SECRET, BAD_ED_SECRET),
                                 (ED_MESSAGE_COMMAND, BAD_ED_SECRET, BAD_ED_SECRET),
                                 (EM_MESSAGE_COMMAND, BAD_EM_SECRET, BAD_EM_SECRET)):
        for verbosity in ([], [VERBOSE]):
            res = run_clio(*verbosity, "sign", command, "--message", MESSAGE, "--private-key", key)
            output = res.stdout + res.stderr
            assert res.returncode != 0 and INVALID_KEY in output, output
            leaked = [chunk for chunk in secret_chunks(secret) if chunk in output]
            assert not leaked, "%s %s printed part of the key: %s" % (command, verbosity, leaked[0])


def test_no_wallet_daemon():
    """Without --no-auto-kiod, the four commands still never start kiod: WALLET_URL points kiod at an empty
    directory, which must stay empty. (A kiod started there by a regression is stopped before the test fails.)"""
    digest = hashlib.sha256(MESSAGE.encode("utf-8")).hexdigest()
    with tempfile.TemporaryDirectory() as data_dir:
        env = {**os.environ, WALLET_URL_ENV: "unix://" + str(pathlib.Path(data_dir) / "kiod.sock")}
        try:
            res = run_clio("sign", "em_digest", digest, "--private-key", EM_PRIVATE, auto_kiod=True, env=env)
            assert res.returncode == 0, res.stderr
            signature = res.stdout.split("Signature: ", 1)[1].strip()
            for args in (("sign", "em_digest_recover", signature, digest),
                         ("sign", EM_MESSAGE_COMMAND, "--message", MESSAGE, "--private-key", EM_PRIVATE),
                         ("sign", ED_MESSAGE_COMMAND, "--message", MESSAGE, "--private-key", ED_PRIVATE)):
                res = run_clio(*args, auto_kiod=True, env=env)
                assert res.returncode == 0, "%s: %s" % (args[1], res.stderr)
            started = os.listdir(data_dir)
            assert not started, "a sign subcommand started kiod in %s: %s" % (data_dir, started)
        finally:
            subprocess.run(["pkill", "-f", data_dir], check=False)


def test_sign_transaction_still_works():
    """``clio sign <transaction>`` signs the transaction as before, and refuses to run without one."""
    res = run_clio("sign", json.dumps(UNSIGNED_TRANSACTION), "-k", K1_PRIVATE, "-c", CHAIN_ID)
    assert res.returncode == 0, "clio sign failed: stderr: %s\nstdout: %s" % (res.stderr, res.stdout)
    signed = json.loads(res.stdout)
    assert len(signed["signatures"]) == 1 and signed["signatures"][0].startswith("SIG_K1_"), signed

    res = run_clio("sign", "-k", K1_PRIVATE, "-c", CHAIN_ID)
    assert res.returncode != 0, res.stdout
    assert "transaction is required" in res.stderr, res.stderr


try:
    test_ed_signs_the_raw_bytes()
    test_em_is_personal_sign()
    test_signature_depends_on_every_byte()
    test_error_handling()
    test_errors_never_show_the_key()
    test_no_wallet_daemon()
    test_sign_transaction_still_works()
    testSuccessful = True
except Exception as e:
    Print(e)
    Utils.errorExit("exception during processing")

exit(0 if testSuccessful else 1)
