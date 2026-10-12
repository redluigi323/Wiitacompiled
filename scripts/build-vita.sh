#!/usr/bin/env bash
# Configure and do a full Vita build (translated shards included), then package the VPK.
#   JOBS=N        parallel compile jobs (default: CPU threads, capped by RAM)
#   BUILD_DIR=d   build folder (default: build-vita)
# Extra arguments go to the CMake configure step, e.g. -DMKWV_SHARD_OPT=-Os
set -euo pipefail
. "$(dirname "$0")/lib/host.sh"
refuse_native_windows

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"
BUILD_DIR="${BUILD_DIR:-build-vita}"
jobs="${JOBS:-$(host_shard_jobs)}"

if [[ -z "${VITASDK:-}" && -x /usr/local/vitasdk/bin/arm-vita-eabi-gcc ]]; then
  export VITASDK=/usr/local/vitasdk
fi
[[ -n "${VITASDK:-}" && -x "${VITASDK}/bin/arm-vita-eabi-gcc" ]] || {
  echo "vitasdk not found: install it and export VITASDK (see BUILD.md)" >&2; exit 1; }
export PATH="${VITASDK}/bin:${PATH}"
require_cmd cmake "install CMake 3.16+"
require_cmd python3 "install Python 3"

[[ -f third_party/vitaGL/source/vitaGL.h ]] || { echo "run scripts/fetch-deps.sh first" >&2; exit 1; }
[[ -f upstream/generated/build_shards/shards.cmake ]] || {
  echo "no translated game yet: run scripts/translate-game.sh first" >&2; exit 1; }

case "${root}" in
  /mnt/[a-zA-Z]/*)
    if [[ "$(host_os)" == wsl ]]; then
      echo "note: this checkout is on a Windows drive. Builds are several times faster from the WSL" >&2
      echo "      filesystem (e.g. ~/Wiitacompiled). Continuing anyway." >&2
    fi ;;
esac

echo "== configure (${BUILD_DIR})"
cmake -S . -B "${BUILD_DIR}" -D MKWV_BUILD_VITA=ON "$@"
echo "== build with ${jobs} jobs (the first build compiles ~74 large shards; this takes a while)"
cmake --build "${BUILD_DIR}" --target wiicompiled-vita.vpk-vpk -j"${jobs}"
echo "VPK: ${BUILD_DIR}/vita/wiicompiled-vita.vpk"
