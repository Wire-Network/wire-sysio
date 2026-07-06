#!/usr/bin/env python3

"""Offline test for clio's EM (Ethereum-style secp256k1) and SOL (ed25519) key support.

Exercises, with no running node:

  * ``clio create key --em``  -> PVT_EM_/PUB_EM_ keypair, round-tripped through
    ``clio convert em_private_key`` / ``em_sign`` / ``em_recover``.
  * ``clio create key --sol`` -> PVT_ED_/PUB_ED_ (Solana ed25519) keypair.
  * A frozen known-answer vector proving libfc's EM path is byte-for-byte
    identical to the Ethereum ecosystem reference (eth_account / MetaMask
    ``personal_sign``). This is the cross-implementation anchor: a regression in
    Wire's EIP-191 envelope, low-s normalization, or recovery-id handling fails
    here, hermetically, with no pip/apt/browser dependency.
  * Flag mutual-exclusion and input-validation error paths.

Known-answer vector provenance (regenerate only by a deliberate, reviewed act):

    Reference implementation : eth_account 0.13.7 (eth-keys/eth-utils stack),
                               byte-identical to MetaMask personal_sign for the
                               same key and message.
    Ethereum private key     : 0x46 * 32   (the well-known EIP-155 example key)
    Message / digest         : sha256(b"wire-sysio metamask external-signer KAT v1")
                               = 7358248b67726c147e6b100d188dec3b5be0bdc08735378b6146ec0207cf58c4
    Recipe                   : personal_sign over the 32 raw digest bytes
                               (EIP-191: keccak256("\\x19Ethereum Signed Message:\\n32" + digest)),
                               deterministic RFC-6979 secp256k1, low-s.

The expected PUB_EM_/SIG_EM_ below were produced by that reference and MUST NOT
be edited to match a code change -- a mismatch means clio/libfc diverged from
the Ethereum ecosystem, which is a bug in libfc, not in this vector.
"""

import os
import re
import stat
import subprocess
import tempfile

from TestHarness import Utils

Print = Utils.Print

# --- frozen known-answer vector (see module docstring for provenance) ---
KAT_ETH_SECRET = "0x4646464646464646464646464646464646464646464646464646464646464646"
KAT_PVT_EM     = "PVT_EM_0x4646464646464646464646464646464646464646464646464646464646464646"
KAT_DIGEST     = "7358248b67726c147e6b100d188dec3b5be0bdc08735378b6146ec0207cf58c4"
KAT_PUB_EM     = ("PUB_EM_044bc2a31265153f07e70e0bab08724e6b85e217f8cd628ceb629742"
                  "47bb493382ce28cab79ad7119ee1ad3ebcdb98a16805211530ecc6cfefa1b88e6dff99232a")
KAT_SIG_EM     = ("SIG_EM_0xea0847dc6f2c9a189b85f01e9a1d51931ac92ee381b67641384b523"
                  "14a803fd5064a69f67b5967f2cac3fa3b9cf3f4f0be9f08e282e0e71ed0ce13d202a892b71b")
# KAT_DIGEST with the last hex nibble flipped: a valid but different 32-byte
# digest. Recovering KAT_SIG_EM against this must yield a *different* key --
# proving recovery is digest-bound (the property the stale-signature pre-flight
# in the metamask harness relies on).
KAT_WRONG_DIGEST = KAT_DIGEST[:-1] + ("5" if KAT_DIGEST[-1] != "5" else "6")

