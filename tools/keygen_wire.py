import click
import json
import random
import re
import shutil
import subprocess
import sys

from sysio_name_encoder import convert_name_to_value

# noinspection PyBroadException
try:
    from eth_keys import keys as eth_keys
except Exception as _e:
    eth_keys = None


# noinspection PyBroadException
def ensure_clio_available():
    exe = shutil.which("clio")
    if not exe:
        print("clio is not available on PATH", file=sys.stderr)
        sys.exit(1)

    try:
        subprocess.run([exe, "--help"], capture_output=True, text=True, check=True)
    except Exception:  # pylint: disable=broad-except
        print("failed to execute clio", file=sys.stderr)
        sys.exit(1)


def generate_chain_id() -> str:
    chars = "abcdef0123456789"
    return ''.join(random.choice(chars) for _ in range(64))


@click.command()
@click.argument('account_name')
@click.argument('key_name')
def main(account_name, key_name):
    """
    ACCOUNT_NAME is the SYSIO name of the account to generate.
    KEY_NAME is the string used to reference the key in the `signature_provider`
    """
    ensure_clio_available()

    # create key via clio
    proc = subprocess.run(["clio", "create", "key", "--to-console"], capture_output=True, text=True)

    if proc.returncode != 0:
        click.echo(proc.stderr or proc.stdout, err=True)
        sys.exit(proc.returncode)

    out = (proc.stdout or "").strip()
    m = re.search(r"Private key:\s*(\S+)\s+Public key:\s*(\S+)", out)
    if not m:
        click.echo("unexpected clio output: " + out, err=True)
        sys.exit(1)
    priv_wif = m.group(1)
    pub_key_text = m.group(2)

    chain_id = generate_chain_id()
    payload = "simple-text-payload"

    sign_args = ["clio", "sign", "--private-key", priv_wif, "--chain-id", chain_id, '{"payload":"' + payload + '"}']
    proc = subprocess.run(sign_args, capture_output=True, text=True)
    if proc.returncode != 0:
        click.echo(proc.stderr or proc.stdout, err=True)
        sys.exit(proc.returncode)

    trx_json = json.loads(proc.stdout)
    signature_hex = trx_json["signatures"][0]

    # compute address from provided name
    try:
        address_val = convert_name_to_value(account_name)
    except Exception as e:
        click.echo(f"invalid name '{account_name}': {e}", err=True)
        sys.exit(2)

    key_data = {
        "key_name": key_name,
        "chain_type": "wire",
        "chain_key_type": "wire",
        "chain_id": chain_id,
        "account_name": account_name,
        "private_key": priv_wif,
        "public_key": pub_key_text,
        "address": str(address_val),
        "payload": payload,
        "signature": signature_hex, }

    click.echo(json.dumps(key_data, indent=2))


if __name__ == "__main__":
    main()
