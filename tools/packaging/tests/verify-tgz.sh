#!/bin/sh
# S1 gate: portable tarball layout. Usage: verify-tgz.sh [--no-service] <tarball>
#
# Default mode verifies a LINUX portable tarball: the binaries plus the packaged
# service assets (systemd unit, tmpfiles.d fragment, logrotate policy).
#
# --no-service verifies a tarball built on a platform that ships no service
# payload (macOS -- cmake/package.cmake gates those install() rules on NOT APPLE).
# It asserts the root directory, the binaries and the bundled licenses ARE
# present, and that every service file is ABSENT: a macOS tarball that somehow
# carried a systemd unit would mean the payload gating regressed.
set -e
no_service=0
if [ "$1" = "--no-service" ]; then
    no_service=1
    shift
fi
t="$1"
[ -f "$t" ] || { echo "S1 FAIL: tarball not found: $t"; exit 1; }
fail() { echo "S1 FAIL: $1"; exit 1; }
list=$(tar tzf "$t")
bad=$(echo "$list" | grep -v '^wire-sysio/' || true)
[ -z "$bad" ] || fail "entries outside wire-sysio/: $bad"
echo "$list" | grep -q '^wire-sysio/usr/' && fail "unexpected usr/ layer inside tarball"

service_files="lib/systemd/system/wire-sysio-nodeop.service lib/tmpfiles.d/wire-sysio.conf etc/logrotate.d/wire-sysio-nodeop"

# All three programs install into the `base` component (programs/*/CMakeLists.txt),
# so all three belong in the portable tarball on every platform -- a tarball with
# nodeop but no clio/kiod means the component staging regressed. (A single-element
# `for` loop here was also a shellcheck SC2043.)
for f in bin/nodeop bin/clio bin/kiod; do
    echo "$list" | grep -qx "wire-sysio/$f" || fail "missing wire-sysio/$f"
done

if [ "$no_service" -eq 1 ]; then
    # Licenses are part of the base component on every platform, so their absence
    # would mean the tarball was staged from the wrong component set.
    echo "$list" | grep -q '^wire-sysio/share/licenses/sysio/' \
        || fail "missing wire-sysio/share/licenses/sysio/"
    for f in $service_files; do
        echo "$list" | grep -qx "wire-sysio/$f" \
            && fail "service file present in a no-service tarball: wire-sysio/$f"
    done
    echo "S1 PASS (no-service): $t"
else
    for f in $service_files; do
        echo "$list" | grep -qx "wire-sysio/$f" || fail "missing wire-sysio/$f"
    done
    echo "S1 PASS: $t"
fi
