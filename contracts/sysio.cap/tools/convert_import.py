#!/usr/bin/env python3
"""Convert the indexer JSON dump into sysio.cap::importseed action batches.

Live sources:
  ETH: curl -H 'x-api-key: <key>' https://index.wire.foundation/opp/balances
  SOL: curl -H 'x-api-key: <key>' https://index.wire.foundation/opp/solana/balances

Schema (verified 2026-05-13):
  metadata     bookkeeping; not consumed by the contract. Notable fields:
               generatedAt, totalMessages, yieldDust (ETH; not present on SOL).
  purchasers[] {address, totalPretokens, yieldClaimed?, ...}
               owed = totalPretokens  (already net of yieldClaimed; field
               absent on SOL).
  stakers[]    {address, pretokenYield, yieldClaimed?, ...}
               owed = pretokenYield - yieldClaimed  (yieldClaimed absent
               on SOL -> defaults to 0).

A given address may appear in both arrays (~9 such addresses in current
SOL snapshot); contributions sum per address.

Per-chain conventions:
  CHAIN_KIND_ETHEREUM
    address  0x-prefixed lowercase hex, 20 raw bytes
    source   18 decimals (wei-style)
    divisor  10^9  (source 1e18 -> WIRE atomic 1e9)
  CHAIN_KIND_SOLANA
    address  base58 (case-sensitive), 32 raw bytes
    source   9 decimals (lamport-style; same as WIRE atomic)
    divisor  1     (no scaling needed)

Per-address conversion:
  total       = sum(purchaser.totalPretokens)
              + sum(staker.pretokenYield - staker.yieldClaimed)
  wire_atomic = total // divisor       (floor; drop sub-atomic dust)

Rows with wire_atomic == 0 are filtered. Output is a JSON array of
importseed action arg objects, each batched up to --batch-size credits
per call. `native_address` is emitted as the hex spelling of the raw
bytes (no 0x prefix), which the sysio.cap ABI consumes as `bytes`.

Usage:
  ./convert_import.py eth_balances.json --chain CHAIN_KIND_ETHEREUM > eth.json
  ./convert_import.py sol_balances.json --chain CHAIN_KIND_SOLANA   > sol.json
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Callable


# ---------------------------------------------------------------------------
#  Base58 decoder (Bitcoin / Solana alphabet). Stdlib has no base58.
# ---------------------------------------------------------------------------
B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
B58_INDEX = {c: i for i, c in enumerate(B58_ALPHABET)}


def base58_decode(s: str) -> bytes:
   n = 0
   for c in s:
      if c not in B58_INDEX:
         raise ValueError(f"invalid base58 character: {c!r}")
      n = n * 58 + B58_INDEX[c]
   leading_ones = len(s) - len(s.lstrip("1"))
   payload = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
   return b"\x00" * leading_ones + payload


def eth_decode(addr: str) -> bytes:
   return bytes.fromhex(addr.lower().removeprefix("0x"))


# ---------------------------------------------------------------------------
#  Chain configuration
# ---------------------------------------------------------------------------
CHAIN_CONFIG = {
   "CHAIN_KIND_ETHEREUM": {
      "decoder":  eth_decode,
      "addr_len": 20,
      "divisor":  10**9,  # source 1e18 -> WIRE atomic 1e9
   },
   "CHAIN_KIND_SOLANA": {
      "decoder":  base58_decode,
      "addr_len": 32,
      "divisor":  1,       # source 1e9 already matches WIRE atomic
   },
}


# ---------------------------------------------------------------------------
#  Pipeline
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
   ap = argparse.ArgumentParser(
      description="Emit sysio.cap::importseed batches from the indexer JSON.",
      formatter_class=argparse.RawDescriptionHelpFormatter,
      epilog=__doc__,
   )
   ap.add_argument("input", type=Path,
                   help="Indexer JSON file (ETH or SOL /opp/[solana/]balances shape)")
   ap.add_argument("--batch-size", type=int, default=50,
                   help="Credits per importseed call (default: 50)")
   ap.add_argument("--chain", default="CHAIN_KIND_ETHEREUM",
                   choices=sorted(CHAIN_CONFIG.keys()),
                   help="ChainKind enum name (default: CHAIN_KIND_ETHEREUM)")
   return ap.parse_args()


def accumulate(data: dict, decoder: Callable[[str], bytes], addr_len: int) -> dict[bytes, int]:
   """Return {raw_address_bytes: total_pretokens_owed}.

   Addresses appearing in both `purchasers` and `stakers` sum together.
   The accumulator is keyed by decoded raw bytes so that case / format
   normalization happens exactly once at decode time.
   """
   acc: dict[bytes, int] = defaultdict(int)

   def addr_bytes(s: str) -> bytes:
      b = decoder(s)
      if len(b) != addr_len:
         raise ValueError(f"address {s!r} decoded to {len(b)} bytes, expected {addr_len}")
      return b

   for row in data.get("purchasers") or []:
      acc[addr_bytes(row["address"])] += int(row["totalPretokens"])
   for row in data.get("stakers") or []:
      owed = int(row["pretokenYield"]) - int(row.get("yieldClaimed", "0"))
      if owed > 0:
         acc[addr_bytes(row["address"])] += owed
   return acc


def to_credits(accumulator: dict[bytes, int], divisor: int) -> tuple[list[dict], int]:
   """Floor each total at `divisor`; return (credits, dropped_dust_total)."""
   credits = []
   dropped_dust = 0
   for addr_bytes, total in sorted(accumulator.items()):
      atomic, dust = divmod(total, divisor)
      dropped_dust += dust
      if atomic <= 0:
         continue
      credits.append({"native_address": addr_bytes.hex(), "wire_atomic": atomic})
   return credits, dropped_dust


def batched(credits: list[dict], chain: str, batch_size: int) -> list[dict]:
   return [
      {"chain": chain, "credits": credits[i:i + batch_size]}
      for i in range(0, len(credits), batch_size)
   ]


def main() -> int:
   args = parse_args()
   cfg = CHAIN_CONFIG[args.chain]

   data = json.loads(args.input.read_text())
   accumulator = accumulate(data, cfg["decoder"], cfg["addr_len"])
   credits, dropped_dust = to_credits(accumulator, cfg["divisor"])
   batches = batched(credits, args.chain, args.batch_size)

   json.dump(batches, sys.stdout, indent=2)
   sys.stdout.write("\n")

   total_atomic = sum(c["wire_atomic"] for c in credits)
   meta = data.get("metadata") or {}
   lines = [
      f"input:            {args.input}",
      f"chain:            {args.chain}",
      f"generatedAt:      {meta.get('generatedAt', '<missing>')}",
      f"totalMessages:    {meta.get('totalMessages', '<missing>')}",
      f"indexer yieldDust: {meta.get('yieldDust', '<missing>')}"
      " (indexer-side ledger; ETH only)",
      f"unique addresses: {len(accumulator)}",
      f"non-zero credits: {len(credits)}",
      f"batches:          {len(batches)} (size {args.batch_size})",
      f"total credited:   {total_atomic} atomic WIRE"
      f" ({total_atomic / 10**9:.6f} WIRE)",
      f"dropped dust:     {dropped_dust} sub-atomic units"
      f" (divisor {cfg['divisor']})",
   ]
   print(*lines, sep="\n", file=sys.stderr)
   return 0


if __name__ == "__main__":
   sys.exit(main())
