#!/usr/bin/env bash
#
# Copy a package from this repository into its AUR repository and commit it.
#
#   ./packaging/aur/sync-to-aur.sh sonero
#   ./packaging/aur/sync-to-aur.sh sonero-git --push
#
# The AUR is a separate git remote per package, holding only the PKGBUILD, the
# .SRCINFO and the install scriptlet — so these files live here, where they are
# reviewed alongside the code, and get copied there.
#
# Without --push the commit is left for you to inspect and push by hand, which
# is the sane default for the one repository whose main branch is what users
# install. The push needs an AUR account with your SSH key registered:
# https://aur.archlinux.org/account/

set -euo pipefail

PKG="${1:-}"
PUSH="${2:-}"
case "${PKG}" in
    sonero|sonero-git) ;;
    *)
        echo "usage: $(basename "$0") <sonero|sonero-git> [--push]" >&2
        exit 2
        ;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/${PKG}"
REMOTE="ssh://aur@aur.archlinux.org/${PKG}.git"

if [[ ! -f ${SRC}/.SRCINFO ]]; then
    echo "error: ${PKG}/.SRCINFO is missing — run ./packaging/aur/gen-srcinfo.sh" >&2
    exit 1
fi

# .SRCINFO is generated, so a stale one means someone edited the PKGBUILD and
# forgot. Catch it here rather than in a user's yay output.
if command -v makepkg >/dev/null 2>&1; then
    if ! diff -q <(cd "${SRC}" && makepkg --printsrcinfo) "${SRC}/.SRCINFO" >/dev/null; then
        echo "error: ${PKG}/.SRCINFO does not match its PKGBUILD — run ./packaging/aur/gen-srcinfo.sh" >&2
        exit 1
    fi
fi

work="$(mktemp -d)"

echo "cloning ${REMOTE}"
git clone --quiet "${REMOTE}" "${work}/${PKG}"

# -L dereferences sonero-git's symlink to the shared install scriptlet: an AUR
# repository has to stand on its own, with no path pointing outside it.
cp -L "${SRC}/PKGBUILD" "${SRC}/.SRCINFO" "${work}/${PKG}/"
if compgen -G "${SRC}/*.install" >/dev/null; then
    cp -L "${SRC}"/*.install "${work}/${PKG}/"
fi

cd "${work}/${PKG}"
git add -A
if git diff --cached --quiet; then
    echo "nothing to publish — the AUR already has this."
    exit 0
fi

version="$(awk -F= '/^pkgver=/{print $2}' PKGBUILD)"
git commit --quiet -m "${PKG} ${version}"
git --no-pager show --stat HEAD

if [[ ${PUSH} == "--push" ]]; then
    git push origin master
    rm -rf "${work}"
    echo "published: https://aur.archlinux.org/packages/${PKG}"
else
    # Kept on purpose: the commit is there to be read before it becomes what
    # every `yay -S ${PKG}` downloads.
    echo
    echo "Committed but not pushed. To publish:"
    echo "  cd ${work}/${PKG} && git push origin master"
    echo "Delete ${work} when you are done, or re-run with --push."
fi