# --- eth_to_k1 known-answer vector (same EIP-155 0x46*32 key as KAT_ETH_SECRET) ---
# External anchor: 0x46*32 is EIP-155's worked example and its sender address is
# published in the spec. PVT_K1_/PUB_K1_ are Wire's own encoding of that same
# secp256k1 secret (compressed point); PUB_K1_ is the compression of the very
# uncompressed point the eth_account-generated KAT_PUB_EM encodes, so it stays
# anchored to that external vector, not to the code under test.
KAT_ETH_ADDRESS = "0x9d8a62f656a8d1615c1294fd71e9cfb3e4855a4f"   # EIP-155 example sender
KAT_PVT_K1      = "PVT_K1_Xx4wsMynSZ39WiwEn8wzRL9trcKK33ZzwaZKJmymYx5UTjKvv"
KAT_PUB_K1      = "PUB_K1_5TrYnZP1RkDSUMzBY4GanCy6AP68kCMdkAb5EACkAwkdc8tm4t"

testSuccessful = False
PERMISSIVE_UMASK = stat.S_IWGRP | stat.S_IWOTH
SECRET_FILE_MODE = stat.S_IRUSR | stat.S_IWUSR


def run_with_umask(mask, callback):
    """Run ``callback`` while the process umask is temporarily set to ``mask``."""
    old_umask = os.umask(mask)
    try:
        return callback()
    finally:
        os.umask(old_umask)


def assert_owner_only_secret_file(path):
    """Assert that a generated secret file is readable and writable only by its owner."""
    mode = stat.S_IMODE(os.stat(path).st_mode)
    assert mode == SECRET_FILE_MODE, "expected %s to have mode 0o600, got 0o%03o" % (path, mode)


def clio(*args, expect_fail=False, raw=False):
    """Run the clio binary offline with `args`.

    Default: assert exit 0 and return a dict of the ``Label: value`` lines clio
    prints. `expect_fail=True`: assert a non-zero exit (CLI11 parse error or a
    thrown libfc exception) and return None. `raw=True`: return the
    ``subprocess.CompletedProcess`` for callers that assert on stderr / exit code
    directly (used for clio's soft-validation paths, which by long-standing
    convention print ``ERROR:`` and emit nothing but still exit 0).
    """
    cmd = [Utils.SysClientPath, *args]
    if Utils.Debug:
        Print("cmd: %s" % " ".join(cmd))
    res = subprocess.run(cmd, capture_output=True, text=True)
    if raw:
        return res
    if expect_fail:
        assert res.returncode != 0, "expected failure but clio succeeded: %s\n%s" % (cmd, res.stdout)
        return None
    assert res.returncode == 0, "clio failed: %s\nstderr: %s\nstdout: %s" % (cmd, res.stderr, res.stdout)
    return {k: v for k, v in re.findall(r'(\w[^:\n]*): ([^\n]+)', res.stdout)}


def test_create_key_em_roundtrip():
    """create key --em yields a PVT_EM_/PUB_EM_ pair that round-trips through convert."""
    r = clio("create", "key", "--em", "--to-console")
    priv, pub = r["Private key"], r["Public key"]
    assert priv.startswith("PVT_EM_"), priv
    assert pub.startswith("PUB_EM_"), pub

    # PVT_EM_ -> same PUB_EM_ via convert em_private_key
    assert clio("convert", "em_private_key", "--private-key", priv, "--to-console")["Public key"] == pub

    # sign an arbitrary digest with it, then recover -> must yield the same PUB_EM_
    sig = clio("convert", "em_sign", KAT_DIGEST, "--private-key", priv)["Signature"]
    assert sig.startswith("SIG_EM_"), sig
    assert clio("convert", "em_recover", sig, KAT_DIGEST)["Public key"] == pub

    # two fresh keys must differ (sanity that generation is not fixed)
    assert clio("create", "key", "--em", "--to-console")["Private key"] != priv


def test_create_key_sol():
    """create key --sol yields a Solana ed25519 PVT_ED_/PUB_ED_ pair."""
    r = clio("create", "key", "--sol", "--to-console")
    assert r["Private key"].startswith("PVT_ED_"), r["Private key"]
    assert r["Public key"].startswith("PUB_ED_"), r["Public key"]
    assert clio("create", "key", "--sol", "--to-console")["Private key"] != r["Private key"]


