#!/usr/bin/env bash
#
# Copies the site into page/_site for publishing.
#
#   ./page/build.sh          then open page/_site/index.html
#
# page/assets/ holds symlinks, not copies: the images live once, where the rest of
# the repository already keeps them (docs/images, packaging/icons). That way
# opening page/index.html directly works — the browser follows the links — while
# git stores three tiny link entries instead of a second copy of every screenshot.
#
# `cp -L` resolves them here, so the published site contains real files.

set -euo pipefail

readonly HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly OUT="${1:-${HERE}/_site}"

# A symlink whose target moved would otherwise ship a site with missing images.
missing=0
for link in "${HERE}"/assets/*; do
    if [[ ! -e "${link}" ]]; then
        echo "error: ${link#"${HERE}"/} points at $(readlink "${link}"), which is gone" >&2
        missing=1
    fi
done
[[ "${missing}" -eq 0 ]] || exit 1

rm -rf "${OUT}"
mkdir -p "${OUT}"
cp "${HERE}/index.html" "${HERE}/robots.txt" "${HERE}/sitemap.xml" "${OUT}/"
cp -rL "${HERE}/assets" "${OUT}/"
# Sources, not part of the published site: the card is shipped as og.png.
rm -f "${OUT}/assets/og.svg" "${OUT}/assets/make-og.sh"

# Pages serves the artifact as-is; without this Jekyll would try to process it.
touch "${OUT}/.nojekyll"

echo "site in ${OUT}"
