# Wiitacompiled

this is a really work in progress port of Mario Kart Wii to the PSVita.

the goal is to push the hardware as much as possible and eventually have feature parity with all other versions of [wiicompiled.](https://github.com/patchzyy/wiicompiled)

letting you know now this is vibecoded with claude's help, and has mostly been a project ive been tackling in my free time, what it needs is major optimizations.

## where it's at

- boots and plays on real hardware, menus and races, grand prix included.
- races sit around 20fps. grand prix is lower until you take 1st.
- a few renderer glitches are still around.

if you're willing to sit through a race at 20fps it's playable. the steps to build it yourself are in [BUILD.md](BUILD.md).

## the renderer

on PC, wiicompiled draws through [aurora](https://github.com/encounter/aurora), which turns the Wii's GX commands into WebGPU via Dawn. porting dawn to the vita's hardware wasnt a good idea, so aurora's GX layer (command FIFO, vertex formats, TEV, EFB copies) was ported to GLES and runs on [vitaGL](https://github.com/Rinnegatamante/vitaGL). TEV setups become shaders that get compiled on the Vita itself through [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK), which is why `libshacccg.suprx` is required.

## what's been done to make it run

the short version, since the long version is [handoff.md](handoff.md):

- **guest memory.** the PC build maps a flat 4 GiB guest address space, which a 32-bit process can't do. the Vita build packs MEM1/MEM2 into a compact layout behind a page table.
- **threads.** the game thread, render thread and sound thread each get their own core, and vertex decode runs as jobs on whatever core is idle.
- **vertex work.** decoded display lists are cached, and vertices stream into a GPU ring buffer instead of being re-uploaded every draw.
- **native engine routines.** the hottest SDK/NW4R matrix, vector and animation functions are replaced with hand-written NEON versions. the original translated code is kept as a fallback.
- **audio.** AX/DSP is HLE'd and mixed on its own core.

[handoff.md](handoff.md) is the place to start if you want to work on it: how the port is put together, the build rules that keep it from breaking, how to read the logs and profiler, what's already been tried, and where the frame time goes now. [docs/](docs) has the older architecture notes and write-ups of the latest performance rounds.

## layout

| path | what |
| --- | --- |
| `vita/` | the port: runtime, GX → GLES renderer, platform code, CMake |
| `tests/` | host-side tests that don't need a Vita or game files |
| `scripts/` | dependency fetch, asset linking, fast rebuilds, profiling helpers |
| `patches/` | small local changes applied on top of vitaGL and vitaShaRK |
| `tools/` | coredump parser and a GX replay tool |
| `upstream/` | not in git; `scripts/fetch-deps.sh` clones wiicompiled here at the pinned commit |

## game files

there is no Nintendo code, no assets and no disc image in this repo, and there won't be. you need your own clean PAL `RMCP01` dump. the translated code is generated on your machine from that dump and stays there. please don't attach game files to issues.

## credits

- [patchzyy](https://github.com/patchzyy) and everyone behind [wiicompiled](https://github.com/patchzyy/wiicompiled), which this is all built on.
- [aurora](https://github.com/encounter/aurora), whose GX layer the renderer is ported from.
- [Rinnegatamante](https://github.com/Rinnegatamante) for vitaGL, vitaShaRK and math-neon.
- [vitasdk](https://vitasdk.org).
- [Dolphin](https://github.com/dolphin-emu/dolphin), as the reference for how the hardware behaves.

## license

GPL-3.0, same as wiicompiled.
