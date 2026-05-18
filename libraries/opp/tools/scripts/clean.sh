#!/usr/bin/env bash
set -euo pipefail

repoRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "Cleaning TypeScript build artifacts in $repoRoot"

find "$repoRoot" -name "*.tsbuildinfo" -delete 2>/dev/null || true
for dir in "$repoRoot"/protobuf-bundler "$repoRoot"/protoc-gen-solana "$repoRoot"/protoc-gen-solidity; do
  if [ -d "$dir" ]; then
    echo "Cleaning $dir"
    rm -rf "$dir/dist" 2>/dev/null || true
    rm -rf "$dir/out" 2>/dev/null || true
    rm -rf "$dir/lib" 2>/dev/null || true
  fi
done
