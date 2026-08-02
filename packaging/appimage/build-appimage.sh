#!/usr/bin/env bash
#
# Builds LinuxSonar as a self-contained AppImage.
#
#   ./packaging/appimage/build-appimage.sh [output_dir]
#
# What it does:
#   1. downloads linuxdeploy + its Qt plugin + appimagetool (cached in .tools/)
#   2. builds LinuxSonar in Release and installs it into an AppDir
#   3. lets linuxdeploy bundle Qt and the remaining dependencies
#   4. drops libpipewire from the bundle on purpose (see below)
#   5. packs the AppDir into LinuxSonar-<version>-x86_64.AppImage
#
# Why libpipewire is NOT bundled: the client library loads SPA plugins from the
# host (/usr/lib/.../spa-0.2). Shipping our own copy risks pairing it with the
# host's mismatched plugins. LinuxSonar requires a running PipeWire session
# anyway, so the host is guaranteed to provide a matching library.
#
# Env overrides: LINUXDEPLOY_URL, LINUXDEPLOY_QT_URL, APPIMAGETOOL_URL,
#                UPDATE_INFO (embeds update metadata for self-updating builds)

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
readonly TOOLS_DIR="${SCRIPT_DIR}/.tools"
readonly BUILD_DIR="${REPO_ROOT}/build-appimage"
readonly APPDIR="${BUILD_DIR}/AppDir"
OUTPUT_DIR="${1:-${REPO_ROOT}/dist}"

LINUXDEPLOY_URL="${LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage}"
LINUXDEPLOY_QT_URL="${LINUXDEPLOY_QT_URL:-https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage}"
APPIMAGETOOL_URL="${APPIMAGETOOL_URL:-https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage}"

# AppImages of the tools themselves need FUSE; extracting instead works anywhere
# (containers, CI) and is just as correct.
export APPIMAGE_EXTRACT_AND_RUN=1

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

fetch_tool() {
    local url="$1" dest="$2"
    if [[ -x "${dest}" ]]; then
        log "cached: $(basename "${dest}")"
    else
        log "downloading $(basename "${dest}")"
        curl -fL --retry 3 --progress-bar -o "${dest}.part" "${url}" \
            || die "download failed: ${url}"
        chmod +x "${dest}.part"
        mv "${dest}.part" "${dest}"
    fi
    # Print the hash so a build can be pinned/audited later.
    printf '    sha256 %s\n' "$(sha256sum "${dest}" | cut -d' ' -f1)"
}

command -v curl >/dev/null || die "curl is required"
command -v cmake >/dev/null || die "cmake is required"

# --- 0. project version (drives the output file name) ------------------------
VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9.]\+\).*/\1/p' \
    "${REPO_ROOT}/CMakeLists.txt" | head -1)"
[[ -n "${VERSION}" ]] || die "could not read VERSION from CMakeLists.txt"
log "LinuxSonar ${VERSION}"

# --- 1. tools ----------------------------------------------------------------
mkdir -p "${TOOLS_DIR}"
fetch_tool "${LINUXDEPLOY_URL}"    "${TOOLS_DIR}/linuxdeploy"
fetch_tool "${LINUXDEPLOY_QT_URL}" "${TOOLS_DIR}/linuxdeploy-plugin-qt"
fetch_tool "${APPIMAGETOOL_URL}"   "${TOOLS_DIR}/appimagetool"

# --- 2. build + install into the AppDir --------------------------------------
log "configuring (Release)"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF >/dev/null

log "building"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)" >/dev/null

log "installing into AppDir"
rm -rf "${APPDIR}"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}" >/dev/null

# --- 3. bundle dependencies --------------------------------------------------
# The Qt plugin needs to find this system's Qt; qmake6 is the reliable pointer.
if command -v qmake6 >/dev/null; then
    export QMAKE="$(command -v qmake6)"
fi
# Wayland sessions need these beyond the default platform plugins.
export EXTRA_QT_PLUGINS="${EXTRA_QT_PLUGINS:-wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration}"

