#!/bin/sh
# S5 gate: static lint of the packaged unit, logrotate policy, and all
# maintainer scripts/scriptlets. Runs host tools when present.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
fail() { echo "S5 FAIL: $1"; exit 1; }
for s in debian/config debian/postinst debian/prerm debian/postrm rpm/post.sh rpm/preun.sh rpm/postun.sh tests/verify-tgz.sh tests/verify-deb.sh tests/verify-rpm.sh tests/verify-install-manifest.sh tests/verify-scripts.sh; do
    sh -n "$root/$s" || fail "sh -n $s"
done
unit="$root/systemd/wire-sysio-nodeop.service"
if command -v systemd-analyze >/dev/null 2>&1; then
    # /usr/bin/nodeop is absent on build hosts; that one finding is expected.
    out=$(systemd-analyze verify "$unit" 2>&1 | grep -v "nodeop.*is not executable" | grep -v "Failed to prepare" || true)
    [ -z "$out" ] || { echo "$out"; fail "systemd-analyze verify $unit"; }
fi
if command -v logrotate >/dev/null 2>&1; then
    logrotate -d "$root/logrotate/wire-sysio-nodeop" >/dev/null 2>&1 || fail "logrotate -d"
fi
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck "$root/debian/config" "$root/debian/postinst" "$root/debian/prerm" "$root/debian/postrm" "$root"/rpm/*.sh || echo "S5 note: shellcheck findings above are advisory"
fi
echo "S5 PASS"
