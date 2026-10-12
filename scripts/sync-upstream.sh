#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${root}/upstream"
url="https://github.com/patchzyy/Wiicompiled.git"

if [[ -d "${dest}/.git" ]]; then
  git -C "${dest}" fetch --depth 1 origin main
  git -C "${dest}" checkout --force origin/main
else
  git clone --depth 1 "${url}" "${dest}"
fi

git -C "${dest}" log -1 --oneline
