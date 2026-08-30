#!/usr/bin/env bash
#
# Fails when the version a build resolved is not the one the release tag names.
#
#   ./packaging/check-release-version.sh <version-file> <tag>
#
# The version comes from the latest release tag, so on a release build these must
# agree. When they do not, the tag lookup failed and the build fell back — the
# packages would then be named after a version nobody released.

set -euo pipefail

FILE="${1:?usage: check-release-version.sh <version-file> <tag>}"
TAG="${2:?usage: check-release-version.sh <version-file> <tag>}"

if [[ ! -r "${FILE}" ]]; then
    echo "::error::no version file at ${FILE} — did the configure run?"
    exit 1
fi

resolved="$(tr -d '[:space:]' < "${FILE}")"
expected="${TAG#v}"

echo "resolved '${resolved}', release tag '${TAG}'"
if [[ "${resolved}" != "${expected}" ]]; then
    echo "::error::build resolved version ${resolved}, but the release is ${TAG}." \
         "The tag lookup fell back — check the 'no release tag readable' warning" \
         "in the configure output above."
    exit 1
fi
