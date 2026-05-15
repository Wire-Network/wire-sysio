"""Verify a Metamask-style EIP-191 signature against an expected signer.

Given:
    --digest        Hex of the message that Metamask was asked to sign. This is
                    treated EXACTLY the way personal_sign treats a 0x-prefixed
                    hex string: the bytes are decoded from hex, then the EIP-191
                    prefix and length are prepended, then keccak256, then ECDSA
                    recovery on the supplied signature.
    --signature     Either an 0x-prefixed 130-hex-char signature (r||s||v) or
                    Wire's SIG_EM_... form. Both are accepted.
    --expect-addr   Optional. 0x-prefixed Ethereum address (40 hex chars) the
                    recovered pubkey must hash to.
    --expect-pub    Optional. Either an 0x-prefixed uncompressed hex pubkey or
                    Wire's PUB_EM_... form. The recovered pubkey must match.

If neither --expect-addr nor --expect-pub is provided, the recovered values are
printed and the script exits 0.

Exit codes:
    0  recovery succeeded and (if specified) matched all expectations
    1  signature recovery failed
    2  recovery succeeded but expectations did not match
    3  CLI / input error
"""

from __future__ import annotations

import argparse
import sys

from em_sig_to_wire import (
    eth_address_from_pub,
    metamask_pub_to_wire,
    wire_pub_to_metamask,
    wire_sig_to_metamask,
)


def _normalize_sig(s: str) -> str:
    return wire_sig_to_metamask(s) if s.startswith("SIG_EM_") else s


def _normalize_pub(s: str) -> str:
    return wire_pub_to_metamask(s) if s.startswith("PUB_EM_") else s


def _recover_pub(digest_hex: str, sig_hex: str) -> str:
    """Run eth_account-style EIP-191 personal_sign recovery.

    Returns the recovered uncompressed pubkey as 0x-prefixed hex (130 chars
    including the SEC1 0x04 prefix).
    """
    from eth_account.messages import _hash_eip191_message, encode_defunct
    from eth_keys import keys

    sig_bytes = bytes.fromhex(sig_hex[2:] if sig_hex.startswith("0x") else sig_hex)
    if len(sig_bytes) != 65:
        raise ValueError(f"signature must be 65 bytes, got {len(sig_bytes)}")

    digest_hex_clean = digest_hex[2:] if digest_hex.startswith("0x") else digest_hex
    message_bytes = bytes.fromhex(digest_hex_clean)

    # Apply the EIP-191 envelope (0x19 + "E" + "thereum Signed Message:\n" + len + payload)
    # and keccak256 it. This matches what Wire's signature_shim::recover() does internally
    # for a sha256 trx digest, and what Metamask's personal_sign does in the browser.
    h = _hash_eip191_message(encode_defunct(primitive=message_bytes))

    r = int.from_bytes(sig_bytes[0:32], "big")
    s = int.from_bytes(sig_bytes[32:64], "big")
    v = sig_bytes[64]
    if v in (27, 28):
        recovery_id = v - 27
    elif v in (0, 1):
        recovery_id = v
    else:
        raise ValueError(f"unexpected v byte: {v}")

    sig = keys.Signature(vrs=(recovery_id, r, s))
    pub = sig.recover_public_key_from_msg_hash(h)
    return "0x04" + pub.to_bytes().hex()


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--digest", required=True,
                   help="Hex-encoded payload Metamask signed (0x-prefixed or bare hex)")
    p.add_argument("--signature", required=True,
                   help="0x-hex signature (130 chars) or SIG_EM_... form")
    p.add_argument("--expect-addr", default=None,
                   help="Optional. 0x... Ethereum address the recovered pubkey must hash to")
    p.add_argument("--expect-pub", default=None,
                   help="Optional. 0x-uncompressed hex or PUB_EM_... form")
    p.add_argument("--quiet", action="store_true", help="Suppress info output")
    args = p.parse_args(argv)

    try:
        sig_hex = _normalize_sig(args.signature)
        recovered_pub = _recover_pub(args.digest, sig_hex)
    except Exception as exc:  # noqa: BLE001
        print(f"recovery failed: {exc}", file=sys.stderr)
        return 1

    recovered_addr = eth_address_from_pub(recovered_pub)
    recovered_wire_pub = metamask_pub_to_wire(recovered_pub)

    if not args.quiet:
        print(f"recovered address:    {recovered_addr}")
        print(f"recovered pub  (hex): {recovered_pub}")
        print(f"recovered pub (wire): {recovered_wire_pub}")

    failures: list[str] = []
    if args.expect_addr:
        if args.expect_addr.lower() != recovered_addr.lower():
            failures.append(f"address mismatch: got {recovered_addr}, expected {args.expect_addr}")
    if args.expect_pub:
        try:
            want = _normalize_pub(args.expect_pub).lower()
        except Exception as exc:  # noqa: BLE001
            print(f"could not parse --expect-pub: {exc}", file=sys.stderr)
            return 3
        if recovered_pub.lower() != want:
            failures.append(f"pubkey mismatch: got {recovered_pub}, expected {want}")

    if failures:
        for line in failures:
            print(f"FAIL: {line}", file=sys.stderr)
        return 2

    if not args.quiet and (args.expect_addr or args.expect_pub):
        print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
