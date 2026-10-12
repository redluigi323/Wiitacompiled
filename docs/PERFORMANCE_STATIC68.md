# static68: native engine routines, memory inlining removed

The user reports that static66/static67 still run around the original 14–18 fps.
This round targets whole engine functions instead of making translated memory
helpers larger. The goal remains 60 fps; no device FPS improvement is established
by the host tests below.

## Implementation

`engine_native.cpp` replaces 38 SDK/NW4R functions with native C++:

- 3x4 matrix identity, copy, concatenation, concatenation arrays, inverse,
  inverse transpose, translation and scale; 3x3 identity and extraction.
- Vector transforms (including normals), add, cross product, scaling,
  normalization, magnitude, dot product and squared distance.
- NW4R table-based sine, cosine, combined sine/cosine and XYZ rotation.
- Uncompressed F96 and compressed I48/I32 Hermite animation tracks.

Matrix data is bound to RAM once per operation, bulk converted from big-endian
using NEON, and processed with native float arithmetic. Matrix concatenation
uses NEON row operations. Animation uses a bounded binary search and direct
channel decoding rather than translated quantized loads, register updates,
stack spills and per-instruction arithmetic helpers. This is a numerical
replacement: ordinary finite results use a tested relative/absolute float
tolerance; it is not a bit-exact PPC replay implementation.

Original guest trig tables and compressed-channel quantization are retained.
Unsupported memory ranges, unexpected GQR formats, invalid channel counts,
unsupported trig ranges, singular/near-singular inverse matrices, zero vector
normalization, and overlapping matrix arrays return to the original routines.
A rejected native call leaves context and RAM unchanged.

The defining functions in four generated shards are renamed in **build copies**;
all original call sites and indirect dispatch entries keep their names and
therefore bind to native wrappers. This catches same-shard calls too. Simply
registering these functions with the HLE dispatcher would miss those calls.
The translator's original sources and binary source archive are untouched.
Separate statefree specializations are not rewritten by this mechanism.

Forced memory and hot-helper inlining are both removed. The force-included
`shard_hot_inline.h` header is deleted; compare/FP and memory helpers follow
ordinary compiler policy. `MKWV_SHARD_HOT_INLINE`, `MKWV_SHARD_MEMORY_INLINE`
and `_ALL` are forced OFF in CMake, even if an old configure command requests
ON. The removed static67 hot-shard split is no longer used.

## Build

One full rebuild is required to remove the old forced-inline code and bind
these wrappers. Do not use FORCE or timestamp tricks for that first build.
The agent compiled the edited native objects and checked the derived shards'
syntax, but left the full shard compilation, final link and VPK packaging to
the user as requested.

```sh
cd Wiitacompiled
export VITASDK=/usr/local/vitasdk
cmake -S . -B build-vita -DMKWV_BUILD_VITA=ON \
  -DMKWV_SHARD_HOT_INLINE=OFF \
  -DMKWV_SHARD_MEMORY_INLINE=OFF -DMKWV_SHARD_MEMORY_INLINE_ALL=OFF
cmake --build build-vita --target wiicompiled-vita.vpk-vpk -j3
```

Install `build-vita/vita/wiicompiled-vita.vpk` only after the command succeeds.
Check that `boot.log` contains `static68`, `native_engine=1`, `memory_inline=0`.

Later edits to native implementations can use:

```sh
JOBS=3 scripts/build-vita-fast.sh
```

The fast script reads CMake's current object manifest, ignoring stale objects
from old source paths and removed targets. It refuses when a current object
is missing, or when the engine catalog, shard preparation script, shard
compiler flags, or shard ABI headers are newer than the oldest current object.
Changing the catalog requires another correct full build.

## Same-VPK hardware comparison

Native engine routines are ON by default. Create the empty marker
`ux0:data/viicompiled/no_native_engine` and restart to run original PPC routines.
Delete it and restart to restore native code. Keep the same race, camera,
quality markers and warm-up interval for both runs.

PERF lines now append `eng: native=N ppc=M`: wrapper call totals since the
previous report (normally about two seconds), **not calls per frame**. With
native disabled the native count should be zero. With it enabled the native
count should rise; PPC counts show guarded fallbacks. Existing game/render/
worker/sound CPU times remain the main measurements.

Keep the usual quality settings for the first comparison to isolate these
changes. `perf_turbo` is still available separately: 0.75 EFB scale, effects
off, 35% AX budget, reverb and sampling off. It does not enable frame skipping.
Retain boot/watchdog logs and check menus, geometry, bone animation, camera
movement, collisions and audio through a sustained race.

## Validation

- 3,040 differential cases (38 routines x 80) against independent routines
  extracted from the original translated source archive. Includes in-place
  aliases, fractional/boundary animation times, compressed value/slope decoding,
  signed rotations, paired-single scalar returns and nonvolatile FPR sentinels.
- Guard tests verify unchanged context/RAM for rejected calls, including invalid
  addresses, singular matrices, zero normalization, quantization mismatches,
  overlapping arrays, invalid animation counts and the disabling switch.
- All 18 fixture-independent host tests pass. The existing translated ISA test
  is excluded because its NAND fixture is unavailable.
- Vita cross-compilation succeeds for engine_native.cpp, hle_native.cpp and
  main.cpp. All four derived shards pass the Vita compiler's syntax check.
  The native engine object is 11,524 bytes of text and contains NEON byte swaps,
  vector loads/stores and matrix arithmetic.
- Vita configuration generates 38 bindings. A fast-build attempt correctly
  refuses because the new defining-shard objects have not been built yet.

Final link, new VPK installation, hardware correctness and FPS remain pending
user testing. A measured comparison must determine how much of the remaining
CPU budget these replacements remove before choosing the next game subsystem.
