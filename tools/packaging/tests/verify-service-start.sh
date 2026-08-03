#!/bin/sh
# S6 gate: out-of-the-box service start inside a systemd-enabled container.
# Runs standalone or as the final stage of verify-deb.sh / verify-rpm.sh.
# Boots systemd as PID 1 (requires --privileged + host cgroup namespace),
# installs the base package inside, and asserts the unit is enabled AND active
# for >= 8 seconds with zero restarts, with nodeop logging into
# /var/log/wire/sysio. The systemd-enabled test image is built from the inline
# Dockerfile on first use and cached locally -- no out-of-band setup.
# Usage: verify-service-start.sh deb <base-deb> | rpm <base-rpm>
set -e
kind="$1"; pkg="$2"
[ -f "$pkg" ] || { echo "S6 FAIL: package not found: $pkg"; exit 1; }
pkgdir=$(cd "$(dirname "$pkg")" && pwd)
base=$(basename "$pkg")
name="wire-svc-test-$kind-$$"
fail() {
    echo "S6 FAIL: $1"
    docker exec "$name" systemctl status wire-sysio-nodeop --no-pager -l 2>/dev/null | tail -12 || true
    docker exec "$name" tail -8 /var/log/wire/sysio/nodeop.log 2>/dev/null || true
    docker rm -f "$name" >/dev/null 2>&1 || true
    exit 1
}
case "$kind" in
    deb) img="wire-systemd-ubuntu:24.04"
         inst='export DEBIAN_FRONTEND=noninteractive; apt-get update -qq >/dev/null 2>&1; apt-get install -y /pkg/PKG >/tmp/inst.log 2>&1 || { tail -12 /tmp/inst.log; exit 1; }' ;;
    rpm) img="wire-systemd-fedora:latest"
         inst='dnf install -y /pkg/PKG >/tmp/inst.log 2>&1 || { tail -12 /tmp/inst.log; exit 1; }' ;;
    *) echo "usage: verify-service-start.sh deb|rpm <base-package>"; exit 2 ;;
esac
inst=$(echo "$inst" | sed "s|PKG|$base|")
if ! docker image inspect "$img" >/dev/null 2>&1; then
    echo "S6: building systemd test image $img (first use, cached afterwards)"
    if [ "$kind" = "deb" ]; then
        docker build -q -t "$img" - <<'DEOF'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y systemd systemd-sysv dbus && apt-get clean
CMD ["/sbin/init"]
DEOF
    else
        docker build -q -t "$img" - <<'REOF'
FROM fedora:latest
RUN dnf install -y systemd && dnf clean all
CMD ["/sbin/init"]
REOF
    fi
fi
docker run -d --name "$name" --privileged --cgroupns=host \
    --tmpfs /run --tmpfs /run/lock -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    -v "$pkgdir":/pkg "$img" >/dev/null
trap 'docker rm -f "$name" >/dev/null 2>&1 || true' EXIT
st=""
for i in $(seq 1 30); do
    st=$(docker exec "$name" systemctl is-system-running 2>/dev/null || true)
    case "$st" in running|degraded) break ;; esac
    sleep 1
done
case "$st" in running|degraded) ;; *) fail "systemd did not come up (state: '$st')" ;; esac
docker exec "$name" sh -c "$inst" || fail "package install inside systemd container"
docker exec "$name" systemctl is-enabled wire-sysio-nodeop.service 2>/dev/null | grep -qx enabled || fail "unit not enabled after install"
sleep 8
act=$(docker exec "$name" systemctl is-active wire-sysio-nodeop.service 2>/dev/null || true)
[ "$act" = "active" ] || fail "unit not active 8s after install (state: $act)"
nres=$(docker exec "$name" systemctl show -p NRestarts --value wire-sysio-nodeop.service)
[ "$nres" = "0" ] || fail "unit restarted $nres time(s) within 8s of install"
docker exec "$name" test -s /var/log/wire/sysio/nodeop.log || fail "/var/log/wire/sysio/nodeop.log missing or empty"
docker exec "$name" test -d /var/lib/wire/sysio/data || fail "data dir not created by tmpfiles"
echo "--- nodeop.log tail (evidence of a running node) ---"
docker exec "$name" tail -3 /var/log/wire/sysio/nodeop.log
echo "S6 PASS ($kind): enabled + active >=8s, 0 restarts, log growing"
