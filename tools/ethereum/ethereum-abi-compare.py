#!/usr/bin/env python3
import sys
import codecs
from typing import List

import click
from decimal import Decimal, getcontext

# noinspection SpellCheckingInspection
getcontext().prec = 100


# noinspection PyBroadException
@click.command()
@click.option('--diff', '--diff_mode', is_flag=True, help='Diff mode (expects exactly two hex strings)')
@click.argument('hex_data_chunks', required=True, nargs=-1)
def run(diff: bool, hex_data_chunks: List[str]):
    
    def split_hex_data_lines(hex_data: str):
        hex_data = hex_data[8:]
        hex_data_len = len(hex_data)
        assert hex_data_len % 64 == 0
        return [hex_data[i:i + 64] for i in range(0, hex_data_len, 64)]

    def print_hex_data_line(line: str, line_num: int, line_numbers: bool = False, color: str = None):
        output = f"{line_num:>4} {line}" if line_numbers else line
        if color:
            output = click.style(output, fg=color)
        click.echo(output)

    def print_hex_data(hex_data: str, line_numbers: bool = False, color: str = None):

        # Split into 64-character chunks (32 bytes)
        lines = split_hex_data_lines(hex_data)

        for idx, line in enumerate(lines, start=1):
            print_hex_data_line(line, idx, line_numbers, color)

        click.echo()

    if diff:
        if len(hex_data_chunks) != 2:
            click.echo("Error: diff mode expects exactly two hex strings", file=sys.stderr)
            sys.exit(1)

        data1, data2 = hex_data_chunks
        lines1 = split_hex_data_lines(data1)
        lines2 = split_hex_data_lines(data2)
        for idx, (line1, line2) in enumerate(zip(lines1, lines2), start=1):
            if line1 != line2:
                print_hex_data_line(line1, idx, line_numbers=True, color='red')
                print_hex_data_line(line2, idx, line_numbers=True, color='green')
            else:
                print_hex_data_line(line1, idx, line_numbers=True)
    else:
        for idx, hex_chunk in enumerate(hex_data_chunks, start=1):
            if len(hex_data_chunks) > 1:
                click.echo(f"=== Hex data chunk {idx} ===")
            print_hex_data(hex_chunk, line_numbers=True)


if __name__ == '__main__':
    run()
