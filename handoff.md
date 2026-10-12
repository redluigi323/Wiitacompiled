# Handoff

Read this before changing anything. It covers how the port is put together, the rules that keep the build
correct, how to see what the game is doing on a Vita, what has already been tried, and where the time
goes now. [BUILD.md](BUILD.md) covers building and installing.

## State

- **Real hardware:** boots, plays menus and races, grand prix included. On static72 (`perf_turbo`) 3D
  menus run at about 22 fps and races at about 12. Grand prix runs lower until you reach 1st. Light
  menus hit 60. Vita3K draws some geometry wrong with the GPU transform; the real Vita does not.
- **Loading:** the first time a scene appears its shaders are compiled on the Vita (a race start used to
  take 30–40 s of it). From static73 vitaGL keeps them in `ux0:data/shader_cache/MKWV00001/`, so only
  the first visit pays. Deleting that folder is safe; it just costs the first-run compile again.
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
- **GPU transform** (static71, `no_gpu_xf` turns it off). A display-list primitive that hits the DL
  cache, in a state the shader can evaluate, is drawn straight from a GPU copy of the cache with a
  vertex shader (`emit_tev_vertex_cg_xf`) that does the position/normal matrices (indexed by the
  per-vertex matrix index), the projection, texgen and the lighting. The CPU does no per-vertex work
  for it: no vertex job, no packing. What makes it work:
  - The DL cache already gives each primitive a stable identity. The static52 mesh cache failed
    because finding that identity cost more than the transform; here it is free.
  - The GPU copy (`dl_cache_install_gpu`) mirrors the pool slot for slot, in uncached GPU-mapped
    memory. An entry is copied the first time it is drawn this way. Every slot remembers the frame
    it was last drawn in; a copy waits for the GPU (`gsync=`) only when it would overwrite vertices
    drawn in the last 3 frames. (static71 waited whenever the entry had been allocated in the last 3
    frames; the race refills a few DL primitives every frame, so that was a `glFinish` nearly every
    frame once the ring had wrapped.)
  - Uniform arrays are only indexed with constants. static71 indexed the matrix palette with the
    per-vertex matrix index and rendered broken geometry in Vita3K (the same program is correct on
    a conforming GLSL implementation, see Tests). Layouts with a matrix index get a program that
    selects the matrix with a chain of compares; others put their one matrix in slot 0.
  - Everything not per vertex is one `GpuXfParams` record per state epoch (`draw_state.hpp`):
    matrices, projection, colour selection and texgen. Texgen is uniform-driven: source weights,
    2x4/3x4/SRTG/identity as 3x4 rows, the dual-tex post matrix, normalize, the zero-q fold and SU
    scale. `gpu_xf_eligible` lists what it covers (texgen from position, normal, texcoord 0 or 1;
    colour channels on). Everything else stays on the vertex jobs.
  - Draws of one epoch merge into one GL draw; different epochs cannot (their matrices differ), so
    expect somewhat more GL draws than before.
  - `gpu_xf_reference` in `decode.cpp` is the shader in C++. `gx_gpu_xf_tests` compares it with the
    CPU path over random states and end to end through the decoder.
  - The program is probed at startup (`gpu_xf_probe`); if the shader compiler refuses it, the GPU
    transform stays off and boot.log says so.
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
    - Submit breakdown: `sub: idx= draw= mesh= prep=` (ms), `uup=lit/xf` (lighting and transform
      uniform sets re-uploaded), and `prog= pbuild=` (programs built and ms spent building them;
      with a warm shader cache `pbuild` should be small even on a scene's first frame).
    - Draw counts: draws, GL draws, vertices, `vfast`/`glit`.
    - Caches and buffers: `dl:` cache hits, `gxf=` vertices drawn with the GPU transform (`gcopy=`
      copied into its GPU copy, `gsync=` waits for it), `ring=`/`rsync=`, `vpool=`.
    - **`cpu: game= rend= w0= w1= snd= aout=`**, real CPU ms per thread per frame. This is what to
      trust over wall-clock zones.
    - `eng: native= ppc=` for the engine natives.
  - **`PROF`**, the guest profiler. It shows self, inclusive and lr per guest function for the game
    and sound threads. It only sees functions entered through dispatch: anything called statically is
    counted in its caller.
  - **`HPROF`**, the host-PC sampler (`no_hprof` turns it off). It sees inside statically called code.
  - `perf_turbo` turns both profilers off unless the `prof` marker exists too. Profile with
    `perf_turbo` + `prof` when that is the configuration being played.
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

**Markers** are empty files in `ux0:data/viicompiled/`, read at boot. Most are A/B switches for one
optimisation:

| group | markers |
| --- | --- |
| quality / speed | `perf_turbo`, `efb_low`, `efb_lower`, `fx_low`, `fx_off`, `frameskip`, `frameskip2`, `race_dof` |
| renderer A/B | `no_gpu_xf`, `xf_no_skin`, `xf_no_lit`, `xf_no_texgen` (keep skinned / lit / texgen'd epochs off the GPU transform), `exact_tev`, `cpu_lighting`, `no_dl_cache`, `no_vbo_ring`, `direct_vertex_ring`, `no_prepared_texgen`, `no_merge`, `no_vertex_reuse`, `no_shaders` |
| threading | `no_render_thread`, `render_nosync`, `render_sync`, `no_vsync`, `no_vertex_worker`, `one_vertex_worker`, `workers_low`, `no_early_handoff`, `no_snd_thread`, `snd_share` |
| audio | `no_audio`, `reverb`, `mix_inline`, `snd_voice_cap`, `snd_voice_cap_low` |
| game code | `no_native_engine` |
| diagnostics | `prof` (profilers on under `perf_turbo`), `no_prof`, `no_hprof`, `heap_check`, `capture_now`, `race_sheet` |

When comparing two runs, change one marker and keep the same race and camera.

## Tried and dropped

These are worth knowing so they don't get tried again without a new reason:

- **Inlining guest memory accessors into the shards.** It doubled shard code and compile time and
  gave no measurable FPS on hardware.
- **Force-inlining compare/FP helpers.** Small code cost, but also no gain. Since then every
  forced inline in the shards is off: `MKW_PPC_FORCE_INLINE` and `MKWV_MEM_INLINE` are plain
  `inline` and `MKW_PPC_ALWAYS_INLINE_BODY` is empty on the Vita. Don't bring `always_inline` back.
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

Real Vita, static72 with `perf_turbo` (no profile in that log):

| scene | frame | render thread | of which decode | of which submit | GL draws |
| --- | --- | --- | --- | --- | --- |
| 3D menu | ~45 ms | ~45 ms | ~27 ms | ~16 ms | ~236 |
| race | ~80–85 ms | ~80 ms | ~40 ms | ~38 ms (draw ~27) | ~700 |

The game thread spends most of a race frame waiting for the renderer (`rwait` ~35 ms of ~48), so
the render thread is the limit. Decode and submit run back to back on it. Next steps, in order:
symbolised HPROF of the render thread (what makes decode cost ~15 µs per GX draw with almost no
vertex work left), fewer or cheaper GL draws (~38 µs each now: one per state epoch on the GPU
transform), then decoding on another core while the render thread submits.

Older real-hardware race profile (before the GPU transform):

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
- **Render thread.** Before static71, vertex work was the largest part: in a heavy menu ~21k
  vertices a frame, 19.7k of them DL cache hits, cost ~50 ms of worker CPU. The GPU transform takes
  those off the CPU. What is left to check on a log: `gxf` against `dl: hit` (hits the shader does
  not take, and why - `gpu_xf_eligible`), the GL draw count and `sub: draw` (more GL draws with
  bigger uniform uploads), `vpool`, and `gsync` (should be 0 outside scene changes). Vertices that
  are not DL hits still go through the jobs; `slow:` says which of those miss the fast paths.
- **Sound.** If `snd` stays high, cut nw4r's per-frame work (voice counts, update rate).
- **GPU.** The GPU is not the limit right now (`swap` is ~0.3 ms). If that changes, `efb_low` and
  `efb_lower` tell you whether it is fill-bound.

## Tests

The host tests need no Vita or game files (see [BUILD.md](BUILD.md)). The GL side runs against a stub
GL (`tests/stub_gl`). The gx tests build anywhere. The runtime tests currently only build on ARM64
hosts (Apple Silicon): on x86-64, `ppc_isa_quantized.h`'s SSE paths do not compile against the
paired-single vector type.

**NEON paths** are checked by cross-compiling the gx tests for Cortex-A9 and running them under
`qemu-arm`. There is no script in the repo for this: use `arm-linux-gnueabihf-g++` with
`-mcpu=cortex-a9 -mfpu=neon -fno-pie`.

**GPU transform programs on a real GLSL implementation:** `tools/gpu_xf_check/`. The host build's
`mkwv_gpu_xf_cases N` writes random cases (program, uniforms, vertices, C++ reference outputs);
`run_cases.py` converts each Cg program to GLSL with NVIDIA's `cgc`, runs it with transform
feedback (Mesa llvmpipe through EGL is enough) and compares every output. It needs `cgc` and
Python's `moderngl`, so it is not part of ctest. Run it after any change to `emit_tev_vertex_cg_xf`.

**Known failure:** `translated_isa_tests` (its NAND fixture isn't in the repo).