def test_secret_key_output_files_are_owner_only():
    """Secret-bearing ``--file`` outputs are owner-only even under a permissive umask."""
    with tempfile.TemporaryDirectory() as temp_dir:
        def create_secret_files():
            """Create representative clio secret-output files in ``temp_dir``."""
            create_path = os.path.join(temp_dir, "created-em-key.txt")
            clio("create", "key", "--em", "--file", create_path)
            assert_owner_only_secret_file(create_path)
            with open(create_path, encoding="utf-8") as f:
                body = f.read()
            assert "PVT_EM_" in body and "PUB_EM_" in body, body

            k1_convert_path = os.path.join(temp_dir, "converted-k1-key.txt")
            clio("convert", "k1_private_key", "--private-key", KAT_PVT_K1, "--file", k1_convert_path)
            assert_owner_only_secret_file(k1_convert_path)

            em_convert_path = os.path.join(temp_dir, "converted-em-key.txt")
            clio("convert", "em_private_key", "--private-key", KAT_ETH_SECRET, "--file", em_convert_path)
            assert_owner_only_secret_file(em_convert_path)

            eth_to_k1_path = os.path.join(temp_dir, "eth-to-k1-key.txt")
            clio("convert", "eth_to_k1_private", "--private-key", KAT_ETH_SECRET, "--file", eth_to_k1_path)
            assert_owner_only_secret_file(eth_to_k1_path)

        run_with_umask(PERMISSIVE_UMASK, create_secret_files)


def test_known_answer_vector():
    """libfc's EM path is byte-for-byte identical to eth_account / MetaMask.

    Both the raw-Ethereum-secret import path and the PVT_EM_ path must derive
    the reference public key; em_sign must reproduce the reference signature
    (RFC-6979 deterministic, so this is exact); em_recover must invert it.
    """
    # raw Ethereum secret import -> reference PUB_EM_
    assert clio("convert", "em_private_key", "--private-key", KAT_ETH_SECRET,
                "--to-console")["Public key"] == KAT_PUB_EM
    # PVT_EM_ import -> same
    assert clio("convert", "em_private_key", "--private-key", KAT_PVT_EM,
                "--to-console")["Public key"] == KAT_PUB_EM

    # signature is deterministic and must match the reference, from either key form
    assert clio("convert", "em_sign", KAT_DIGEST, "--private-key", KAT_ETH_SECRET)["Signature"] == KAT_SIG_EM
    assert clio("convert", "em_sign", KAT_DIGEST, "--private-key", KAT_PVT_EM)["Signature"] == KAT_SIG_EM
    # 0x-prefixed digest is accepted and equivalent
    assert clio("convert", "em_sign", "0x" + KAT_DIGEST, "--private-key", KAT_PVT_EM)["Signature"] == KAT_SIG_EM

    # recover the reference signature back to the reference public key
    assert clio("convert", "em_recover", KAT_SIG_EM, KAT_DIGEST)["Public key"] == KAT_PUB_EM


def test_recover_is_digest_bound():
    """Recovery is bound to the exact digest signed.

    Recovering the reference signature against a *different* (but well-formed)
    digest still succeeds -- ECDSA recovery yields some point for almost any
    digest -- but must yield a DIFFERENT public key, never the reference one.
    This is precisely what lets the metamask harness reject a stale or
    wrong-digest paste offline instead of as an opaque unsatisfied_authorization.
    """
    recovered = clio("convert", "em_recover", KAT_SIG_EM, KAT_WRONG_DIGEST)["Public key"]
    assert recovered.startswith("PUB_EM_"), recovered
    assert recovered != KAT_PUB_EM, "recovery is not digest-bound: wrong digest recovered the reference key"


