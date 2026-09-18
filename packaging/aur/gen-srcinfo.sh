#!/usr/bin/env bash
#
# Regenerate .SRCINFO next to each PKGBUILD.
#
# The AUR reads only .SRCINFO — the web interface, the dependency solver and
# yay's search all come from it, not from the PKGBUILD. A push where the two
# disagree looks fine in git and wrong to every user, so this is generated,
# never edited.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v makepkg >/dev/null 2>&1; then
    echo "error: makepkg not found — this needs an Arch system (pacman -S base-devel)" >&2
    exit 1
fi

for pkg in sonero sonero-git; do
    # Run from the package directory: makepkg resolves PKGBUILD relative to the
    # working directory, and --printsrcinfo neither builds nor downloads.
    (cd "${HERE}/${pkg}" && makepkg --printsrcinfo > .SRCINFO)
    echo "wrote packaging/aur/${pkg}/.SRCINFO"
done
