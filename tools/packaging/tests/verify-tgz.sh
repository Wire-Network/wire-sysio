#!/bin/sh
# S1 gate: portable tarball layout. Usage: verify-tgz.sh <tarball>
set -e
t="$1"
[ -f "$t" ] || { echo "S1 FAIL: tarball not found: $t"; exit 1; }
fail() { echo "S1 FAIL: $1"; exit 1; }
list=$(tar tzf "$t")
bad=$(echo "$list" | grep -v '^wire-sysio/' || true)
[ -z "$bad" ] || fail "entries outside wire-sysio/: $bad"
echo "$list" | grep -q '^wire-sysio/usr/' && fail "unexpected usr/ layer inside tarball"
for f in bin/nodeop lib/systemd/system/wire-sysio-nodeop.service lib/tmpfiles.d/wire-sysio.conf etc/logrotate.d/wire-sysio-nodeop; do
    echo "$list" | grep -qx "wire-sysio/$f" || fail "missing wire-sysio/$f"
done
echo "S1 PASS: $t"
