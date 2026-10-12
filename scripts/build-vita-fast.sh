#!/usr/bin/env bash
# Rebuild the Vita VPK without recompiling the translated shards (mkwv_translated).
#
# HOW THIS WORKS, AND WHY IT IS NOT "make <target>/fast":
#
# The previous version of this script used CMake's per-target "/fast" rules. Those skip the
# `depend` step, so a change to a HEADER rebuilds nothing at all - only edited .cpp files are
# recompiled. That silently produced a binary in which some TUs had the old layout of
# gx::Processor and some had the new one, which cost a full debugging round (see handoff.md,
# "Never back-date a header's mtime"). Do not reintroduce it.
#
# Instead this runs a completely normal, dependency-checked build, and protects only the shard
# objects by stamping them newer than every source and header. That is safe because no generated
# shard source references gx::Processor, and sizeof(gx::Fifo) / sizeof(HleFifo) - the only shard-
# visible layouts - depend on kFifoCapacity alone. Every non-shard TU still gets correct
# dependency checking, so an ABI skew like the one above cannot happen again.
#
# If a header the shards genuinely depend on has changed, the script refuses and asks for a full
# build rather than stamping over a real ABI change. FORCE=1 overrides that, at your own risk.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-build-vita}"
JOBS="${JOBS:-3}"
SHARD_MANIFEST="$BUILD_DIR/vita/translated-objects.txt"
if [ ! -f "$SHARD_MANIFEST" ]; then
  echo "Missing $SHARD_MANIFEST - configure CMake and run the full build first." >&2
  exit 1
fi
SHARD_OBJECTS=()
while IFS= read -r obj; do
  [ -n "$obj" ] || continue
  if [ ! -f "$obj" ]; then
    echo "Missing current shard object $obj - run the full build first." >&2
    exit 1
  fi
  SHARD_OBJECTS+=("$obj")
done < "$SHARD_MANIFEST"
if [ ${#SHARD_OBJECTS[@]} -eq 0 ]; then
  echo "No current shard objects - run the full build first." >&2
  exit 1
fi

# Headers whose contents actually reach the translated shards' ABI. A change to any of these means
# the shards must be recompiled; stamping over it would produce a mismatched binary.
ABI_INPUTS=(
  "$BUILD_DIR/vita/CMakeFiles/mkwv_translated.dir/flags.make"
  scripts/prepare-engine-shards.py
  vita/include/mkwv/runtime/engine_native_catalog.inc
  vita/include/abi_bridge.h
  vita/include/native_cpu_calls.inc
  vita/include/ppc_runtime.h
  vita/include/memory.h
  vita/include/memory_access.h
  vita/include/recomp_mod_loader.h
  vita/include/guest_flat_memory.h
  vita/include/mkwv/runtime/guest_access.hpp
  vita/include/mkwv/runtime/compact_memory.hpp
  vita/include/mkwv/runtime/dispatch.hpp
  # Transitively shard-visible: memory_access.h -> mkwv/runtime/hle_fifo.hpp ->
  # mkwv/gx/fifo.hpp. This one was missing from the guard, and a change to fifo.hpp once
  # skewed Runtime's layout between the shards and main.cpp - image_ moved 753,664 bytes
  # and the guest never started.
  vita/include/mkwv/runtime/hle_fifo.hpp
  vita/include/mkwv/gx/fifo.hpp
  vita/include/mkwv/gx/state.hpp
  vita/include/mkwv/gx/types.hpp
  vita/include/mkwv/gx/texture.hpp
)
OLDEST_OBJ=""
while IFS= read -r obj; do
  if [ -z "$OLDEST_OBJ" ] || [ "$obj" -ot "$OLDEST_OBJ" ]; then
    OLDEST_OBJ="$obj"
  fi
done < <(printf '%s\n' "${SHARD_OBJECTS[@]}")
if [ -z "$OLDEST_OBJ" ]; then
  echo "Could not determine the oldest shard object in $SHARD_MANIFEST" >&2
  exit 1
fi
STALE=()
for h in "${ABI_INPUTS[@]}" vita/include/isa/*.h; do
  [ -e "$h" ] || continue
  if [ "$h" -nt "$OLDEST_OBJ" ]; then
    STALE+=("$h")
  fi
done
if [ ${#STALE[@]} -ne 0 ] && [ "${FORCE:-0}" != "1" ]; then
  echo "These shard-ABI inputs changed since the shards were built:" >&2
  printf '  %s\n' "${STALE[@]}" >&2
  echo "The shards must be rebuilt. Run the full build:" >&2
  echo "  cmake --build $BUILD_DIR --target wiicompiled-vita.vpk-vpk -j$JOBS" >&2
  echo "(FORCE=1 $0 stamps over this - only if you are certain the ABI is unchanged.)" >&2
  exit 1
fi

# Stamp the shard objects as up to date, then build everything else normally.
touch "${SHARD_OBJECTS[@]}"
cmake --build "$BUILD_DIR" --target wiicompiled-vita.vpk-vpk -j"$JOBS"
echo "VPK: $BUILD_DIR/vita/wiicompiled-vita.vpk"
