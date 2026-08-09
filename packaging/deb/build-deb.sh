#!/usr/bin/env bash
#
# Builds Sonero as a Debian package.
#
#   ./packaging/deb/build-deb.sh [output_dir]
#
# Unlike the AppImage, a .deb installs system-wide and can do the things a
# portable bundle cannot: it drops the udev rule into /usr/lib/udev/rules.d (so
# SteelSeries headsets work without any manual step) and lets apt pull in Qt,
# PipeWire and the rest.
#
# The dependency list is derived from the linked binary by dpkg-shlibdeps, so it
# matches whatever this machine built against. Build on the OLDEST distribution
# you intend to support: the resulting package requires at least those versions.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
readonly BUILD_DIR="${REPO_ROOT}/build-deb"
OUTPUT_DIR="${1:-${REPO_ROOT}/dist}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v cmake >/dev/null || die "cmake is required"
command -v dpkg-shlibdeps >/dev/null || die "dpkg-dev is required (apt install dpkg-dev)"

log "configuring (Release)"
# spdlog is deliberately off: its Debian package name encodes both its own and
# libfmt's ABI (libspdlog1.15-fmt10) and differs on every distribution, so linking
# it would tie this package to the exact machine that built it.
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DSONAR_USE_SPDLOG=OFF >/dev/null

log "building"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)" >/dev/null

log "packaging"
rm -f "${BUILD_DIR}"/*.deb
(cd "${BUILD_DIR}" && cpack -G DEB >/dev/null)

mkdir -p "${OUTPUT_DIR}"
package="$(find "${BUILD_DIR}" -maxdepth 1 -name '*.deb' | head -1)"
[[ -n "${package}" ]] || die "cpack produced no package"
mv -f "${package}" "${OUTPUT_DIR}/"
output="${OUTPUT_DIR}/$(basename "${package}")"

log "done: ${output}"
printf '    size %s\n' "$(du -h "${output}" | cut -f1)"
printf '    %s\n' "$(dpkg-deb -f "${output}" Depends | fold -s -w 70 | sed '2,$s/^/    /')"
echo
echo "Install with:  sudo apt install ${output}"
