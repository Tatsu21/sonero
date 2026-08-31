#!/usr/bin/env bash
#
# Renders og.png (the Open Graph card) from og.svg.
#
# It has to happen in a scratch directory: the images beside og.svg are symlinks
# pointing outside page/assets, and librsvg refuses to follow references that
# leave the SVG's own directory. Rendering in place silently produces a card with
# the artwork missing — which looks fine in a file listing and wrong everywhere
# the link is shared.

set -euo pipefail
readonly HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

cp -L "${HERE}/og.svg" "${HERE}/mixer.png" "${HERE}/sonero-128.png" "${TMP}/"
rsvg-convert -w 1200 -h 630 "${TMP}/og.svg" -o "${HERE}/og.png"
echo "wrote ${HERE}/og.png"
