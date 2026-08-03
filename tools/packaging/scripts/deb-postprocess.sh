#!/bin/bash
set -euo pipefail

# Post-processes a built .deb's control archive:
# 1. Removes the Installed-Size field; it is not reproducible across
#    filesystems.
# 2. Collapses redundant multi-clause libc6 Depends down to the first clause
#    (historically emitted as e.g.
#       libc6 (>= 2.27), libc6 (>> 2.28), libc6 (<< 2.29), ...
#    which is unnecessarily restrictive).
#
# The control archive is fully unpacked and repacked with deterministic
# metadata instead of being patched in place: GNU tar's --delete/--update
# cannot edit the pax-format archives CPack emits ("Skipping to next
# header"), while a fresh GNU-format repack is portable and reproducible.

WORKDIR="$(mktemp -d)"
trap 'rm -rf -- "${WORKDIR}"' EXIT

if [ $# -lt 1 ]; then
   echo "Must specify .deb file to postprocess as argument to script"
   exit 1
fi

if [ ! -f "$1" ]; then
   echo "Argument passed is not a file"
   exit 1
fi

DEB_PATH="$(realpath "${1}")"
cd "${WORKDIR}"

ar x "${DEB_PATH}" control.tar.gz
gzip -d control.tar.gz
mkdir ctrl
tar xf control.tar -C ctrl
sed -i -E -e '/Installed-Size/d' -e 's/, libc6[^,]+//g' ctrl/control
(cd ctrl && find . -mindepth 1 -maxdepth 1 -type f -printf './%f\n' | LC_ALL=C sort) > members.txt
rm -f control.tar
tar cf control.tar --format=gnu --mtime=@0 --owner=0 --group=0 --numeric-owner \
    --no-recursion -C ctrl -T members.txt
gzip -n control.tar
ar rD "${DEB_PATH}" control.tar.gz
