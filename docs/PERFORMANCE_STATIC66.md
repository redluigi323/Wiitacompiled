# static66: hardware performance push

Historical round: static68 removes forced memory inlining after the user reported
no useful hardware gain. Use [PERFORMANCE_STATIC68.md](PERFORMANCE_STATIC68.md)
for the current build and native engine comparison.

This round builds on static65. The reported baseline is 14–18 fps in a race;
there is no new device log yet. No 60 fps result is claimed. At roughly 170 ms
of aggregate CPU work per frame, three user cores would need that work below
roughly 50 ms for 60 fps, as well as each serial stage below 16.7 ms.

## Changes

- **Translated RAM accesses:** `MKWV_SHARD_MEMORY_INLINE=ON` force-inlines the
  scalar FlatRead/FlatWrite/FlatWriteRam fast paths. Misses still call the
  checked runtime. RAM-only stores never become FIFO stores; their slow
  fallback is now one out-of-line runtime function. Small FP rounding and
  bitcasts are forced inline too. Guest fast-math remains disabled.
- **Vertex layout preparation:** BP writes no longer retire the prepared
  VCD/VAT/array layout or recompute its 64-bit display-list signature. CP, XF,
  indexed loads, Aurora escapes, display lists and process boundaries still
  retire it. Processed-vertex reuse and per-draw state keep their existing,
  conservative invalidation rules.
- **Cached staging for the GPU ring:** workers pack into the existing cached batch
  arena, then one NEON streaming copy uploads the finished batch to the uncached
  GPU ring. A system memory barrier completes stores before GPU submission.
  GPU lifetime protection stays in place. PERF `rup=` measures upload milliseconds
  per frame separately from worker time. This reuses the existing arena;
  only a 64 KiB GPU-offset table is added. Overwrite protection now also
  covers the first three frames, when a heavy startup batch can wrap early. `direct_vertex_ring` restores the old
  per-vertex uncached writes. Oversized batches fall back to client submission.
- **Cached vertex packing:** the fast path without capture/mirroring restores
  only positions, normals, two texcoords, colours and the matrix index. It
  avoids copying the entire decoded-vertex default structure per vertex.
  Captures and general texgen retain full restoration.
- **Worker completion:** when no unclaimed jobs remain, the renderer's drain
  yields for 50 microseconds instead of spinning above the workers. It still
  helps execute unclaimed jobs.
- **Audio:** NEON byte reversal for parameter-block transfers, and NEON
  four-tap polyphase products with widening 64-bit addition. A duplicated
  four-sample ring supplies contiguous filter windows. Rounding, clipping,
  sample history and coefficient selection remain unchanged.
- **Profiler:** sampling uses a nonblocking lock; reporting copies its
  histograms and releases the lock before sorting or writing the watchdog
  log. Other cores skip a contended sample. Raw/error formatting uses
  thread-local buffers. This removes cross-core high-priority spin waits
  during reports and shared formatting-buffer races.

## Explicit aggressive quality preset

Create an empty `ux0:data/viicompiled/perf_turbo` file and restart:

- EFB scale 0.75 (the existing `efb_lower` setting).
- Effects level 0 (the existing `fx_off` setting).
- AX DSP budget 35% (the existing `snd_voice_cap_low` setting). AX chooses
  which lower-priority voices to drop; this is not a literal voice count.
- Reverb and both sampling profilers disabled; periodic PERF lines retained.
- No frame skipping is enabled by the preset.

Remove the file to restore the usual settings. Existing individual markers
still work. For useful HPROF samples, run without `perf_turbo` or `no_hprof`.
Do not combine testing with `frameskip`/`frameskip2` if measuring real rendered
frame throughput.

## Build and code-size tradeoff

This requires a **full shard rebuild**. The fast-build script now guards
`shard_hot_inline.h` too; never use FORCE or timestamp changes for this round.

```sh
VITASDK=/usr/local/vitasdk cmake -S . -B build-vita \
  -DMKWV_SHARD_HOT_INLINE=ON -DMKWV_SHARD_MEMORY_INLINE=ON \
  -DMKWV_INIT_ARRAY_ADDR=0x88000000
VITASDK=/usr/local/vitasdk cmake --build build-vita \
  --target wiicompiled-vita.vpk-vpk -j3
```

The initial sampled ARM shard grew from 568,631 to 1,198,843 bytes of text.
Its common scalar RAM wrappers disappeared as calls; checked miss branches
still contain calls, so **total static call count is not a hot-path metric**.
That sample preceded outlining the RAM-only store fallbacks. Larger code may
hurt instruction-cache locality or memory headroom; device A/B testing is
required. `.init_array` is placed at 0x88000000 to leave room above the larger
text segment. CMake permits `-DMKWV_SHARD_MEMORY_INLINE=OFF` for a smaller
comparison build; that also requires rebuilding all shards.

The full build uses three compiler jobs to avoid severe host memory compression
from six simultaneous large shards. Increase parallelism only with memory headroom.

The previous ELF and VPK are preserved under `build-vita/performance/static65/`.
Keep the ELF paired with its VPK when symbolizing logs.

## Validation and next hardware run

The ARM Mac host runs the NEON paths. All 17 fixture-independent tests pass,
including 12 combinations of compact packing (zero/one/two texgens, default/
explicit matrix indices, GPU-lit/unlit), cache/ring tests in direct and staged modes with two workers,
including oversized-batch fallback and early GPU-overwrite synchronization,
20,000 wide FIR dot products, and parameter-block byte swapping at unaligned
offsets and varied lengths. The full audio mixer also matches an independent
scalar polyphase reference at three fractional rates across nine command lists,
including exact persisted sample history and fractional position. The existing translated ISA test still fails at
`nand read`; its device NAND fixture is unavailable in the host test directory.
The Vita AX object contains `vrev16`, `vmull.s16` and `vpaddl.s32` instructions;
the vertex-upload object contains vector loads/stores and `dsb sy`.
A local ARM Mac microbenchmark of 4,096,000 restores took 60.7 ms for full
restoration and 45.7 ms for compact restoration (about 25% lower). This is
a hot-loop host measurement, not an end-to-end Vita frame-rate prediction.

Compare the same race/course/camera after warm-up:

1. Previous static65 VPK, usual markers: baseline.
2. static66, usual markers: computational changes with profiling available.
3. static66 with `no_prof` and `no_hprof`: exclude sampling overhead.
4. Optional `direct_vertex_ring` run: isolate cached staging.
5. static66 with `perf_turbo`: quality/budget tradeoffs.
6. Optional `no_audio` run: isolate the entire audio cost before pursuing
   more invasive sound changes.

Keep `boot.log` and `watchdog.log` from each run. Compare PERF fps, frame time,
`cpu: game/rend/w0/w1/snd`, `help/wwait`, `sub: idx/draw`, `slow:`, audio queue/
underruns and cache/ring counters. Check geometry, menu transitions, racing
and sustained audio, not just a stationary frame.

Symbolize HPROF without manual address arithmetic:

```sh
python3 scripts/symbolize-vita-profile.py watchdog.log \
  --elf build-vita/vita/wiicompiled-vita
```

The tool applies the logged relocation anchor and groups top PC/LR buckets
by function separately for each report. Bucket percentages are rounded,
partial samples, not complete per-function CPU accounting. A relocated
synthetic fixture was verified against the preserved static65 ELF.
