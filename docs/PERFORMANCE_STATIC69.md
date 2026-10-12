# static69: trophy/general-texgen worker path

This pass follows the user's Vita3K menu log. The steady slow samples show
40.8 fps, 24.5 ms/frame, 21.8 ms decode, 2.6 ms submission, 14.9 ms worker
waiting and 19,512 vertices in the general-texgen category. Approximately
21,500 of 23,000 vertices already use GPU lighting. Guest time is about
6.8 ms. The log also has lighter menu intervals at 60 fps; none is a Grand
Prix race measurement. Vita3K's per-thread CPU counters are zero here and
cannot establish real Vita core utilization.

The next target is the CPU vertex pipeline feeding the GPU. Shader submission
is much smaller than decode in these samples. The original fast path accepted
only up to two 2x4 texgens reading their corresponding raw UV coordinates,
without normalization or per-vertex texture matrices. Normal-based reflection,
projective channels and additional texgens all sent the entire vertex through
the general pipeline, including separate model-view and projection operations.

## Changes

- Worker fast transforms/packing now accept up to eight texture channels,
  position/normal/NBT/colour/UV sources, 3x4 projection, normalization and SRTG.
- Fixed texture matrices are prepared once per job. Linear post matrices are
  composed once; normalized channels keep both transforms and their original
  normalization. Zero-q clamping and SU scaling remain in the pipeline.
- Matrix columns evaluate all three texcoord components with NEON vector
  multiply/adds instead of three horizontal dot-product reductions per channel.
- Per-vertex texture matrix indices retain the unchanged texgen evaluator,
  followed by fast composed clip/eye/normal transforms and packing.
- Compact cache hits skip the approximately 150-byte default-vertex copy for
  any channels needing only position, normal, colours and UV0/UV1. NBT, higher
  UV sources, dynamic texture matrices and mirrored/captured vertices retain
  full restoration. Eligibility is checked once per job.
- Unlit jobs no longer prepare unused eye and normal matrices.

No shader quality reduction, model removal, frame skipping or shard helper
inlining is added by this pass. GPU lighting and draw merging remain enabled.
Prepared matrix composition changes floating-point rounding; worker geometry
checks use a relative/absolute tolerance of 1e-4, with colours and layouts exact.
Existing identical-bit NaN values in the reuse fixture remain accepted.

## Build and compare

This is a **fast build**: no shard sources, shard ABI headers or compiler flags
were changed. All 74 current shard objects exist and their ABI guard inputs
are older than the objects. Do not use FORCE if the script reports otherwise.
The agent compiled decode.cpp and main.cpp with the Vita SDK; final linking
and VPK packaging are left to the user's terminal.

```sh
cd Wiitacompiled
export VITASDK=/usr/local/vitasdk
JOBS=3 scripts/build-vita-fast.sh
```

Install the VPK after successful packaging. Check `boot.log` for `static69`
and `prepared texgen on`. The new path is ON by default.

Create the empty `ux0:data/viicompiled/no_prepared_texgen` marker and restart
to send newly accelerated jobs back to the original general evaluator.
Delete it and restart to restore the new path. This compares the expanded
path in one VPK; already eligible simple UV jobs still use worker packing.
Keep all other markers fixed, including perf_turbo, for both tests.

First compare the trophy menu after warm-up, then a sustained Grand Prix race.
Inspect fps, decode, worker/help/wwait and vfast. More vfast vertices prove
that jobs entered the expanded path; only lower frame time proves a benefit.
Check trophy reflections, text/UI, animated/skinned geometry and racing.

The supplied log and previous VPK are saved under
`build-vita/performance/static68-vita3k/`. The old ELF is preserved there when
available. Neither the host tests nor the SDK compilation establishes FPS
improvement in Vita3K or on real Vita.

## Validation

All 18 fixture-independent host tests pass (NAND-dependent translated ISA
fixture excluded). There are 240 comparisons against the unchanged general
texgen/vertex evaluator, repeated with the new path disabled, spanning 1–8
channels, all source families, normalized and linear post transforms,
projective/SRTG output, AB11 input, zero-q clamps, dynamic matrix fallback,
GPU-lit and unlit jobs. Cache/reuse tests compare inline and worker results,
including two workers, while keeping inline cache equality exact.
The Vita decoder and startup objects compile; the decoder contains vector
multiply/adds and vector stores. Hardware/emulator gameplay validation and
60 fps Grand Prix remain pending.
