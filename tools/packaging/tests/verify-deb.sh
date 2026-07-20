#!/bin/sh
# S2 gate: DEB payload, control files, and containerized install.
# Usage: verify-deb.sh <base-deb> [dev-deb]
set -e
d="$1"; dev="$2"
[ -f "$d" ] || { echo "S2 FAIL: deb not found: $d"; exit 1; }
fail() { echo "S2 FAIL: $1"; exit 1; }
c=$(dpkg-deb -c "$d" | awk '{print $6}')
for f in ./usr/bin/nodeop ./usr/lib/systemd/system/wire-sysio-nodeop.service ./usr/lib/tmpfiles.d/wire-sysio.conf ./etc/logrotate.d/wire-sysio-nodeop; do
    echo "$c" | grep -qx "$f" || fail "payload missing $f"
done
echo "$c" | grep -q "^./usr/include/" && fail "base deb leaks headers"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
dpkg-deb -e "$d" "$tmp/ctrl"
for s in control templates config postinst prerm postrm; do
    [ -f "$tmp/ctrl/$s" ] || fail "control member missing: $s"
done
grep -q "^Depends:.*debconf" "$tmp/ctrl/control" || fail "control missing debconf dependency"
if [ -n "$dev" ]; then
    [ -f "$dev" ] || fail "dev deb not found: $dev"
    dc=$(dpkg-deb -c "$dev" | awk '{print $6}')
    for f in ./usr/include/fc ./usr/include/sysio/chain ./usr/lib/libfc.a ./usr/share/sysio_testing; do
        echo "$dc" | grep -q "^$f" || fail "dev payload missing $f"
    done
    dpkg-deb -e "$dev" "$tmp/devctrl"
    grep -q "^Depends:.*wire-sysio (= " "$tmp/devctrl/control" || fail "dev deb missing versioned base dependency"
    echo "$dc" | grep -q "^./usr/lib/python3/dist-packages/TestHarness" || fail "dev payload missing TestHarness dist-packages symlink"
    echo "$dc" | grep -q "^./usr/share/sysio_testing/bin/nodeop" || fail "dev payload missing sysio_testing/bin programs"
fi
pkgdir=$(cd "$(dirname "$d")" && pwd)
devbase=""; [ -n "$dev" ] && devbase=$(basename "$dev")
docker run --rm -v "$pkgdir":/pkg ubuntu:24.04 bash -ec "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y /pkg/$(basename "$d") ${devbase:+/pkg/$devbase} > /tmp/apt.log 2>&1 || { tail -15 /tmp/apt.log; exit 1; }
    dpkg -s wire-sysio | grep -q 'Status: install ok installed'
    # postinst exits before any debconf interaction when systemd is absent, so
    # exercise the config script explicitly; it registers both questions and
    # persists the yes/yes defaults exactly as a real prompting install would.
    dpkg-reconfigure -f noninteractive wire-sysio
    debconf-show wire-sysio > /tmp/dc 2>&1; cat /tmp/dc
    grep -q 'install-service: true' /tmp/dc
    grep -q 'enable-service: true' /tmp/dc
    test -x /usr/bin/nodeop
    test -f /etc/logrotate.d/wire-sysio-nodeop
    test -f /usr/lib/systemd/system/wire-sysio-nodeop.service
    test -f /usr/lib/tmpfiles.d/wire-sysio.conf
    if [ -n \"$devbase\" ]; then
        dpkg -s wire-sysio-dev | grep -q 'Status: install ok installed'
        test -d /usr/include/fc
        test -f /usr/lib/libfc.a
        test -L /usr/lib/python3/dist-packages/TestHarness
    fi
" || fail "container install / debconf defaults"
# S6: runtime verification -- the installed service must enable, start, and
# stay up inside a systemd-enabled container.
"$(cd "$(dirname "$0")" && pwd)/verify-service-start.sh" deb "$d" || fail "S6 service-start stage"
echo "S2 PASS: $d ${dev:+(+ $dev)}"
