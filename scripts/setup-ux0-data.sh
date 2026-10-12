#!/usr/bin/env bash
# Wire PAL RMCP01 disc + guest blobs into ux0:data/viicompiled without copying
# Nintendo bytes into the VPK or the git tree.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dump="${MKW_DUMP:-${root}/mariokartwii}"
files="${dump}/DATA/files"
sys="${dump}/DATA/sys"

if [[ ! -d "${files}" || ! -f "${sys}/fst.bin" ]]; then
  echo "setup-ux0-data: expected WIT extract at ${dump}/DATA/files and ${sys}/fst.bin" >&2
  exit 1
fi

vita3k_data=""
for candidate in \
  "${HOME}/Library/Application Support/Vita3K/Vita3K/fs/ux0/data" \
  "${HOME}/Library/Application Support/Vita3K/Vita3K/ux0/data" \
  "${HOME}/Library/Application Support/Vita3K/ux0/data"; do
  if [[ -d "${candidate}" ]]; then
    vita3k_data="${candidate}"
    break
  fi
done

setup_tree() {
  local dest="$1"
  mkdir -p "${dest}/viicompiled"
  ln -sfn "${files}" "${dest}/viicompiled/disc"
  ln -sfn "${sys}" "${dest}/viicompiled/sys"

  if [[ -d "${dest}/mkwv" && ! -L "${dest}/mkwv" ]]; then
    mkdir -p "${dest}/viicompiled/guest" "${dest}/viicompiled/overlay"
    if [[ -d "${dest}/mkwv/guest" ]]; then
      cp -R "${dest}/mkwv/guest/." "${dest}/viicompiled/guest/"
    fi
    if [[ -d "${dest}/mkwv/overlay" ]]; then
      cp -R "${dest}/mkwv/overlay/." "${dest}/viicompiled/overlay/"
    fi
    shopt -s nullglob
    for gxp in "${dest}/mkwv/"*.gxp "${dest}/mkwv/boot.log"; do
      if [[ -e "${gxp}" ]]; then
        mv -f "${gxp}" "${dest}/viicompiled/"
      fi
    done
    shopt -u nullglob
    rm -rf "${dest}/mkwv"
  fi

  ln -sfn viicompiled "${dest}/mkwv"
}

echo "dump files -> ${files}"
echo "dump sys   -> ${sys}"

local_dest="${root}/ux0-data"
setup_tree "${local_dest}"
echo "project ux0-data: ${local_dest}/viicompiled"
echo "  disc -> $(readlink "${local_dest}/viicompiled/disc")"
echo "  sys  -> $(readlink "${local_dest}/viicompiled/sys")"

if [[ -n "${vita3k_data}" ]]; then
  setup_tree "${vita3k_data}"
  echo "Vita3K ux0:data: ${vita3k_data}/viicompiled"
  echo "  disc -> $(readlink "${vita3k_data}/viicompiled/disc")"
  echo "  sys  -> $(readlink "${vita3k_data}/viicompiled/sys")"
else
  echo "Vita3K ux0/data not found; created ${local_dest} only"
fi

guest_src="${root}/upstream/generated/data_sections_init_blobs"
if [[ -f "${guest_src}/StaticR.bin" ]]; then
  "${root}/scripts/install-guest-data.sh" "${local_dest}/viicompiled/guest"
  if [[ -n "${vita3k_data}" ]]; then
    "${root}/scripts/install-guest-data.sh" "${vita3k_data}/viicompiled/guest"
  fi
fi

echo "Vita path: ux0:data/viicompiled (compat symlink ux0:data/mkwv)"
ls -ld "${local_dest}/viicompiled/disc/opening.bnr" "${local_dest}/viicompiled/sys/fst.bin"
