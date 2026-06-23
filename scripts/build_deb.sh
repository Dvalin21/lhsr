#!/bin/bash
# scripts/build_deb.sh — Build LHSR Debian packages
#
# Produces:
#   lhsr-dkms_<version>_all.deb   (kernel module source)
#   lhsrd_<version>_<arch>.deb     (userspace tools)
#   lhsr_<version>_all.deb         (metapackage)
#
# Usage: ./scripts/build_deb.sh [--sign] [--no-dkms]

set -euo pipefail

cd "$(dirname "$0")/.."
SIGN="--no-sign"

for arg in "$@"; do
    case "$arg" in
        --sign) SIGN="" ;;
        --no-dkms) SKIP_DKMS=1 ;;
    esac
done

# Ensure dependencies
for cmd in dpkg-buildpackage debuild debhelper; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "Missing: $cmd — install devscripts and debhelper"
        exit 1
    fi
done

# Clean previous builds
rm -rf build/deb

# Build the packages
dpkg-buildpackage ${SIGN:+-us -uc} -b

# Collect results
mkdir -p build/deb
mv ../lhsr_* ../lhsr-dkms_* ../lhsrd_* ../lhsrd-dbgsym_* build/deb/ 2>/dev/null || true

echo ""
echo "Packages built:"
ls -lh build/deb/