def test_error_handling():
    """Mutually exclusive curve flags and malformed crypto inputs are hard errors.

    The missing-output-sink case is clio's long-standing soft-validation
    convention (shared by ``create key`` / ``convert k1_private_key``): print
    ``ERROR:`` and emit no key, exit 0. Assert that contract rather than an exit
    code so this test does not silently encode a behavior change to it.
    """
    # mutually exclusive curve flags -> CLI11 parse error (non-zero); all of
    # --k1/--r1/--em/--sol exclude each other
    clio("create", "key", "--em", "--sol", "--to-console", expect_fail=True)
    clio("create", "key", "--em", "--r1", "--to-console", expect_fail=True)
    clio("create", "key", "--k1", "--em", "--to-console", expect_fail=True)
    clio("create", "key", "--k1", "--r1", "--to-console", expect_fail=True)

    # neither --file nor --to-console: soft validation -> ERROR, no key, exit 0
    res = clio("create", "key", "--em", raw=True)
    assert "ERROR" in res.stderr, res.stderr
    assert "PVT_EM_" not in res.stdout, res.stdout

    # malformed crypto inputs throw in libfc -> non-zero
    clio("convert", "em_sign", "deadbeef", "--private-key", KAT_PVT_EM, expect_fail=True)  # 8 hex != 64
    clio("convert", "em_sign", "z" * 64, "--private-key", KAT_PVT_EM, expect_fail=True)    # 64 chars, not hex
    clio("convert", "em_recover", "SIG_EM_0xdeadbeef", KAT_DIGEST, expect_fail=True)       # malformed sig


def test_eth_to_k1():
    """Reuse an Ethereum keypair as a Wire K1 signing key (same secp256k1 secret).

    Anchored on the EIP-155 example address (external truth). The private and
    public conversion paths must agree, and the public path is fed the very
    uncompressed pubkey the eth_account-generated KAT_PUB_EM encodes -- tying
    K1 back to the cross-impl EM vector above.
    """
    # raw Ethereum secret -> PVT_K1_/PUB_K1_ + the EIP-155 address
    r = clio("convert", "eth_to_k1_private", "--private-key", KAT_ETH_SECRET, "--to-console")
    assert r["Private key"] == KAT_PVT_K1, r["Private key"]
    assert r["Public key"] == KAT_PUB_K1, r["Public key"]
    assert r["Ethereum address"].lower() == KAT_ETH_ADDRESS, r["Ethereum address"]

    # public path: the EIP-155 uncompressed pubkey (== KAT_PUB_EM minus its prefix)
    # -> the SAME PUB_K1_ + address. Proves the two paths agree and binds the K1
    # point to the eth_account EM vector.
    uncompressed = KAT_PUB_EM[len("PUB_EM_"):]
    rp = clio("convert", "eth_to_k1_public", uncompressed)
    assert rp["Public key"] == KAT_PUB_K1, rp["Public key"]
    assert rp["Ethereum address"].lower() == KAT_ETH_ADDRESS, rp["Ethereum address"]
    # the 0x-prefixed form is accepted identically
    assert clio("convert", "eth_to_k1_public", "0x" + uncompressed)["Public key"] == KAT_PUB_K1

    # the same secret imported as EM (existing path) must give the reference PUB_EM_:
    # i.e. the K1 and EM import paths agree on the underlying curve point.
    assert clio("convert", "em_private_key", "--private-key", KAT_ETH_SECRET,
                "--to-console")["Public key"] == KAT_PUB_EM

    # error paths for the public command's length/prefix validation (the private
    # command shares em_private_key's lenient raw-secret parse, so it is not retested here).
    clio("convert", "eth_to_k1_public", "04abcd", expect_fail=True)                 # too short
    clio("convert", "eth_to_k1_public", "05" + uncompressed[2:], expect_fail=True)  # 65-byte, wrong prefix


try:
    test_create_key_em_roundtrip()
    test_create_key_sol()
    test_secret_key_output_files_are_owner_only()
    test_known_answer_vector()
    test_recover_is_digest_bound()
    test_error_handling()
    test_eth_to_k1()
    testSuccessful = True
except Exception as e:
    Print(e)
    Utils.errorExit("exception during processing")

exit(0 if testSuccessful else 1)
