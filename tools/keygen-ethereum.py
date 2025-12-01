import os
import json
from eth_keys import keys
from eth_utils import keccak
from eth_account import Account
from eth_account.messages import encode_defunct

# 1) Generate 32-byte random private key
# priv_bytes = HexBytes("0x70ebafd02898154d0fc465cac8977a87eece51c9f487af818b4d74de5c913f2a")
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
    "private_key": priv.to_hex(),
    "public_key": pub.to_hex(),
    "address": addr,
    "signature": signed_message.signature.hex(),  # sig.to_hex(),
    "payload": payload
}
# print("Private key (hex):", priv.to_hex())          # 0x-prefixed 64-hex
# print("Public key (hex): ", pub.to_hex())           # 0x04 + 64-byte coords
# print("Address:          ", addr)                   # 0x + 40-hex

print(json.dumps(key_data))
