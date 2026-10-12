# static73: load times, and measuring the render thread on a real Vita

First pass from a real-Vita log (static72-xffix, `perf_turbo`). Steady numbers:

| scene | fps | frame | decode | submit | GL draws |
| --- | --- | --- | --- | --- | --- |
| 3D menu | ~22 | ~45 ms | ~27 ms | ~16 ms (draw ~8) | ~236 |
| race | ~12 | ~80–85 ms | ~40–43 ms | ~38–40 ms (draw ~27) | ~700 |

In a race the game thread waits ~32–37 ms of its ~48 ms frame for the renderer, so the render
thread is the limit, and decode and submit share it about equally. The log had no PROF/HPROF
lines: `perf_turbo` switched both profilers off.

Loads were 5–41 s frames, nearly all in submit. Most of it was SceShaccCg compiling TEV
programs the first time each material appeared (a race start built ~150 programs).

## Changes

- **vitaGL shader disk cache** (`HAVE_SHADER_CACHE` in `vita/cmake/vitaGL.cmake`). Every compiled
  shader is stored as a `.gxp` under `ux0:data/shader_cache/MKWV00001/v2/{v,f}/XX/`, keyed by
  a hash of its source; later compiles of the same source load it. Only the first visit to a scene
  compiles. The very first boot creates 512 cache folders once.
- **vitaGL patch:** the cache write is skipped when a compile fails (upstream would read a null
  program, and a cached failure would come back as a hit), and goes through a `.tmp` file renamed
  when complete, so a crash mid-write cannot leave a truncated shader for the next run.
- **Mesh path:** the object-space mesh draws used to compile a full TEV program they never used
  before building their own. The slot is now made without it; the main program is built if a
  normal draw ever asks for the same state.
- **`prof` marker:** keeps PROF and HPROF on under `perf_turbo`, so the played configuration can be
  profiled.
- **PERF additions:** `pbuild=` (ms building programs per frame), `prog=` now counts programs built,
  `sub: mesh= prep=` (mesh draws, everything before the draw loop) and `uup=lit/xf` (lighting and
  transform uniform sets re-uploaded per frame).
- `scripts/fetch-deps.sh` resets a dependency that carries an older version of its patch before
  applying the current one.

No change to what is drawn, or to any shard-visible header: this is a fast build.

## What to run

Fast build (`./scripts/build-vita-fast.sh`), install, check `boot.log` for `static73-loads`.

1. Load time: boot to the menu and start a race twice. The second run should load much faster;
   `pbuild` in the slow frames tells how much of a load is still compiling.
2. Profile: keep `perf_turbo`, add an empty `prof` file in `ux0:data/viicompiled/`, then sit in a
   3D menu for ~20 s and race for ~60 s (grand prix if possible). Send `watchdog.log` together with
   `build-vita/vita/wiicompiled-vita` (the unstripped ELF of that same build) so HPROF can be
   symbolised. That decides the next pass: what decode spends ~15 µs per GX draw on, and what
   makes a GL draw ~38 µs.
