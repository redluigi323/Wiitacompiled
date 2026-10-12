#!/usr/bin/env bash
# Lay out ux0:data/viicompiled (disc/, sys/, guest/) from your dump and the translator output,
# without copying Nintendo bytes into the VPK or the git tree.
#
# Always builds ./ux0-data/viicompiled (symlinks, for reference or for copying to a Vita).
# If Vita3K is installed it also fills Vita3K's ux0. On macOS/Linux that is symlinks; for a
# Windows Vita3K reached from WSL the files are copied, since Windows can't follow WSL symlinks.
#   MKW_DUMP=dir          dump location (default ./mariokartwii)
#   VITA3K_UX0_DATA=dir   Vita3K's ux0/data folder, if it isn't found on its own
set -euo pipefail
. "$(dirname "$0")/lib/host.sh"
refuse_native_windows

root="$(cd "$(dirname "$0")/.." && pwd)"
dump="${MKW_DUMP:-${root}/mariokartwii}"
files="${dump}/DATA/files"
sys="${dump}/DATA/sys"
guest_src="${root}/upstream/generated/data_sections_init_blobs"

if [[ ! -d "${files}" || ! -f "${sys}/fst.bin" ]]; then
  echo "setup-ux0-data: expected an extracted disc at ${dump}/DATA/files and ${sys}/fst.bin" >&2
  exit 1
fi

link_or_copy() { # src dest-dir
  local src="$1" dest="$2"
  if is_windows_path "${dest}"; then
    [[ -L "${dest}" ]] && rm -f "${dest}"
    mkdir -p "${dest}"
    echo "  copying $(basename "${dest}")/ (Windows can't read WSL symlinks)..."
    cp -R -u "${src}/." "${dest}/"
  else
    ln -sfn "${src}" "${dest}"
  fi
}

setup_tree() { # ux0/data dir
  local dest="$1/viicompiled"
  mkdir -p "${dest}"
  link_or_copy "${files}" "${dest}/disc"
  link_or_copy "${sys}" "${dest}/sys"
  if [[ -f "${guest_src}/StaticR.bin" ]]; then
    bash "${root}/scripts/install-guest-data.sh" "${dest}/guest" >/dev/null
  else
    echo "  guest/ skipped: run scripts/translate-game.sh first" >&2
  fi
  echo "ux0:data/viicompiled -> ${dest}"
}

setup_tree "${root}/ux0-data"

vita3k_data="$(vita3k_ux0_data)"
if [[ -n "${vita3k_data}" ]]; then
  setup_tree "${vita3k_data}"
else
  echo "Vita3K's ux0/data not found (set VITA3K_UX0_DATA if it is installed). Only ./ux0-data was made."
fi
echo "On a real Vita, copy the contents of disc/, sys/ and guest/ into ux0:data/viicompiled/ (see BUILD.md)."
