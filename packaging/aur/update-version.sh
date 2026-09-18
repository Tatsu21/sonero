#!/usr/bin/env bash
#
# Point the AUR release package at a new tag.
#
#   ./packaging/aur/update-version.sh 0.1.3
#
# Downloads the tag's source tarball from GitHub, writes its checksum into the
# PKGBUILD, resets pkgrel, and regenerates both .SRCINFO files. The AUR keeps no
# copy of the source, so a wrong checksum is the one mistake every user of the
# package hits at once — this derives it rather than trusting a paste.
#
# Publishing the result is a second step: packaging/aur/sync-to-aur.sh.

set -euo pipefail

VERSION="${1:-}"
if [[ -z ${VERSION} ]]; then
    echo "usage: $(basename "$0") <version>   e.g. 0.1.3" >&2
    exit 2
fi
VERSION="${VERSION#v}"
if [[ ! ${VERSION} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: '${VERSION}' is not x.y.z — the AUR package is named after a release" >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKGBUILD="${HERE}/sonero/PKGBUILD"
URL="https://github.com/Tatsu21/sonero/archive/refs/tags/${VERSION}.tar.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "fetching ${URL}"
# --fail: without it curl writes GitHub's 404 page to the file and happily
# hashes that, producing a package that downloads an error and builds nothing.
if ! curl -fsSL --retry 3 -o "${tmp}/src.tar.gz" "${URL}"; then
    echo "error: no tarball at ${URL} — is the ${VERSION} release published?" >&2
    exit 1
fi

sum="$(sha256sum "${tmp}/src.tar.gz" | cut -d' ' -f1)"
echo "sha256 ${sum}"

sed -i \
    -e "s/^pkgver=.*/pkgver=${VERSION}/" \
    -e "s/^pkgrel=.*/pkgrel=1/" \
    -e "s/^sha256sums=.*/sha256sums=('${sum}')/" \
    "${PKGBUILD}"

"${HERE}/gen-srcinfo.sh"

echo
echo "sonero is now ${VERSION}. Review the diff, commit, then publish with:"
echo "  ./packaging/aur/sync-to-aur.sh sonero"
