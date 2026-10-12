# Handoff

Read this before changing anything. It covers how the port is put together, the rules that keep the build
correct, how to see what the game is doing on a Vita, what has already been tried, and where the time
goes now. [BUILD.md](BUILD.md) covers building and installing.

## State

- **Real hardware:** boots, plays menus and races, grand prix included. Races run at about 14–20 fps,
  and grand prix runs lower until you reach 1st. Light menus hit 60. 3D-heavy menus are well below that.
- **Rendering:** correct apart from some small glitches. Depth of field is off in races, as it is upstream.
- **Saves:** go through the NAND HLE (details below).
- **Target:** 60 fps. That is not reachable with the current guest CPU cost (see "Where the time goes").

Builds identify themselves in `boot.log` with a `rev=staticNN-...` string. Bump it in `main.cpp`
whenever you hand someone a build, so their logs can be matched to the build that produced them.

## Layout of a running frame

The Vita has three usable Cortex-A9 cores at 444 MHz (500 with a kernel overclock plugin, which the app
tries for at boot).

| thread | what it does |
| --- | --- |
| game (`main`) | runs the translated game. Guest `OSThread`s are cooperative fibers on this thread (`co_switch.S`, saves r4–r11, lr, d8–d15) |
| render (`mkwv_render`) | GX FIFO decode, draw merging, GL submit, swap. All GL calls happen here, including vitaGL init |
| sound | AX/DSP HLE, the DSP mix, and nw4r's sound and task threads as fibers |
| vertex workers ×2 | vertex jobs on whichever core is idle. They run below game, render and sound in priority (`workers_low` drops them further) |

**Hand-off between the game and render threads.** The game thread passes raw FIFO bytes to the render
thread: `hle_fifo_full` forwards a buffer when it fills, and `hle_present_frame` sends the frame's tail.
At each present the game thread waits until everything is decoded and submitted (GXDrawDone semantics).
It does not wait for the swap, which is deferred so vsync overlaps the next frame.

## Translated code and how calls reach it

Upstream's translator turns the PAL DOL and StaticR.rel into ~29k C++ functions. These are grouped
into ~74 shards (`upstream/generated/build_shards/base_common/shard_*.cpp`) by contiguous guest address
range. They build as the `mkwv_translated` object library at `-O2`, without fast-math. FP contraction
and SLP vectorisation are off: the translator's float casts are PPC rounding points.

There are three call styles. An HLE replacement only runs if it hooks the style the caller actually uses:

| call site | how it binds | how to replace it |
| --- | --- | --- |
| indirect `bctrl` (virtuals, function pointers) | `invoke_guest` → overlay → `find_hle_native` → static table, behind a 4096-entry cache | `g_natives` in `hle_native.cpp` (keep it sorted). Runtime only, no shard rebuild |
| `InvokeDirectCpu<addr>` | compile time | `native_cpu_calls.inc`. Shard rebuild |
| `MKW_STATIC_TRANSLATED_CALL(addr, func_..., ctx)` | direct call to `func_...` | `native_cpu_calls.inc` (`abi_bridge.h` prefers natives). Shard rebuild |

**`--wrap` does not work** for this. A caller in the same object as the definition never goes
through the linker, and most hot callers are in the same shard. Two mechanisms do work:

- **Engine natives**, the newest mechanism. `vita/include/mkwv/runtime/engine_native_catalog.inc` lists
  guest addresses. At configure time, `scripts/prepare-engine-shards.py` copies the shards that define
  those functions into the build folder and renames the definitions there. Every caller, same shard
  included, then binds to the native wrapper in `engine_native.cpp`. The original translation stays
  callable as a fallback. 38 SDK/NW4R matrix, vector, trig and animation-track routines are done this
  way, in NEON, and the `no_native_engine` marker turns them off. This is the lever for game-thread
  speed (see below).
- **Extra translated functions.** A few functions that upstream replaces natively on PC are needed
  translated here. `scripts/extract_translated.py` pulls them out of
  `upstream/generated/base_translation_sources.bin` at build time (see `MKWV_EXTRA_TRANSLATED` in
  `vita/CMakeLists.txt`). That output comes from the user's disc and is never committed.

Upstream's desktop HLE sometimes loops per byte through `MemoryInline` (for example the Yaz0 decoder
in `egg_decomp.cpp`). That is fine on a PC and far too slow here. Decode on the host and `memcpy` in.

## Guest memory

PC wiicompiled reserves a flat 4 GiB guest map. A 32-bit Vita process cannot do that. `CompactMemory`
works as follows:

