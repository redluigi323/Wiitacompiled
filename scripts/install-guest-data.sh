#!/usr/bin/env bash
# Copy relocated RMCP01 DOL/REL section blobs to Vita3K ux0 (not into the VPK).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="${root}/upstream/generated/data_sections_init_blobs"
if [[ ! -f "${src}/StaticR.bin" ]]; then
  echo "install-guest-data: run the translator generate-data-init first (${src} missing)" >&2
  exit 1
fi

dest="${1:-}"
if [[ -z "${dest}" ]]; then
  for candidate in \
    "${HOME}/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/viicompiled/guest" \
    "${HOME}/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/mkwv/guest" \
    "${HOME}/Library/Application Support/Vita3K/Vita3K/ux0/data/mkwv/guest" \
    "${HOME}/Library/Application Support/Vita3K/ux0/data/mkwv/guest"; do
    if [[ -d "$(dirname "$(dirname "$(dirname "${candidate}")")")" ]]; then
      dest="${candidate}"
      break
    fi
  done
fi
if [[ -z "${dest}" ]]; then
  dest="${root}/vita-guest-data"
fi

mkdir -p "${dest}"
cp -f "${src}/"*.bin "${dest}/"
echo "copied guest blobs to ${dest}"
echo "Vita3K path to use: ux0:data/viicompiled/guest"
ls -lh "${dest}/StaticR.bin" "${dest}/_init.bin"
