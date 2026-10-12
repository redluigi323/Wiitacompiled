#!/usr/bin/env bash
# Clone upstream WiiCompiled and the vitaGL stack at the commits this tree was built against,
# then apply the small local patches in patches/. Safe to run again.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

fetch() { # dir url commit
  local dir="$1" url="$2" commit="$3"
  if [[ ! -d "${dir}/.git" ]]; then
    git init -q "${dir}"
    git -C "${dir}" remote add origin "${url}"
  fi
  if [[ "$(git -C "${dir}" rev-parse HEAD 2>/dev/null || true)" != "${commit}" ]]; then
    git -C "${dir}" fetch -q --depth 1 origin "${commit}"
    git -C "${dir}" checkout -q --force FETCH_HEAD
  fi
  echo "$(basename "${dir}"): $(git -C "${dir}" log -1 --oneline)"
}

patch_dep() { # dir patch
  local dir="$1" patch="$2"
  if git -C "${dir}" apply --reverse --check "${patch}" 2>/dev/null; then
    echo "  $(basename "${patch}") already applied"
    return
  fi
  if ! git -C "${dir}" apply --check "${patch}" 2>/dev/null && ! git -C "${dir}" diff --quiet; then
    # An older version of the patch is applied (the patch grew since this checkout was made).
    # The pinned dependency carries no other local changes, so start it from clean.
    echo "  $(basename "${dir}") has an older patch applied; resetting it to the pinned commit"
    git -C "${dir}" checkout -q -- .
  fi
  git -C "${dir}" apply "${patch}"
  echo "  applied $(basename "${patch}")"
}

fetch "${root}/upstream"             https://github.com/patchzyy/Wiicompiled.git       7e6604c4157d3a6f937d1a03ac9818fd85bba0a9
fetch "${root}/third_party/vitaGL"    https://github.com/Rinnegatamante/vitaGL.git     16fe309d87761112b813be77842b20338659c460
fetch "${root}/third_party/vitaShaRK" https://github.com/Rinnegatamante/vitaShaRK.git  df24065e65098b2d1ac533760109ad4367573f28
fetch "${root}/third_party/math-neon" https://github.com/Rinnegatamante/math-neon.git  0faab814782c071ff4015527f1ca955ab1ccc470

patch_dep "${root}/third_party/vitaGL"    "${root}/patches/vitaGL.patch"
patch_dep "${root}/third_party/vitaShaRK" "${root}/patches/vitaShaRK.patch"
