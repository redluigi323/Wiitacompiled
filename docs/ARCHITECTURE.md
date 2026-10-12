# WiiCompiled Vita architecture

Mario Kart Wii on Vita is a static recompilation port, not Dolphin-on-handheld. Upstream
WiiCompiled already proves that path on 64-bit PCs: a C# translator lifts Broadway PowerPC to C++,
a runtime HLE's OS/DVD/audio/input, and [Aurora](https://github.com/encounter/aurora) implements GX
on Dawn/WebGPU.

Vita changes the host so violently that "cross-compile the PC runtime" is not a port. The work is
three coupled problems: **ARM32**, **90 MiB-class RAM**, and a **native GX→GXM backend**.

## Hardware we actually have

| | Wii (Broadway / Hollywood) | Vita (PCH-1xxx/2xxx) |
| --- | --- | --- |
| CPU | 729 MHz PPC 750CL, in-order dual-issue, 1 core used by MKW | 4× Cortex-A9, userland max **444 MHz**, kernel OC **500 MHz** (~494) |
| GPU | Hollywood GX, immediate TEV, EFB 640×528 | PowerVR SGX543MP4+ @ **166–222 MHz** ES4, tile-based deferred, GXM + Cg |
| RAM | 24 MiB MEM1 + 64 MiB MEM2 | 512 MiB LPDDR + **128 MiB CDRAM** |
| Display | 480p (16:9 ~854×480) | 960×544 @ 60 Hz |

Userland clocks via `scePowerSet*ClockFrequency` top out at **444 / 222 / 222 / 166**
(ARM / GPU-ES4 / BUS / XBAR). 500 MHz ARM is a kernel plugin (PSVshell / similar), not something a
VPK can guarantee. GPU "333 MHz" in older plugins is the codec engine, not the SGX cores.

Single-thread Cortex-A9 @ 500 MHz is not Broadway. 60 fps only happens if we:

1. Keep the guest on one core and move GX submit / audio / DVD off it.
2. Never pay Dawn, wgpu validation, or a 4 GiB VA map.
3. Feed the SGX tiled, swizzled, compressed textures (CMPR→DXT1 is a native path).
4. Render below 960×544 until the GPU budget is proven, then scale up.

## What upstream will not do for us

`upstream/runtime/CMakeLists.txt` requires 64-bit Clang. `guest_flat_memory.h` reserves a **4 GiB**
guest VA at a fixed high address (`0x100000000000` on x86-64, `0x8000000000` on Darwin ARM64,
`0x1000000000` on Linux AArch64) so translated loads become `*(T*)(kFlatGuestBase + addr)`.

Vita is ILP32. There is no 4 GiB user VA, and there is nowhere to put that base. The ARM64 work
(macOS / experimental Linux aarch64) is useful for NEON paired-singles and the cooperative-fiber
split, and useless for the address-space trick.

The shipped memory config also backs **128 MiB MEM2** (NDEV-sized) so MKW creates the EGGRootDebug
heap. Unique guest RAM is then ~152 MiB plus a 2 MiB Kamek overlay. That is an unacceptable default
on a 512 MiB machine that still has to hold translated code, GXM parameter buffers, and CDRAM
working set. Vita uses **retail 64 MiB MEM2**.

Dawn is not a Vita API. Aurora's `lib/gx/shader.cpp` emits WGSL. The Vita backend keeps the FIFO /
TEV / vtxfmt state (the part that is "GX"), and replaces `lib/gfx` + Dawn with GXM.

## Guest memory (ARM32)

Wii addresses are already `uint32_t`. `CpuContext.gpr[]` is already `uint32_t`. The 64-bit-only
assumption is the *host mapping*, not the ISA package.

Vita mapping:

- One 24 MiB MEM1 buffer, aliased at `0x00000000` / `0x80000000` / `0xC0000000`.
- One 64 MiB MEM2 buffer, aliased at `0x10000000` / `0x90000000` / `0xD0000000`.
- 2 MiB Kamek overlay at `0x81800000` (Rel24 range).
- 16 KiB locked cache at `0xE0000000` (THP). Not padded to 1 MiB.

The 1 MiB page table and encoded-bias fast path from `memory_access.h` still work: on ILP32 the
bias arithmetic wraps mod 2^32 and still recovers `host = guest + (host_base - guest_page_base)`.
MMIO (`0xCC000000`) never gets a fast-path entry; FIFO writes go to GX HLE.

