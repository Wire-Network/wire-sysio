import os
import json

import click
from nacl.signing import SigningKey

# noinspection SpellCheckingInspection
_B58_ALPHABET = b"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def _b58encode(b: bytes) -> str:
    # Count leading zeros
    n_zeros = len(b) - len(b.lstrip(b"\x00"))
    # Convert to integer
    n = int.from_bytes(b, byteorder="big")
    # Encode base58
    enc = bytearray()
    while n > 0:
        n, r = divmod(n, 58)
        enc.append(_B58_ALPHABET[r])
    # Add leading zeros
    enc.extend(_B58_ALPHABET[0] for _ in range(n_zeros))
    enc.reverse()
    return enc.decode("ascii")


@click.command()
@click.argument('key_name')
def main(key_name):
    """
    KEY_NAME is the string used to reference the key in the `signature_provider`
    """
    priv_bytes = os.urandom(32)
    sk = SigningKey(priv_bytes)
    vk = sk.verify_key
    pub_bytes = bytes(vk)  # 32 bytes

    # 2) Solana address is the base58-encoded public key
    address = _b58encode(pub_bytes)

    # 3) Sign a payload
    payload = "simple-text-payload"

    signature_bytes = sk.sign(payload.encode())
    signature = "0x" + signature_bytes.signature.hex()

    key_data = {
        "key_name": key_name,
        "chain_type": "solana",
        "chain_key_type": "solana",
        "private_key": "0x" + priv_bytes.hex(),
        "public_key": "0x" + pub_bytes.hex(),
        "address": address,
        "payload": payload,
        "signature": signature
    }

    print(json.dumps(key_data))


if __name__ == "__main__":
    main()
