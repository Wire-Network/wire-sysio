#!/usr/bin/env python3
"""Convert the indexer JSON dump into sysio.cap::importseed action batches.

Schema (current snapshot, may evolve as the indexer stabilizes):
  metadata     bookkeeping; not consumed by the contract
  purchasers[] {address, totalPretokens, yieldClaimed, ...}
               owed = totalPretokens  (already net of yieldClaimed)
  stakers[]    {address, pretokenYield, yieldClaimed, ...}
               owed = pretokenYield - yieldClaimed

Per-address conversion:
  total_pretokens = sum(purchaser.totalPretokens)
                  + sum(staker.pretokenYield - staker.yieldClaimed)
  wire_atomic     = total_pretokens // 1_000_000_000   (floor, drop sub-atomic dust)

Rows with wire_atomic == 0 are filtered. Output is a JSON array of importseed
action arg objects, each batched up to --batch-size credits per call.

Usage:
  ./convert_import.py response_1778592566067.json > batches.json
  ./convert_import.py response_*.json --batch-size 100 --chain CHAIN_KIND_ETHEREUM
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

# WIRE has 9 decimals; source pretoken values are 1e18 scale (wei-style).
# Flooring source by DUST_BASE drops the sub-atomic remainder.
DUST_BASE = 1_000_000_000


def parse_args() -> argparse.Namespace:
   ap = argparse.ArgumentParser(
      description="Emit sysio.cap::importseed batches from the indexer JSON.",
      formatter_class=argparse.RawDescriptionHelpFormatter,
      epilog=__doc__,
   )
   ap.add_argument("input", type=Path,
                   help="Indexer JSON file (response_*.json shape)")
   ap.add_argument("--batch-size", type=int, default=50,
                   help="Credits per importseed call (default: 50)")
   ap.add_argument("--chain", default="CHAIN_KIND_ETHEREUM",
                   help="ChainKind enum name (default: CHAIN_KIND_ETHEREUM)")
   return ap.parse_args()


def accumulate(data: dict) -> dict[str, int]:
   """Return {hex_address_without_prefix: total_pretokens_owed}."""
   acc: dict[str, int] = defaultdict(int)
   for row in data.get("purchasers") or []:
      addr = row["address"].lower().removeprefix("0x")
      acc[addr] += int(row["totalPretokens"])
   for row in data.get("stakers") or []:
      addr = row["address"].lower().removeprefix("0x")
      owed = int(row["pretokenYield"]) - int(row.get("yieldClaimed", "0"))
      if owed > 0:
         acc[addr] += owed
   return acc


def to_credits(accumulator: dict[str, int]) -> tuple[list[dict], int]:
   """Floor each total at DUST_BASE; return (credits, total_dropped_dust)."""
   credits = []
   dropped_dust = 0
   for addr, total in sorted(accumulator.items()):
      atomic, dust = divmod(total, DUST_BASE)
      dropped_dust += dust
      if atomic <= 0:
         continue
      credits.append({"native_address": addr, "wire_atomic": atomic})
   return credits, dropped_dust


def batched(credits: list[dict], chain: str, batch_size: int) -> list[dict]:
   return [
      {"chain": chain, "credits": credits[i:i + batch_size]}
      for i in range(0, len(credits), batch_size)
   ]


def main() -> int:
   args = parse_args()

   data = json.loads(args.input.read_text())
   accumulator = accumulate(data)
   credits, dropped_dust = to_credits(accumulator)
   batches = batched(credits, args.chain, args.batch_size)

   json.dump(batches, sys.stdout, indent=2)
   sys.stdout.write("\n")

   total_atomic = sum(c["wire_atomic"] for c in credits)
   print(
      f"input:            {args.input}",
      f"unique addresses: {len(accumulator)}",
      f"non-zero credits: {len(credits)}",
      f"batches:          {len(batches)} (size {args.batch_size})",
      f"total credited:   {total_atomic} atomic WIRE"
      f" ({total_atomic / DUST_BASE:.6f} WIRE)",
      f"dropped dust:     {dropped_dust} sub-atomic units"
      f" ({dropped_dust / 10**18:.2e} WIRE)",
      sep="\n",
      file=sys.stderr,
   )
   return 0


if __name__ == "__main__":
   sys.exit(main())
