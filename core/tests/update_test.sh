#!/usr/bin/env bash
# tools/fujinet/update-fujinet-firmware.sh in a scratch copy of the tree:
# from an older pin it moves all three pin sites to master; with a patch that
# no longer applies it keeps the old pin. Needs the network (SKIPs, 77).
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
URL=$(sed -n 's/^set(FUJINET_URL "\(.*\)")$/\1/p' "${ROOT}/cmake/Dependencies.cmake")
HEAD=$(timeout 15 git ls-remote "${URL}" refs/heads/master 2>/dev/null | cut -f1)
[[ -n "${HEAD}" ]] || { echo "SKIP: ${URL} unreachable"; exit 77; }
OLD=$(git -C "${ROOT}/third_party/fujinet-firmware" rev-parse "${HEAD}~3" 2>/dev/null) \
    || { echo "SKIP: no local history for ${HEAD:0:9}"; exit 77; }

TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

setup() {
    rm -rf "${TMP}/tree"
    mkdir -p "${TMP}/tree/cmake" "${TMP}/tree/build-aux/flatpak" "${TMP}/tree/third_party"
    cp -R "${ROOT}/tools" "${TMP}/tree/tools"
    rm -rf "${TMP}/tree/tools/fujinet/work"
    sed "s/^set(FUJINET_COMMIT \".*\")$/set(FUJINET_COMMIT \"${OLD}\")/" \
        "${ROOT}/cmake/Dependencies.cmake" > "${TMP}/tree/cmake/Dependencies.cmake"
    for f in gnome kde; do
        printf 'sources:\n  - type: git\n    url: %s\n    commit: %s\n' "${URL}" "${OLD}" \
            > "${TMP}/tree/build-aux/flatpak/test.${f}.yml"
    done
    git clone --quiet --filter=blob:none --no-checkout "${URL}" "${TMP}/tree/third_party/fujinet-firmware"
    git -C "${TMP}/tree/third_party/fujinet-firmware" -c advice.detachedHead=false checkout -q "${OLD}"
}

pins_are() {
    grep -q "set(FUJINET_COMMIT \"$1\")" "${TMP}/tree/cmake/Dependencies.cmake" || return 1
    grep -q "commit: $1" "${TMP}/tree/build-aux/flatpak/test.gnome.yml" || return 1
    grep -q "commit: $1" "${TMP}/tree/build-aux/flatpak/test.kde.yml" || return 1
}

setup
bash "${TMP}/tree/tools/fujinet/update-fujinet-firmware.sh" || fail "update"
pins_are "${HEAD}" || fail "pins not moved to ${HEAD:0:9}"
[[ "$(git -C "${TMP}/tree/third_party/fujinet-firmware" rev-parse HEAD)" == "${HEAD}" ]] \
    || fail "clone not moved"

setup
printf -- '--- a/build.sh\n+++ b/build.sh\n@@ -1,1 +1,1 @@\n-this line is not there\n+nor this\n' \
    > "${TMP}/tree/tools/fujinet/patches/9999-broken.patch"
if bash "${TMP}/tree/tools/fujinet/update-fujinet-firmware.sh" 2>/dev/null; then
    fail "a broken patch was accepted"
fi
pins_are "${OLD}" || fail "pins moved despite a broken patch"
bash "${TMP}/tree/tools/fujinet/update-fujinet-firmware.sh" --auto || fail "--auto must not fail"
pins_are "${OLD}" || fail "--auto moved the pins"
echo "update: ok"