log "bundling Qt and dependencies"
"${TOOLS_DIR}/linuxdeploy" \
    --appdir "${APPDIR}" \
    --plugin qt \
    --desktop-file "${APPDIR}/usr/share/applications/LinuxSonar.desktop" \
    --icon-file "${REPO_ROOT}/packaging/icons/linuxsonar-256.png" \
    --icon-filename LinuxSonar

# --- 3b. Wayland platform support -------------------------------------------
# linuxdeploy-plugin-qt only ships the xcb platform plugin, which forces the app
# through XWayland on Wayland sessions. Deploy the Wayland plugins explicitly:
# linuxdeploy --library pulls each one *and its dependencies* into usr/lib, then
# we move the plugin itself into the plugin directory Qt actually searches.
deploy_wayland_plugins() {
    local qt_plugins
    qt_plugins="$("${QMAKE:-qmake6}" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    if [[ ! -d "${qt_plugins}" ]]; then
        log "Qt plugin dir not found — skipping Wayland (XWayland will be used)"
        return 0
    fi
    if [[ ! -f "${qt_plugins}/platforms/libqwayland-generic.so" ]]; then
        log "no Qt Wayland platform plugin installed — XWayland will be used"
        return 0
    fi

    local rel=() args=() so name
    for so in "${qt_plugins}"/platforms/libqwayland-*.so; do
        [[ -f "${so}" ]] || continue
        rel+=("platforms/$(basename "${so}")")
        args+=(--library "${so}")
    done
    local dir
    for dir in wayland-decoration-client wayland-graphics-integration-client \
               wayland-shell-integration; do
        for so in "${qt_plugins}/${dir}"/*.so; do
            [[ -f "${so}" ]] || continue
            rel+=("${dir}/$(basename "${so}")")
            args+=(--library "${so}")
        done
    done
    [[ ${#args[@]} -gt 0 ]] || return 0

    log "bundling ${#rel[@]} Wayland plugins (+ their dependencies)"
    "${TOOLS_DIR}/linuxdeploy" --appdir "${APPDIR}" "${args[@]}"

    # Relocate the plugins out of usr/lib into the Qt plugin tree.
    local entry
    for entry in "${rel[@]}"; do
        name="$(basename "${entry}")"
        if [[ -f "${APPDIR}/usr/lib/${name}" ]]; then
            mkdir -p "${APPDIR}/usr/plugins/$(dirname "${entry}")"
            mv -f "${APPDIR}/usr/lib/${name}" "${APPDIR}/usr/plugins/${entry}"
        fi
    done
}
deploy_wayland_plugins

# --- 4. deliberately un-bundle libpipewire -----------------------------------
shopt -s nullglob
pw_removed=0
for lib in "${APPDIR}"/usr/lib/libpipewire-0.3.so*; do
    rm -f "${lib}"
    pw_removed=1
done
shopt -u nullglob
if (( pw_removed )); then
    log "removed bundled libpipewire (host provides it)"
fi

# --- 5. pack -----------------------------------------------------------------
mkdir -p "${OUTPUT_DIR}"
readonly OUTPUT="${OUTPUT_DIR}/LinuxSonar-${VERSION}-x86_64.AppImage"

appimagetool_args=("${APPDIR}" "${OUTPUT}")
if [[ -n "${UPDATE_INFO:-}" ]]; then
    # Enables `--appimage-update`-style delta updates for published releases.
    appimagetool_args=(-u "${UPDATE_INFO}" "${APPDIR}" "${OUTPUT}")
    log "embedding update info: ${UPDATE_INFO}"
fi

log "packing AppImage"
ARCH=x86_64 "${TOOLS_DIR}/appimagetool" "${appimagetool_args[@]}"

chmod +x "${OUTPUT}"
log "done: ${OUTPUT}"
printf '    size   %s\n' "$(du -h "${OUTPUT}" | cut -f1)"
printf '    sha256 %s\n' "$(sha256sum "${OUTPUT}" | cut -d' ' -f1)"
