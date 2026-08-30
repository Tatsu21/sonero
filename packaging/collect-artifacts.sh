#!/usr/bin/env bash
#
# Gathers the packages a CI run produced into one directory, under names that do
# not collide.
#
#   ./packaging/collect-artifacts.sh <artifacts-dir> <output-dir>
#
# The download step keeps each artifact in its own directory, because it has to:
# every AppImage is called Sonero-<version>-x86_64.AppImage, and both .deb files
# are sonero_<version>_amd64.deb. Merging them blindly is silent data loss, so
# this renames as it flattens.
#
# Shared by the release and the nightly job — two copies of this would drift, and
# a drift here publishes mislabelled binaries.

set -euo pipefail

ARTIFACTS="${1:?usage: collect-artifacts.sh <artifacts-dir> <output-dir>}"
OUT="${2:?usage: collect-artifacts.sh <artifacts-dir> <output-dir>}"

mkdir -p "${OUT}"

# Both runners emit sonero_<version>_amd64.deb. Fold the target into the version
# part rather than the middle of the name, so the file keeps the
# name_version_arch shape Debian tooling expects.
for dir in "${ARTIFACTS}"/Sonero-mint*; do
    [[ -d "${dir}" ]] || continue
    target="${dir##*-}"  # Sonero-mint21-ubuntu2204 -> ubuntu2204
    for f in "${dir}"/*.deb; do
        [[ -f "${f}" ]] || continue
        cp "${f}" "${OUT}/$(basename "${f}" _amd64.deb)-${target}_amd64.deb"
    done
done

# Built on the oldest glibc, so it runs on the most systems: this is the one to
# download, and it keeps the plain name.
for f in "${ARTIFACTS}"/Sonero-mint21-ubuntu2204/*.AppImage; do
    [[ -f "${f}" ]] || continue
    cp "${f}" "${OUT}/"
done

# The rolling-distribution builds need their distribution in the name.
for dir in "${ARTIFACTS}"/Sonero-arch "${ARTIFACTS}"/Sonero-fedora; do
    [[ -d "${dir}" ]] || continue
    distro="${dir##*-}"
    for f in "${dir}"/*.AppImage; do
        [[ -f "${f}" ]] || continue
        cp "${f}" "${OUT}/$(basename "${f}" -x86_64.AppImage)-${distro}-x86_64.AppImage"
    done
done

ls -l "${OUT}"
