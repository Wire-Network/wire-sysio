"""Headless substitute for Metamask's personal_sign.

Mimics MetaMask's `personal_sign` on an arbitrary 0x-hex payload using
`eth_account`. The signature produced by this script is byte-identical to what
Metamask would emit if a user signed the same digest in the browser, because
both go through the EIP-191 envelope:

    keccak256("\\x19Ethereum Signed Message:\\n" + len(payload) + payload)

then deterministic RFC-6979 secp256k1 with low-s normalization.

Run:
    python metamask_sim.py keygen
        -> emits {private_key, eth_address, wire_pub_em} as JSON

    python metamask_sim.py sign --private-key 0x<hex> --digest 0x<hex>
        -> emits {signature_hex, signature_wire, eth_address, wire_pub_em} as JSON

    python metamask_sim.py sign --private-key 0x<hex> --digest 0x<hex> --raw
        -> emits the SIG_EM_... value alone on stdout (script-friendly)
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any

from em_sig_to_wire import metamask_pub_to_wire, metamask_sig_to_wire


def _load_key(priv_hex: str):
    from eth_keys import keys
    raw = bytes.fromhex(priv_hex[2:] if priv_hex.startswith("0x") else priv_hex)
    if len(raw) != 32:
        raise ValueError(f"private key must be 32 bytes, got {len(raw)}")
    return keys.PrivateKey(raw)


def _generate_key() -> str:
    return "0x" + os.urandom(32).hex()


def _key_info(priv) -> dict[str, str]:
    pub_hex = "0x04" + priv.public_key.to_bytes().hex()
    from eth_utils import keccak
    addr = "0x" + keccak(priv.public_key.to_bytes())[-20:].hex()
    return {
        "eth_address": addr,
        "uncompressed_pub_hex": pub_hex,
        "wire_pub_em": metamask_pub_to_wire(pub_hex),
    }


def _sign_digest(priv, digest_hex: str) -> dict[str, Any]:
    """Sign `digest_hex` (raw payload, will be EIP-191 wrapped + keccak256-ed).

    `digest_hex` is what Metamask receives as the message argument to
    personal_sign when called as `personal_sign('0x<hex>', address)`.
    """
    from eth_account.messages import encode_defunct
    from eth_account import Account

    msg_bytes = bytes.fromhex(digest_hex[2:] if digest_hex.startswith("0x") else digest_hex)
    msg = encode_defunct(primitive=msg_bytes)
    signed = Account.sign_message(msg, priv.to_hex())
    sig_hex = "0x" + signed.signature.hex()
    return {
        "signature_hex": sig_hex,
        "signature_wire": metamask_sig_to_wire(sig_hex),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_keygen = sub.add_parser("keygen", help="Generate a fresh EM key")
    p_keygen.add_argument("--private-key", default=None,
                          help="Use this private key instead of generating one")

    p_sign = sub.add_parser("sign", help="Sign a 0x-hex digest as Metamask would")
    p_sign.add_argument("--private-key", required=True, help="0x-prefixed 32-byte hex")
    p_sign.add_argument("--digest", required=True, help="0x-prefixed hex message to sign")
    p_sign.add_argument("--raw", action="store_true",
                        help="Print only the SIG_EM_... line (no JSON)")

    args = parser.parse_args(argv)

    try:
        if args.cmd == "keygen":
            priv_hex = args.private_key or _generate_key()
            priv = _load_key(priv_hex)
            info = {"private_key": priv_hex, **_key_info(priv)}
            print(json.dumps(info, indent=2))
            return 0

        # The only other subcommand; argparse's required subparser guarantees
        # args.cmd is "keygen" or "sign", so this needs no further dispatch.
        priv = _load_key(args.private_key)
        sig = _sign_digest(priv, args.digest)
        if args.raw:
            print(sig["signature_wire"])
        else:
            print(json.dumps({**sig, **_key_info(priv)}, indent=2))
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
