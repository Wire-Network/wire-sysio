#!/bin/sh
# S3 gate: RPM payload, scriptlets, version tag, and containerized install.
# Usage: verify-rpm.sh <base-rpm> [dev-rpm]
set -e
r="$1"; dev="$2"
[ -f "$r" ] || { echo "S3 FAIL: rpm not found: $r"; exit 1; }
fail() { echo "S3 FAIL: $1"; exit 1; }
l=$(rpm -qlp "$r" 2>/dev/null)
for f in /usr/bin/nodeop /usr/lib/systemd/system/wire-sysio-nodeop.service /usr/lib/tmpfiles.d/wire-sysio.conf /etc/logrotate.d/wire-sysio-nodeop; do
    echo "$l" | grep -qx "$f" || fail "payload missing $f"
done
echo "$l" | grep -q "^/usr/include/" && fail "base rpm leaks headers"
rpm -qp --scripts "$r" 2>/dev/null | grep -q "enable --now wire-sysio-nodeop" || fail "%post missing enable --now"
v=$(rpm -qp --qf '%{VERSION}' "$r" 2>/dev/null)
case "$v" in *-*) fail "version tag contains hyphen: $v" ;; esac
if [ -n "$dev" ]; then
    [ -f "$dev" ] || fail "dev rpm not found: $dev"
    dl=$(rpm -qlp "$dev" 2>/dev/null)
    for f in /usr/include/fc /usr/include/sysio/chain /usr/lib/libfc.a; do
        echo "$dl" | grep -q "^$f" || fail "dev payload missing $f"
    done
    n=$(rpm -qp --scripts "$dev" 2>/dev/null | grep -c systemctl || true)
    [ "$n" = "0" ] || fail "dev rpm carries service scriptlets"
    rpm -qp --requires "$dev" 2>/dev/null | grep -q "wire-sysio = " || fail "dev rpm missing versioned base requirement"
fi
pkgdir=$(cd "$(dirname "$r")" && pwd)
devbase=""; [ -n "$dev" ] && devbase=$(basename "$dev")
docker run --rm -v "$pkgdir":/pkg fedora:latest bash -ec "
    dnf install -y /pkg/$(basename "$r") ${devbase:+/pkg/$devbase} >/dev/null
    rpm -q wire-sysio
    test -x /usr/bin/nodeop
    test -f /etc/logrotate.d/wire-sysio-nodeop
    if [ -n \"$devbase\" ]; then rpm -q wire-sysio-dev; test -d /usr/include/fc; fi
" || fail "fedora container install"
# S6: runtime verification -- the installed service must enable, start, and
# stay up inside a systemd-enabled container.
"$(cd "$(dirname "$0")" && pwd)/verify-service-start.sh" rpm "$r" || fail "S6 service-start stage"
echo "S3 PASS: $r ${dev:+(+ $dev)}"
