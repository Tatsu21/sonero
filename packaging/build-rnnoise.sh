#!/usr/bin/env bash
#
# Build RNNoise and install it where CMake will find it.
#
#   ./packaging/build-rnnoise.sh [prefix]     # default: /usr/local
#
# Sonero's microphone noise suppression needs this library, and only Arch ships a
# package of it. Debian has one in sid and forky but not in trixie, Ubuntu has
# none in 22.04 or 24.04, and Fedora has none at all — so on every target except
# Arch, "install the package" is not an option and the library has to be built.
#
# Static on purpose. A .deb cannot depend on a library its distribution does not
# carry, and an AppImage that expects one on the host is an AppImage that fails on
# the machines it exists for. Linked in, it is about a hundred kilobytes and the
# question disappears.
#
# The tarballs, their checksums and the model version are the ones Arch's own
# rnnoise package uses — a source that is already trusted to build this library
# for a distribution, rather than a URL picked here.

set -euo pipefail

PREFIX="${1:-/usr/local}"
VERSION="0.2"
MODEL_VERSION="0b50c45"

SOURCE_URL="https://gitlab.xiph.org/xiph/rnnoise/-/archive/v${VERSION}/rnnoise-v${VERSION}.tar.gz"
MODEL_URL="https://media.xiph.org/rnnoise/models/rnnoise_data-${MODEL_VERSION}.tar.gz"

SOURCE_SHA512="930aa892299edbc1d512803df6b845ea6164eb498cacdab9970e5ae799bc6cf3c8c94d2b9576955fb9a2d8aa13a6d255e58fb99d0367a0d0ef842a1cb938e674"
MODEL_SHA512="c15fef7c88d86264a29a3dab14d94bde769da68f255d131d135f6a40d94037b1ffe521f9e0a26339114750dbdd7cf774c3185ba40279c74200fb32732f57db8b"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

fetch() {
    local url="$1" out="$2" want="$3"
    echo "fetching $(basename "${url}")"
    # --fail, or curl writes the server's error page to the file and the checksum
    # below is the only thing standing between that and the build.
    curl -fsSL --retry 3 -o "${out}" "${url}"
    local got
    got="$(sha512sum "${out}" | cut -d' ' -f1)"
    if [[ "${got}" != "${want}" ]]; then
        echo "error: checksum mismatch for $(basename "${url}")" >&2
        echo "  expected ${want}" >&2
        echo "  got      ${got}" >&2
        exit 1
    fi
}

fetch "${SOURCE_URL}" "${work}/rnnoise.tar.gz" "${SOURCE_SHA512}"
fetch "${MODEL_URL}" "${work}/model.tar.gz" "${MODEL_SHA512}"

tar -xzf "${work}/rnnoise.tar.gz" -C "${work}"
# The weights ship separately from the code and unpack into their own src/, which
# belongs on top of the source tree's. Without this the library builds and does
# nothing, because the model it would run is not there.
tar -xzf "${work}/model.tar.gz" -C "${work}"
cp -a "${work}/src/." "${work}/rnnoise-v${VERSION}/src/"

cd "${work}/rnnoise-v${VERSION}"
autoreconf -isf
# --with-pic and -fPIC: the static archive is linked into a position-independent
# executable, which is the default everywhere Sonero builds. Without it the link
# fails with a relocation error that says nothing about PIC.
./configure --prefix="${PREFIX}" \
            --disable-shared --enable-static --with-pic --enable-x86-rtcd \
            CFLAGS="-O2 -fPIC"
make -j"$(nproc)"

# Created first: testing a prefix for writability before it exists always says
# no, which would send a plain local install through sudo for no reason.
mkdir -p "${PREFIX}" 2>/dev/null || true
if [[ -w "${PREFIX}" ]] || [[ "${EUID}" -eq 0 ]]; then
    make install
else
    sudo make install
fi

echo
echo "RNNoise ${VERSION} installed under ${PREFIX}"
echo "Configure Sonero with: PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig cmake -S . -B build"
