#!/usr/bin/env python3
import sys
import codecs

import click
from decimal import Decimal, getcontext

# noinspection SpellCheckingInspection
getcontext().prec = 100


# noinspection PyBroadException
@click.command()
@click.option('-r', '--reverse', is_flag=True, help='Convert WEI to ETH instead of ETH to WEI')
@click.argument('amount')
def run(reverse, amount):
    if amount.startswith('0x'):
        try:
            hex_str = amount[2:]
            value = Decimal(int(hex_str, 16))
        except (ValueError, TypeError) as e:
            click.echo(f"Invalid number ({amount}): {str(e)}", err=True)
            sys.exit(1)
    else:
        try:
            value = Decimal(amount)
        except Exception as e:
            click.echo(f"Invalid number ({amount}): {str(e)}", file=sys.stderr)
            sys.exit(1)

    if value < 0:
        click.echo("Amount must be non-negative", file=sys.stderr)
        sys.exit(1)

    if reverse:
        eth = value / (Decimal(10) ** 18)
        click.echo(f"{eth}")
    else:
        wei = value * (Decimal(10) ** 18)
        # Ensure no precision below 1 wei (i.e., no more than 18 decimals)
        if wei != wei.to_integral_value():
            click.echo("Error: more than 18 decimal places (below 1 wei).", file=sys.stderr)
            sys.exit(2)
        click.echo(hex(int(wei)))


if __name__ == '__main__':
    run()
