#!/usr/bin/env bash
# Run the whole packaging verification suite (S1-S6) over one directory of artifacts.
#
# Usage: verify-all.sh <dir> [version]
#
#   WITH <version>    -- the RELEASE path. Every artifact is named EXPLICITLY, so
#                        a missing or misnamed file fails here rather than being
#                        waved through by a glob that happened to match nothing.
#                        The release directory also holds the macOS tarball.
#   WITHOUT <version> -- the BUILD path. The caller does not know the version the
#                        artifact names carry, so the safe globs documented below
#                        are used instead. The linux build artifact contains no
#                        macOS tarball, hence no macOS check in this mode.
#
# THE GLOB RULE -- the reason this lives in ONE file: never hand verify-tgz.sh a
# bare `wire-sysio-*.tar.gz`. That pattern globs the linux tarball, the macOS
# tarball and the system-contracts bundle that tag builds add, and the verifier
# silently checks whichever one sorted first -- so a broken tarball can ship
# having "passed". Every tarball pattern here is ARCH-SUFFIXED, and the base rpm
# is matched as `wire-sysio-[0-9]*` so the `-dev` rpm can never be picked up in
# its place.
#
# The system-contracts bundle is deliberately NOT verified here: it exists only
# on the tag/release path and its check (wasm + abi presence) is a different
# assertion, so it stays its own step in release.yaml.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: verify-all.sh <dir> [version]" >&2; exit 1; }
dir="$1"
version="${2:-}"
[[ -d "$dir" ]] || { echo "verify-all: not a directory: $dir" >&2; exit 1; }

ls -la "$dir"

# S5 first: a static lint of the packaged unit, the maintainer scripts and the
# verifier suite itself, so a broken verifier is reported as such instead of as
# a package failure.
"$here/verify-scripts.sh"

if [[ -n "$version" ]]; then
   "$here/verify-tgz.sh" "$dir/wire-sysio-${version}-x86_64.tar.gz"
   # --no-service: the macOS tarball must carry no Linux service payload.
   "$here/verify-tgz.sh" --no-service "$dir/wire-sysio-${version}-macos-arm64.tar.gz"
   "$here/verify-deb.sh" "$dir/wire-sysio_${version}_amd64.deb" "$dir/wire-sysio-dev_${version}_amd64.deb"
   "$here/verify-rpm.sh" "$dir/wire-sysio-${version}-x86_64.rpm" "$dir/wire-sysio-dev-${version}-x86_64.rpm"
else
   "$here/verify-tgz.sh" "$dir"/wire-sysio-*-x86_64.tar.gz
   "$here/verify-deb.sh" "$dir"/wire-sysio_*.deb "$dir"/wire-sysio-dev_*.deb
   "$here/verify-rpm.sh" "$dir"/wire-sysio-[0-9]*.rpm "$dir"/wire-sysio-dev-*.rpm
fi
