#!/bin/bash
# scripts/build_rpm.sh — Build LHSR RPM packages
#
# Produces:
#   lhsr-dkms-<version>.noarch.rpm  (kernel module source via DKMS)
#   lhsrd-<version>.<arch>.rpm      (userspace tools)
#
# Usage: ./scripts/build_rpm.sh [--no-dkms]

set -euo pipefail

cd "$(dirname "$0")/.."

# Check for rpmbuild
if ! command -v rpmbuild &>/dev/null; then
    echo "Missing: rpmbuild — install rpm-build"
    exit 1
fi

# Clean and create build tree
rm -rf build/rpm
mkdir -p build/rpm/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

# Create source tarball
VERSION=$(grep 'PACKAGE_VERSION=' dkms.conf | cut -d'"' -f2)
git archive --format=tar --prefix=lhsr-${VERSION}/ HEAD | gzip > build/rpm/SOURCES/lhsr-${VERSION}.tar.gz

# Build SRPM and RPM
rpmbuild --define "_topdir $(pwd)/build/rpm" \
         -ta build/rpm/SOURCES/lhsr-${VERSION}.tar.gz

echo ""
echo "RPMs built:"
find build/rpm/RPMS -name '*.rpm' -exec ls -lh {} \;
