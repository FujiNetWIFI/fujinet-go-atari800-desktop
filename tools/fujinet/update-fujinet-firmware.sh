#!/usr/bin/env bash
#
# Move the embedded FujiNet to the newest fujinet-firmware commit, if every
# patch still applies to it.
#
#   update-fujinet-firmware.sh [--auto] [ref]     (ref: default master)
#
# Checks the remote; if it has moved, stages the new commit and runs
# build-fujinet-desktop.sh's patching against it (FN_PATCH_ONLY). If that
# succeeds, checks out the commit in third_party/fujinet-firmware and rewrites
# all three pin sites -- cmake/Dependencies.cmake and both flatpak manifests
# -- for you to commit. If a patch no longer applies, the old pin stays and
# the failure is printed; the build carries on with it.
#
# --auto (how the build runs it): at most once per 10 minutes, and never fails.

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd)
DEPS="${ROOT_DIR}/cmake/Dependencies.cmake"
CLONE="${ROOT_DIR}/third_party/fujinet-firmware"
STAMP="${SCRIPT_DIR}/work/.update-checked"
MANIFESTS=("${ROOT_DIR}"/build-aux/flatpak/*.yml)

AUTO=0
if [[ "${1:-}" == "--auto" ]]; then
    AUTO=1
    shift
fi
REF="${1:-master}"

note() { echo "update-fujinet-firmware: $*" >&2; }
give_up() {
    note "$*"
    [[ ${AUTO} -eq 1 ]] && exit 0
    exit 1
}

if [[ ${AUTO} -eq 1 && -f "${STAMP}" ]]; then
    now=$(date +%s)
    then=$(stat -c %Y "${STAMP}" 2>/dev/null || stat -f %m "${STAMP}" 2>/dev/null || echo 0)
    (( now - then < 600 )) && exit 0
fi

URL=$(sed -n 's/^set(FUJINET_URL "\(.*\)")$/\1/p' "${DEPS}")
OLD=$(sed -n 's/^set(FUJINET_COMMIT "\([0-9a-f]*\)")$/\1/p' "${DEPS}")
[[ -n "${URL}" && -n "${OLD}" ]] || give_up "no FUJINET_URL / FUJINET_COMMIT in ${DEPS}"

ls_remote() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 15 git ls-remote "${URL}" "refs/heads/${REF}"
    else
        git ls-remote "${URL}" "refs/heads/${REF}"
    fi
}
NEW=$(ls_remote 2>/dev/null | cut -f1)
[[ -n "${NEW}" ]] || give_up "could not reach ${URL} (offline?); staying on ${OLD:0:9}"
mkdir -p "$(dirname "${STAMP}")"
touch "${STAMP}"
if [[ "${NEW}" == "${OLD}" ]]; then
    [[ ${AUTO} -eq 1 ]] || note "already at ${REF} (${OLD:0:9})"
    exit 0
fi

note "${REF} moved: ${OLD:0:9} -> ${NEW:0:9}; checking the patches against it"
if [[ ! -f "${CLONE}/build.sh" ]]; then
    git clone --quiet --filter=blob:none "${URL}" "${CLONE}" || give_up "could not clone ${URL}"
fi
if [[ -n "$(git -C "${CLONE}" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then
    give_up "${CLONE} has local changes; staying on ${OLD:0:9}"
fi
git -C "${CLONE}" fetch --quiet origin "${REF}" 2>/dev/null \
    || git -C "${CLONE}" fetch --quiet origin 2>/dev/null \
    || give_up "could not fetch ${REF}"
git -C "${CLONE}" cat-file -e "${NEW}^{commit}" 2>/dev/null || give_up "fetched, but ${NEW:0:9} is missing"

TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${TMP}/src"
git -C "${CLONE}" archive "${NEW}" | tar -x -C "${TMP}/src" || give_up "could not export ${NEW:0:9}"
if ! FUJINET_SRC="${TMP}/src" FN_WORK_ROOT="${TMP}/work" FN_PATCH_ONLY=1 \
        bash "${SCRIPT_DIR}/build-fujinet-desktop.sh" > "${TMP}/patch.log" 2>&1; then
    note "a patch no longer applies to ${NEW:0:9}; staying on ${OLD:0:9}:"
    tail -n 20 "${TMP}/patch.log" >&2
    [[ ${AUTO} -eq 1 ]] && exit 0
    exit 1
fi

# every pin site, exactly once each
replace_once() {
    local file="$1" old="$2" new="$3" n
    n=$(grep -c -F -- "${old}" "${file}")
    [[ "${n}" -eq 1 ]] || { note "${file}: expected one '${old}', found ${n}"; return 1; }
    python3 - "${file}" "${old}" "${new}" <<'PY' || return 1
import sys
p, old, new = sys.argv[1:4]
with open(p, encoding="utf-8", newline="") as f:
    s = f.read()
with open(p, "w", encoding="utf-8", newline="") as f:
    f.write(s.replace(old, new, 1))
PY
}
for m in "${MANIFESTS[@]}"; do
    [[ -f "${m}" ]] || continue
    grep -q -F "commit: ${OLD}" "${m}" || give_up "${m} does not pin ${OLD:0:9}; fix the pins by hand first"
done
replace_once "${DEPS}" "set(FUJINET_COMMIT \"${OLD}\")" "set(FUJINET_COMMIT \"${NEW}\")" \
    || give_up "could not rewrite ${DEPS}"
for m in "${MANIFESTS[@]}"; do
    [[ -f "${m}" ]] || continue
    replace_once "${m}" "commit: ${OLD}" "commit: ${NEW}" || give_up "could not rewrite ${m}"
done
git -C "${CLONE}" -c advice.detachedHead=false checkout --quiet "${NEW}" \
    || give_up "could not check out ${NEW:0:9}"

note "FujiNet now at ${NEW:0:9} (commit the pins: Dependencies.cmake and the flatpak manifests)"
git -C "${CLONE}" log --oneline --no-decorate "${OLD}..${NEW}" 2>/dev/null | head -n 30 >&2
exit 0
