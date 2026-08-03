#!/bin/sh
# S4 gate: containerized `cmake --install --component base` manifest diff vs
# pre-change baseline. Additions-only passes; any deletion/relocation fails.
# Usage: BASELINE=<baseline-file> verify-install-manifest.sh <build-dir>
set -e
b="$1"
[ -d "$b" ] || { echo "S4 FAIL: build dir not found: $b"; exit 1; }
[ -n "$BASELINE" ] && [ -f "$BASELINE" ] || { echo "S4 FAIL: BASELINE env var not set or file missing"; exit 1; }
ws=/data/shared/code/wire-platform
out=$(mktemp)
docker run --rm -v "$ws":"$ws" ubuntu:24.04 bash -ec "
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq cmake >/dev/null 2>&1
    DESTDIR=/tmp/post cmake --install '$b' --component base >/dev/null
    # cmake --install writes install_manifest_base.txt into the build dir as
    # root; hand it back to the workspace owner so host cpack can rewrite it.
    chown --reference='$b/CMakeCache.txt' '$b/install_manifest_base.txt' 2>/dev/null || true
    find /tmp/post \( -type f -o -type l \) | sed 's|^/tmp/post||' | sort
" > "$out"
sed 's|^/tmp/base||' "$BASELINE" | sort > "$out.base"
if diff -u "$out.base" "$out" > "$out.diff"; then
    echo "S4 PASS: manifest identical to baseline"
else
    if grep -q '^-[^-]' "$out.diff"; then
        echo "S4 FAIL: deletions/relocations vs baseline:"
        grep '^-[^-]' "$out.diff"
        exit 1
    fi
    echo "S4 PASS: additions only:"
    grep '^+[^+]' "$out.diff"
fi
rm -f "$out" "$out.base" "$out.diff"
