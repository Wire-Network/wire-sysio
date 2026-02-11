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

    Generates Solana-compatible keypair with base58 encoding:
    - private_key: 64-byte secret key (32-byte seed + 32-byte public key) in base58
    - public_key: 32-byte public key in base58
    - address: same as public_key (Solana addresses are public keys)
    - signature: 64-byte ED25519 signature in base58
    """
    priv_seed = os.urandom(32)
    sk = SigningKey(priv_seed)
    vk = sk.verify_key
    pub_bytes = bytes(vk)  # 32 bytes

    # Solana secret key is 64 bytes: seed (32) + public key (32)
    secret_key_64 = priv_seed + pub_bytes

    # Base58 encode all keys
    private_key_b58 = _b58encode(secret_key_64)
    public_key_b58 = _b58encode(pub_bytes)

    # Solana address is the base58-encoded public key
    address = public_key_b58

    # Sign a payload (Solana signs raw message, not hash)
    payload = "simple-text-payload"
    signature_bytes = sk.sign(payload.encode())
    signature_b58 = _b58encode(signature_bytes.signature)

    key_data = {
        "key_name": key_name,
        "chain_type": "solana",
        "chain_key_type": "solana",
        "private_key": private_key_b58,
        "public_key": public_key_b58,
        "address": address,
        "payload": payload,
        "signature": signature_b58
    }

    print(json.dumps(key_data))


if __name__ == "__main__":
    main()
