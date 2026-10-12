#!/usr/bin/env bash
# Copy the relocated RMCP01 DOL/REL section blobs (translator output) to a guest/ folder.
# Usage: install-guest-data.sh [dest]   (default: Vita3K's ux0:data/viicompiled/guest, else ./vita-guest-data)
set -euo pipefail
. "$(dirname "$0")/lib/host.sh"

root="$(cd "$(dirname "$0")/.." && pwd)"
src="${root}/upstream/generated/data_sections_init_blobs"
if [[ ! -f "${src}/StaticR.bin" ]]; then
  echo "install-guest-data: run scripts/translate-game.sh first (${src} missing)" >&2
  exit 1
fi

dest="${1:-}"
if [[ -z "${dest}" ]]; then
  vita3k_data="$(vita3k_ux0_data)"
  if [[ -n "${vita3k_data}" ]]; then
    dest="${vita3k_data}/viicompiled/guest"
  else
    dest="${root}/vita-guest-data"
  fi
fi

mkdir -p "${dest}"
cp -f "${src}/"*.bin "${dest}/"
echo "copied guest blobs to ${dest} (ux0:data/viicompiled/guest on the Vita)"