Translated code on Vita **always** uses this checked path. `GuestFlat::IsActive()` is false. No
SIGSEGV/MMIO trampoline — Vita signal handling is not Windows VEH, and we cannot spare a 4 GiB
guard map.

## Translated code size

The translator emits on the order of **28k function TUs**. A single VELF that contains all of them
will not fit in RAM next to 90 MiB of guest memory. This is the other hard ARM32 problem, equal
with graphics.

Plan, in order:

1. `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections`, no RTTI in shards, no frame
   interpolation, no ImGui/RmlUi.
2. A function **overlay cache**: shards live on `ux0`, a fixed RAM window maps hot functions, cold
   ones are unmapped. MKW already thinks in DOL + REL + Kamek overlays; we extend that to native
   code. The catalog, 1 MiB LRU window, shard files, FIFO MMIO HLE, and PPC-address dispatch are
   in tree. Loading a shard into RX memory and executing it is still future work — residency
   currently means “this native function is live,” not “this `.o` was mapped executable.”
3. Only after (2) is the translator allowed to emit Vita-specific C++ (no `uint64_t` host pointers,
   no `MKW_FLAT_GUEST_BASE`). Stock emit can still be consumed: `MemoryInline::Flat*` is
   implemented on CompactMemory, so the 4 GiB map is not required at runtime.

Do not try to `mmap` a 4 GiB file of code. There isn't a POSIX mmap that works that way on Vita.

## Graphics: Aurora semantics, GXM metal

Keep (from Aurora, as reference then as extracted code):

- `lib/gx/command_processor.cpp` — FIFO decode, BP/CP/XF, aurora extensions.
- `lib/gx/gx.hpp` TEV / vtxfmt / fog / indtex structs.
- `lib/gfx/texture_convert.cpp` — CMPR, RGB5A3, CI8, etc.
- EFB→RAM / EFB→texture contracts.

Replace:

- Dawn / wgpu command encoding.
- WGSL shader generation → **Cg**, compiled offline or cached as `.gxp`, patched with
  `sceGxmShaderPatcher`.
- Texture upload → CDRAM, **swizzled**, native `UBC1` (DXT1) for CMPR.
- Color surfaces **tiled**. Display buffers in CDRAM. Parameter buffer in LPDDR
  (`SCE_GXM_INITIALIZE_FLAG_PB_LPDDR`) so VRAM stays on textures.

SGX is tile-based. Immediate GX with thousands of tiny draws is the failure mode. The submit path
must batch by pipeline (TEV config hash → patched GXM programs) and avoid mid-scene render-target
changes. EFB copies become GXM mid-scene resolves; they are expensive and will be the first things
we LOD/skip if a track is GPU-bound.

Internal resolution starts at **Wii EFB 640×528** (or 640×368 16:9) and is bilinear-presented to
960×544. Native 960×544 is a later unlock, not bring-up.

## Fibers

Guest `OSThread` is cooperative. Windows uses Fibers; Linux uses libco; macOS ARM64 uses a local
`.S` because Darwin's `x18` is reserved. Vita uses a local ARMv7 `.S` that saves `r4–r11`, `lr`,
and VFP/NEON `d8–d15` (AAPCS hard-float). libco's Clang path wants `mprotect` on a code blob, which
is the wrong fight on Vita.

## 60 fps plan (deliberately insane, but ordered)

1. **Correct 30** with 640×-class EFB, retail MEM2, clocks at 444/222/222/166.
2. Move FIFO consume + texture decode to a second A9; keep PPC-translated code on core 0.
3. TEV specialization cache (not one ubershader). Hash the Aurora `ShaderConfig`.
4. CMPR stays compressed. Swizzle offline where the translator can see static TPL data.
5. No MSAA. No Aurora frame interpolation (that exists for 120 Hz PCs).
6. Drop THP / menus extras first if a race is CPU-bound.
7. Only then try 500 MHz ARM and 960×544.

If a track cannot hold 60 at 640× and 500 MHz after (1–6), the answer is more batching and fewer
TEV variants, not a higher internal resolution.

## Legal / licensing

WiiCompiled is GPL-3. This port is GPL-3. Aurora is MIT and can be linked. Game data stays on the
user's dump. This repo must never grow a `main.dol` or a disc image.
