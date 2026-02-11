import os
import json

import click
from eth_keys import keys
from eth_utils import keccak
from eth_account import Account
from eth_account.messages import encode_defunct


@click.command()
@click.argument('key_name')
def main(key_name):
    """
    KEY_NAME is the string used to reference the key in the `signature_provider`
    """
    priv_bytes = os.urandom(32)
    priv = keys.PrivateKey(priv_bytes)
    pub = priv.public_key

    # 2) Ethereum address = last 20 bytes of keccak(pub_key_uncompressed[1:])
    addr_bytes = keccak(pub.to_bytes()[1:])[-20:]
    addr = "0x" + addr_bytes.hex()

    payload = "simple-text-payload"

    # Sign the message
    # account = Account.from_key(priv_bytes)
    message = encode_defunct(text=payload)
    signed_message = Account.sign_message(message, priv.to_hex())  # priv.sign_msg(message)

    # sig = priv.sign_msg(payload.encode())

    key_data = {
        "key_name": key_name,
        "chain_type": "ethereum",
        "chain_key_type": "ethereum",
        "private_key": priv.to_hex(),
        "public_key": pub.to_hex(),
        "address": addr,
        "signature": signed_message.signature.hex(),  # sig.to_hex(),
        "payload": payload
    }

    print(json.dumps(key_data, indent=2))

if __name__ == "__main__":
    main()
