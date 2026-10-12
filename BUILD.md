# building

> **heads up:** the game isn't fully ready. the framerate will be unideal: a race runs at around 20fps, and grand prix runs lower until you get into 1st. if you're willing to sit through that, it's playable minus some small renderer glitches. don't build this expecting a finished port.

this has only been built on macOS (Apple Silicon). Linux should work since it's all bash, CMake and .NET. Windows will need WSL.

## what you need

**on your computer**

- [vitasdk](https://vitasdk.org), with `VITASDK` set and `$VITASDK/bin` on your `PATH`
- CMake 3.16+, git and Python 3
- the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0), for wiicompiled's translator
- [wit](https://wit.wiimm.de) or anything else that can extract a Wii disc to a folder
- a clean PAL Mario Kart Wii image (`RMCP01`). other regions and modified dumps get rejected by the translator.
- around 4 GB of free disk space for the dump, the translated sources and the build folder

**on your Vita**

- HENkaku/Ensō with VitaShell, or whatever you use to move files and install VPKs
- `libshacccg.suprx` at `ur0:data/libshacccg.suprx`. dump it from your own firmware with [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D). without it nothing renders, and `boot.log` will say it's missing.

the app requests 444 MHz on its own. if a kernel overclock plugin lets it, it takes 500.

## 1. fetch the dependencies

```sh
git clone https://github.com/redluigi323/Wiitacompiled.git
cd Wiitacompiled
./scripts/fetch-deps.sh
```

this clones wiicompiled into `upstream/` and vitaGL, vitaShaRK and math-neon into `third_party/`, at the exact commits this tree was built against. it then applies the patches in `patches/`. running it again is safe.

## 2. extract your disc

```sh
wit extract /path/to/RMCP01.iso mariokartwii
```

the scripts expect `mariokartwii/DATA/sys/main.dol` and `mariokartwii/DATA/files/`. if your extractor didn't make the `DATA/` folder, move `sys` and `files` into one. `mariokartwii/` is gitignored.

then point the translator at it. this makes symlinks, so nothing gets copied:

```sh
./scripts/link-mkw-assets.sh
```

## 3. translate the game

this is the same pipeline as upstream wiicompiled (see `upstream/docs/building-macos.md`). it only has to run once (and again if you ever change the dump), and the output (~400 MB) stays in `upstream/generated/`.

```sh
cd upstream
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release
translator() { dotnet "$(pwd)/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll" "$@"; }

mkdir -p generated/functions build/base generated/build_shards

translator translate-recursive 0x800060A4 \
  --project projects/mkwii/recomp.yml \
  --outdir generated/functions \
  --output-metadata generated/base_translation_output.json \
  --production-source-bundle generated/base_translation_sources.bin \
  --no-function-files --prune-stale --threads $JOBS

translator emit-base-manifest \
  --project projects/mkwii/recomp.yml \
  --out build/base \
  --functions-dir generated/functions \
  --translation-output-metadata generated/base_translation_output.json \
  --region P

translator generate-data-init --project projects/mkwii/recomp.yml

translator emit-build-shards \
  --project projects/mkwii/recomp.yml \
  --base-metadata generated/base_translation_output.json \
  --base-functions-dir generated/functions \
  --native-source-dir runtime/src \
  --out generated/build_shards

cd ..
```

retro rewind isn't supported on the Vita yet, so use the base-game shards only.

## 4. build the VPK

```sh
cmake -S . -B build-vita -D MKWV_BUILD_VITA=ON
cmake --build build-vita -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

the first build is the slow one. the translated game is split into ~74 large shards, and each takes the compiler around half a minute and about 400 MB of RAM. on a machine with less than 16 GB, give `-j` a lower number than your core count. without `-j` at all, CMake builds them one at a time and it takes well over an hour.

you end up with `build-vita/vita/wiicompiled-vita.vpk` (title id `MKWV00001`).

after that, if you only touched the port's own code, rebuild with:

```sh
./scripts/build-vita-fast.sh
```

that skips recompiling the shards. if you changed a header the translated code depends on, it refuses and tells you to do a full build. listen to it: forcing it past that gives you a binary that crashes in confusing ways.

## 5. put the game data on the Vita

the VPK only has the port in it. the game data goes in `ux0:data/viicompiled/`:

| on the Vita | from |
| --- | --- |
| `ux0:data/viicompiled/disc/` | everything inside `mariokartwii/DATA/files/` |
| `ux0:data/viicompiled/sys/` | everything inside `mariokartwii/DATA/sys/` (needs `fst.bin`) |
| `ux0:data/viicompiled/guest/` | the `.bin` files from `upstream/generated/data_sections_init_blobs/` |

`./scripts/setup-ux0-data.sh` builds that same tree locally under `ux0-data/viicompiled` (and in Vita3K's folder if it finds one). the result is symlinks though, so copy the real files over with VitaShell's FTP or USB mode.

then install the VPK and launch it.

## if something goes wrong

the port writes `ux0:data/viicompiled/boot.log` (startup) and `ux0:data/viicompiled/watchdog.log` (per-second perf lines, profiler samples, hangs). attach both to an issue, and say which commit you built. crash dumps in `ux0:data/` can be read with `scripts/parse-vita-core.sh` against `build-vita/vita/wiicompiled-vita`, the unstripped ELF from the same build.

## toggles

behaviour can be switched by creating empty files in `ux0:data/viicompiled/` and restarting. the one worth knowing about:

- `perf_turbo` renders at 0.75 resolution, turns effects off and lowers the sound budget. it looks worse but runs faster.

the rest are mostly A/B switches for testing a single optimization (`no_native_engine`, `no_dl_cache`, `no_vbo_ring` and others). they're documented with the round that added them in [handoff.md](handoff.md) and [docs/](docs).

## tests

the host tests don't need a Vita or any game files:

```sh
cmake -S . -B build-host -D MKWV_BUILD_VITA=OFF
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
```
