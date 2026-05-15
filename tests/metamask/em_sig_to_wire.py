"""Bidirectional converter between Metamask-format secp256k1 signatures /
uncompressed public keys and Wire's SIG_EM_/PUB_EM_ string forms.

The Wire EM format is hex, NOT base58. From libfc:
  - em::public_key_shim::to_string(no_prefix=true)  -> 130 hex chars (no 0x)
    so PUB_EM_ form is: "PUB_EM_" + 130 hex chars (65-byte uncompressed, SEC1 0x04 prefix in first 2 hex chars)
  - em::signature_shim::to_string()                 -> "0x" + 130 hex chars
    so SIG_EM_ form is: "SIG_EM_" + "0x" + 130 hex chars (65 bytes: r||s||v)

Metamask format:
  - Signature: "0x" + 130 hex chars  (r(32) || s(32) || v(1))
  - Public key (uncompressed): 64 raw bytes (X || Y), or 65 with 0x04 SEC1 prefix
  - Address: "0x" + last 40 hex chars of keccak256(uncompressed_pub[1:])

Run as a CLI:
    python em_sig_to_wire.py sig-to-wire   0x<130-hex>
    python em_sig_to_wire.py wire-to-sig   SIG_EM_0x<130-hex>
    python em_sig_to_wire.py pub-to-wire   0x<128-hex or 130-hex>
    python em_sig_to_wire.py wire-to-pub   PUB_EM_<130-hex>
    python em_sig_to_wire.py addr-from-pub 0x<128-hex>
"""

from __future__ import annotations

import sys


def _hex_to_bytes(s: str) -> bytes:
    s = s[2:] if s.startswith(("0x", "0X")) else s
    return bytes.fromhex(s)


def _bytes_to_0x_hex(b: bytes) -> str:
    return "0x" + b.hex()


# --- signature conversions -------------------------------------------------

def metamask_sig_to_wire(sig_hex: str) -> str:
    """Convert a 65-byte Metamask hex signature (r||s||v) to Wire SIG_EM_ form.

    Wire form is `SIG_EM_0x<130hex>`. Normalizes v to 27/28.
    """
    sig = _hex_to_bytes(sig_hex)
    if len(sig) != 65:
        raise ValueError(f"signature must be 65 bytes, got {len(sig)}")
    v = sig[64]
    if v in (0, 1):
        sig = sig[:64] + bytes([v + 27])
    elif v not in (27, 28):
        raise ValueError(f"unexpected v byte: {v}; expected 27, 28, 0, or 1")
    return "SIG_EM_" + _bytes_to_0x_hex(sig)


def wire_sig_to_metamask(sig_wire: str) -> str:
    """Convert SIG_EM_0x... back to a 65-byte 0x-prefixed hex signature."""
    if not sig_wire.startswith("SIG_EM_"):
        raise ValueError(f"expected SIG_EM_... prefix, got {sig_wire[:10]}")
    tail = sig_wire[len("SIG_EM_"):]
    # tail starts with "0x" because em::signature_shim::to_string always adds it.
    if not tail.startswith(("0x", "0X")):
        # Accept the variant without "0x" too, just in case.
        tail = "0x" + tail
    raw = _hex_to_bytes(tail)
    if len(raw) != 65:
        raise ValueError(f"decoded signature length {len(raw)} != 65")
    return _bytes_to_0x_hex(raw)


# --- public key conversions ------------------------------------------------

def metamask_pub_to_wire(pub_hex: str) -> str:
    """Convert an uncompressed Ethereum-style hex pub (128 or 130 chars) to PUB_EM_..."""
    raw = _hex_to_bytes(pub_hex)
    if len(raw) == 64:
        raw = b"\x04" + raw
    elif len(raw) == 65:
        if raw[0] != 0x04:
            raise ValueError(f"uncompressed pubkey prefix must be 0x04, got 0x{raw[0]:02x}")
    else:
        raise ValueError(f"uncompressed pubkey must be 64 or 65 bytes, got {len(raw)}")
    # Wire form: PUB_EM_<130 hex chars, no 0x in middle>
    return "PUB_EM_" + raw.hex()


def wire_pub_to_metamask(pub_wire: str) -> str:
    """Convert PUB_EM_... to a 0x-prefixed uncompressed-pub hex string (130 chars including 04)."""
    if not pub_wire.startswith("PUB_EM_"):
        raise ValueError(f"expected PUB_EM_... prefix, got {pub_wire[:10]}")
    tail = pub_wire[len("PUB_EM_"):]
    # PUB_EM_ form has no "0x" in the middle, but accept it gracefully.
    if tail.startswith(("0x", "0X")):
        tail = tail[2:]
    raw = bytes.fromhex(tail)
    if len(raw) == 64:
        raw = b"\x04" + raw
    elif len(raw) != 65:
        raise ValueError(f"unexpected pubkey hex length {len(tail)}; want 128 or 130")
    if raw[0] != 0x04:
        raise ValueError("uncompressed pubkey prefix must be 0x04")
    return _bytes_to_0x_hex(raw)


def eth_address_from_pub(pub_hex: str) -> str:
    """Derive the 0x... Ethereum address from an uncompressed pubkey hex (128 or 130 chars)."""
    raw = _hex_to_bytes(pub_hex)
    if len(raw) == 65:
        if raw[0] != 0x04:
            raise ValueError("uncompressed pubkey prefix must be 0x04")
        xy = raw[1:]
    elif len(raw) == 64:
        xy = raw
    else:
        raise ValueError(f"uncompressed pubkey must be 64 or 65 bytes, got {len(raw)}")
    digest = _keccak256(xy)
    return _bytes_to_0x_hex(digest[-20:])


def _keccak256(data: bytes) -> bytes:
    try:
        from eth_utils import keccak  # type: ignore
        return keccak(data)
    except ImportError:
        pass
    try:
        from Crypto.Hash import keccak  # type: ignore
        h = keccak.new(digest_bits=256)
        h.update(data)
        return h.digest()
    except ImportError:
        pass
    try:
        import sha3  # type: ignore  (pysha3 / safe-pysha3)
        h = sha3.keccak_256()
        h.update(data)
        return h.digest()
    except ImportError as exc:
        raise RuntimeError(
            "no keccak256 backend found; pip install eth-utils, pycryptodome, or pysha3"
        ) from exc


# --- CLI -------------------------------------------------------------------

def _main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    args = argv[2:]
    try:
        if cmd == "sig-to-wire" and len(args) == 1:
            print(metamask_sig_to_wire(args[0]))
        elif cmd == "wire-to-sig" and len(args) == 1:
            print(wire_sig_to_metamask(args[0]))
        elif cmd == "pub-to-wire" and len(args) == 1:
            print(metamask_pub_to_wire(args[0]))
        elif cmd == "wire-to-pub" and len(args) == 1:
            print(wire_pub_to_metamask(args[0]))
        elif cmd == "addr-from-pub" and len(args) == 1:
            print(eth_address_from_pub(args[0]))
        else:
            print(__doc__)
            return 2
    except Exception as exc:  # noqa: BLE001
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