- **Buffers.** MEM1 (24 MiB) and retail-size MEM2 (64 MiB, not upstream's 128) are separate buffers,
  aliased at their cached and uncached addresses. There is also the 2 MiB Kamek overlay and the 16 KiB
  locked cache.
- **Pages are 1 MiB** (`kPageShift = 20`). `host_pointer(addr, len)` fails across a page boundary, so
  bulk copies have to chunk by page (`guest_copy` in `hle_os.cpp`).
- **Accessors.** Translated loads and stores take an inline fast path (one table load, a range check,
  LDR+REV). MMIO, FIFO, partial pages and page-straddling accesses fall back to `*_slow`.
- **Memory headroom.** The app runs in extended memory mode (`ATTRIBUTE2=12` in the SFO flags). Guest
  blocks are allocated largest first, falling back from user memory to phycont and then CDRAM. vitaGL's
  pools are sized from whatever is left. `boot.log` prints free memory at each step and where each
  block landed.
- **`.init_array` is pinned** (`MKWV_INIT_ARRAY_ADDR`) so it does not overlap vitaGL's rodata. If
  `.text` grows past it the link fails with an overlap error; raise the address.

## Renderer

The GX side follows aurora: command processor, BP/CP/XF state, vertex formats, TEV, texture formats
and EFB copies. The backend is GLES 2 on vitaGL. The main files are `vita/src/gx/fifo.cpp`,
`decode.cpp`, `gl_submit.cpp`, `gl_backend.cpp`, `cg_emit.cpp` and `tev.cpp`.

- **Vertices** are decoded and transformed on the CPU into GL clip space. This happens in vertex jobs
  (`vertex_worker.cpp`) that the render thread also helps with. The fast paths are:
  - a combined clip matrix per state epoch
  - prepared texgens (up to 8 channels; `no_prepared_texgen` turns this off)
  - NEON column transforms
- **GX lighting** runs in the vertex shader (`cpu_lighting` turns it off). Lit vertices carry their
  raw colours plus eye-space position and normal. `gpu_lit_reference` in `decode.cpp` is the shader's
  maths in C++, and the host tests compare the two.
- **Display-list vertex cache** (`dl_cache.cpp`, `no_dl_cache` turns it off). Primitives inside
  display lists are cached by data address, layout, count and packet hash. Every 4th hit re-checks
  vertex 0, and on a mismatch the primitive is marked volatile.
- **GPU vertex ring.** Jobs pack into an uncached memblock that the GPU reads directly. A run binds one
  VBO and re-points it with `vglBufferData`. Overwriting data from the last 3 frames forces a `glFinish`
  (`rsync=` in PERF, which should stay 0). `no_vbo_ring` turns the ring off.
- **TEV becomes Cg.** `cg_emit.cpp` turns each TEV setup into a Cg program, which vitaGL compiles on
  device through vitaShaRK/libshacccg and caches as `.gxp`:
  - The source holds only structure. Colours, konst, alpha refs and fog come in as uniforms, so
    palette changes don't recompile.
  - The program key is `hash_tev_program`.
  - "Fast TEV" is the default: float lerps with saturation, with no 8-bit wrap or rounding per stage.
    `exact_tev` switches to the exact version, which the SGX can't afford.
- **Draw merging.** Consecutive draws with equal state merge into one `glDrawElements`. The comparison
  uses a 64-bit key per draw (`draw_merge_key`), not a memcmp.
- **EFB copies** are rendered on the GPU into textures. Copy FBOs are pooled by size (12): every vitaGL
  FBO owns a GXM render target and a depth buffer, and creating dozens of them used to exhaust the
  pools and crash on race load.
- **Depth across scenes.** vitaGL is built with `STORE_DEPTH_STENCIL`, because every EFB copy ends the
  GXM scene and depth has to survive that.
- **Present** stretches the 16:9 display copy to 960×544. Don't letterbox it.

`vita/src/gxm/`, `gx/backend.cpp`, `gx/submit.cpp` and the `.cg`/prebuilt `.gxp` files in
`vita/shaders/` are the original raw GXM backend. They are not linked and are kept only for reference.
The switch to GLES on vitaGL is what first put the game on screen.

**vitaGL and vitaShaRK** are built with `HAVE_VITA3K_SUPPORT` (`vita/cmake/vitaGL.cmake`). The two
patches in `patches/` make shader compilation go through `shark_init_simple()` and stub out the taiHEN
extension symbols. The same build runs on Vita3K and on hardware.

## Audio

AX and the DSP are HLE'd (`vita/src/runtime/audio/`).

- **Deferred mix.** The DSP mix runs outside the guest lock, after the AI callback returns, the same
  way upstream's mix worker does it. `mix_inline` puts it back inside the callback.
- **NEON kernels** for the volume ramps, bus accumulation and the four-tap resampler. These are
  bit-exact against the scalar code.
- **Reverb off by default.** `AxManager::AuxCallbackFunc` zeroes the aux buffers, and `reverb`
  restores the original.
- **Voice cap.** `snd_voice_cap` and `snd_voice_cap_low` lower AX's DSP-cycle budget, so AX drops
  low-priority voices itself.

**Sound and game mutex.** nw4r's sound thread holds the sound mutex for every 3 ms frame. Between
sound frames, the sound thread gives the game thread up to 2 ms to take a mutex it is waiting on
(`audio_yield_to_game` in `hle_os.cpp`). Anything stronger than that deadlocked (see "Tried and
dropped").

## Saves (NAND)

- **Location.** The NAND root is `ux0:data/viicompiled/nand`. The game's files are under
  `title/00010004/524d4350/data/`.
- **`rksys.dat`** must be exactly 2,867,200 bytes. A read of an all-zero `rksys.dat` deletes it, so
  the next boot recreates it.
- **Don't delete saves by hand** unless you want a clean save.
- **`setting.txt`** is generated as a PAL identity.

## Build rules that keep the binary correct

- **Shard-visible headers.** These headers are compiled into the shards. Changing one means a full
  rebuild:
  - `abi_bridge.h`, `native_cpu_calls.inc`, `ppc_runtime.h`
  - `memory.h`, `memory_access.h`, `guest_flat_memory.h`, `recomp_mod_loader.h`, `isa/*.h`
  - `guest_access.hpp`, `compact_memory.hpp`, `dispatch.hpp`
  - `hle_fifo.hpp`, and through it `gx/fifo.hpp`, `state.hpp`, `types.hpp`, `texture.hpp`
  - the engine catalog, `prepare-engine-shards.py`, and the shard compiler flags

  This means **adding a field to `gx::Processor` or `gx::State` is a full rebuild.** Per-frame
  diagnostics belong in file-scope statics in `fifo.cpp`, with an accessor in `diagnostics.hpp`, which
  is not shard-visible.
- **Fast rebuilds.** `scripts/build-vita-fast.sh` runs a normal, dependency-checked build and keeps only
  the shard objects from rebuilding. If a shard-visible input changed, it refuses. **Don't force it,
  and never back-date a header's mtime**: both have produced binaries where some files saw an old
  struct layout, which failed in ways that looked like guest bugs.
- **Never use CMake's `<target>/fast` rules.** They skip dependency scanning, so a header change
  rebuilds nothing.
- **Build shards with `-j`.** Each shard job needs about 0.4–0.5 GB of RAM and roughly 25–30 s. Code
  size drives compile time, so anything that inlines more into shards makes the full build much longer.
- **`MKWV_SHARD_OPT`** (default `-O2`) is the lever if memory or I-cache becomes the problem.

## Seeing what it does

**Logs** go to `ux0:data/viicompiled/`:

- **`boot.log`** has startup, memory, clocks, the rev string and feature switches. It ends with
  `--- end of this run's log ---`, and anything after that marker is from an older run (the Vita
  doesn't truncate files reliably).
- **`watchdog.log`** has no such marker, so check its size or timestamps before trusting the tail.
  It carries:
  - **`PERF` every ~2 s:**
    - Frame timing: fps, frame/worst ms, guest/decode/submit/copy/swap/render/rwait.
    - Draw counts: draws, GL draws, vertices, `vfast`/`glit`.
    - Caches and buffers: `dl:` cache hits, `ring=`/`rsync=`, `vpool=`.
    - **`cpu: game= rend= w0= w1= snd= aout=`**, real CPU ms per thread per frame. This is what to
      trust over wall-clock zones.
    - `eng: native= ppc=` for the engine natives.
  - **`PROF`**, the guest profiler. It shows self, inclusive and lr per guest function for the game
    and sound threads. It only sees functions entered through dispatch: anything called statically is
    counted in its caller.
  - **`HPROF`**, the host-PC sampler (`no_hprof` turns it off). It sees inside statically called code.
    Symbolise it with `scripts/symbolize-vita-profile.py watchdog.log --elf build-vita/vita/wiicompiled-vita`,
    using the ELF from the same build.
  - **`STUCK`** lines when a guest mutex or the guest lock is held for 2 s, naming owner and waiter.

**Other tools:**

- **Frame capture:** hold L+R+Triangle, or create `capture_now`. The next frame's draws, TEV state,
  vertices and textures are written to `capture_NN/`.
  - `mkwv_gx_replay <dir>` (host build, `tools/gx_replay.cpp`) renders a capture with a software
    rasteriser and aurora's integer TEV. If the replay is right and the Vita is wrong, the renderer is
    at fault. If both are wrong, look at the decoder or HLE.
  - Captures contain game textures, so keep them out of git.
- **Crashes:** `scripts/parse-vita-core.sh ux0-dump.psp2dmp build-vita/vita/wiicompiled-vita`.
- **Hot shard ranking:** `scripts/rank-hot-shards.py` ranks shards by sampled time.

**Markers** are empty files in `ux0:data/viicompiled/`, read at boot. Most are A/B switches for one
optimisation:

| group | markers |
| --- | --- |
| quality / speed | `perf_turbo`, `efb_low`, `efb_lower`, `fx_low`, `fx_off`, `frameskip`, `frameskip2`, `race_dof` |
| renderer A/B | `exact_tev`, `cpu_lighting`, `no_dl_cache`, `no_vbo_ring`, `direct_vertex_ring`, `no_prepared_texgen`, `no_merge`, `no_vertex_reuse`, `no_shaders` |
| threading | `no_render_thread`, `render_nosync`, `render_sync`, `no_vsync`, `no_vertex_worker`, `one_vertex_worker`, `workers_low`, `no_early_handoff`, `no_snd_thread`, `snd_share` |
| audio | `no_audio`, `reverb`, `mix_inline`, `snd_voice_cap`, `snd_voice_cap_low` |
| game code | `no_native_engine` |
| diagnostics | `no_prof`, `no_hprof`, `heap_check`, `capture_now`, `race_sheet` |

When comparing two runs, change one marker and keep the same race and camera.

## Tried and dropped

These are worth knowing so they don't get tried again without a new reason:

- **Inlining guest memory accessors into the shards.** It doubled shard code and compile time and
  gave no measurable FPS on hardware.
- **Force-inlining compare/FP helpers.** Small code cost, but also no gain.
- **Register-preserving asm stubs for the accessors.** They removed only ~2% of spills.

  The game thread's cost is register pressure, not the accessor calls: 32 GPRs plus 32 FPRs are mapped
  onto ~13 host registers inside very large functions. No accessor calling convention fixes that.
  Replacing whole hot functions (engine natives) does.
- **GPU mesh cache** (`gpu_mesh.cpp`, still in the tree, never activated). Fingerprinting the arrays
  cost more than the transform it saved.
- **Making the game thread own the sound mutex across `SoundArchivePlayer::Update`, and a fair guest
  lock.** Both deadlocked at the title screen. The bounded yield described under Audio is what's left.
- **`--wrap` on translated functions.** See "Translated code and how calls reach it".

## Where the time goes, and what's next

Last full real-hardware race profile:

| thread | CPU per frame |
| --- | --- |
| game | ~50 ms |
| render | ~60 ms |
| sound | 25–50 ms |
| each vertex worker | 6–12 ms |

That is ~170 ms of CPU a frame across three cores, so every core is close to full. A 60 fps frame is
16.7 ms.

- **Game thread.** The biggest win left is more engine natives: find the hottest guest functions
  with HPROF (symbolised), check that they are self-contained, add them to the catalog, and write NEON
  or plain-float versions with a guarded fallback to the translated original. Take every
  implementation through the differential test in `tests/engine_native_tests.cpp` against the
  original translation before it reaches hardware. Known heavy areas:
  - `nw4r::g3d` model processing (`ScnMdl::G3dProc`), `ResAnmChr`
  - `RaceScene::OnCalc`
  - the race audio actors (`RaceRSARPlayer`, `SortAndToggleActors`)
  - particles
- **Render thread.** Vertex decode is still the largest part, with submit next. Look at the `slow:`
  reasons in PERF (vertices that miss the fast paths), and keep moving work into jobs or onto the GPU.
- **Sound.** If `snd` stays high, cut nw4r's per-frame work (voice counts, update rate).
- **GPU.** The GPU is not the limit right now (`swap` is ~0.3 ms). If that changes, `efb_low` and
  `efb_lower` tell you whether it is fill-bound.

## Tests

The host tests run on macOS or Linux and need no Vita or game files (see [BUILD.md](BUILD.md)). The GL
side runs against a stub GL (`tests/stub_gl`).

**NEON paths** are checked by cross-compiling the gx tests for Cortex-A9 and running them under
`qemu-arm`. There is no script in the repo for this: use `arm-linux-gnueabihf-g++` with
`-mcpu=cortex-a9 -mfpu=neon -fno-pie`.

**Known failure:** `translated_isa_tests` (its NAND fixture isn't in the repo).
