#!/usr/bin/env bash
#
# Builds a .deb for another distribution, inside a container.
#
#   ./packaging/deb/build-deb-docker.sh [image] [output_dir]
#
#   ./packaging/deb/build-deb-docker.sh                    # Ubuntu 24.04 / Mint 22
#   ./packaging/deb/build-deb-docker.sh ubuntu:22.04       # Ubuntu 22.04 / Mint 21
#   ./packaging/deb/build-deb-docker.sh debian:13
#
# A .deb links against the distribution's own Qt, so it installs only on the
# distribution it was built on. This builds on the target instead of the host,
# then *installs the result in a clean container* to prove apt accepts it — which
# is the part you cannot check by inspecting the file.
#
# Output goes to dist/<image-tag>/ so builds for different targets coexist.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
readonly IMAGE="${1:-ubuntu:24.04}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarn:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "docker is required"

# Docker normally needs root. Prefer the socket if this user may use it.
DOCKER=(docker)
if ! docker info >/dev/null 2>&1; then
    if sudo -n true 2>/dev/null || [[ -t 0 ]]; then
        DOCKER=(sudo docker)
        warn "using sudo for docker (add yourself to the 'docker' group to avoid this)"
    else
        die "cannot talk to the docker daemon, and sudo would need a password"
    fi
fi

readonly TAG="$(printf '%s' "${IMAGE}" | tr ':/' '--')"
OUTPUT_DIR="${2:-${REPO_ROOT}/dist/${TAG}}"
mkdir -p "${OUTPUT_DIR}"

readonly BUILD_SCRIPT='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config dpkg-dev \
    qt6-base-dev qt6-base-dev-tools libpipewire-0.3-dev file ca-certificates \
    libgl-dev libegl-dev \
    >/dev/null
mkdir -p /work
tar -x -C /work
cd /work
./packaging/deb/build-deb.sh /work/out >/dev/null
cp /work/out/*.deb /out/
chown "${HOST_UID}:${HOST_GID}" /out/*.deb
'

log "building for ${IMAGE}"
# Stream the working tree in rather than bind-mounting it: build directories are
# large, and a read-only mount would still leave root-owned files behind.
tar -c \
    --exclude=./.git \
    --exclude=./build \
    --exclude='./build-*' \
    --exclude=./dist \
    --exclude='./packaging/appimage/.tools' \
    -C "${REPO_ROOT}" . \
| "${DOCKER[@]}" run --rm -i \
    -v "${OUTPUT_DIR}:/out" \
    -e "HOST_UID=$(id -u)" \
    -e "HOST_GID=$(id -g)" \
    "${IMAGE}" bash -c "${BUILD_SCRIPT}"

package="$(find "${OUTPUT_DIR}" -maxdepth 1 -name '*.deb' -newermt '-10 minutes' | head -1)"
[[ -n "${package}" ]] || die "no package produced"
log "built: ${package}"

# --- verify: does apt actually accept it on a clean system? -------------------
readonly VERIFY_SCRIPT='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# Installing the file lets apt resolve the dependencies it declares.
apt-get install -y -qq /out/'"$(basename "${package}")"' >/dev/null
echo "  installed: $(dpkg-query -W -f=${Version} sonero)"
echo "  files:     $(dpkg -L sonero | grep -c .)"
missing="$(ldd /usr/bin/Sonero | grep "not found" || true)"
if [ -n "${missing}" ]; then
    echo "  MISSING LIBRARIES:"; echo "${missing}"; exit 1
fi
echo "  libraries: all resolved"
test -f /usr/lib/udev/rules.d/70-sonero-steelseries.rules \
    && echo "  udev rule: installed"
test -f /usr/share/applications/Sonero.desktop \
    && echo "  desktop:   installed"
echo "  tray icon: $(find /usr/share/icons -name "Sonero-tray.png" | wc -l) sizes"
'

log "verifying the package installs on a clean ${IMAGE}"
"${DOCKER[@]}" run --rm -v "${OUTPUT_DIR}:/out:ro" "${IMAGE}" bash -c "${VERIFY_SCRIPT}"

echo
log "done"
printf '    %s\n' "${package}"
printf '    size %s\n' "$(du -h "${package}" | cut -f1)"
"${DOCKER[@]}" run --rm -v "${OUTPUT_DIR}:/out:ro" "${IMAGE}" \
    dpkg-deb -f "/out/$(basename "${package}")" Depends \
    | fold -s -w 68 | sed 's/^/    /'
