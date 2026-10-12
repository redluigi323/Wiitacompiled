# building

> **heads up:** the game isn't fully ready. the framerate will be unideal: a race runs at around 20fps, and grand prix runs lower until you get into 1st. if you're willing to sit through that, it's playable minus some small renderer glitches. don't build this expecting a finished port.

the build runs on **macOS**, **Linux**, and **Windows through WSL2**. the scripts are bash, so on Windows everything below happens inside a WSL2 terminal, not PowerShell or Git Bash.

## what you need

**on your computer**

- [vitasdk](https://vitasdk.org), with `VITASDK` set (the scripts also find it at `/usr/local/vitasdk`)
- CMake 3.16+, git, Python 3, and a normal C/C++ toolchain for vitasdk's tools
- the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0), for wiicompiled's translator
- [wit](https://wit.wiimm.de), or anything else that can extract a Wii disc to a folder
- a clean PAL Mario Kart Wii image (`RMCP01`). other regions and modified dumps get rejected by the translator.
- around 4 GB of free disk space, plus about 0.5 GB of RAM for each parallel compile job

**on your Vita**

- HENkaku/Ensō with VitaShell, or whatever you use to move files and install VPKs
- `libshacccg.suprx` at `ur0:data/libshacccg.suprx`. dump it from your own firmware with [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D). without it nothing renders, and `boot.log` will say it's missing.

the app requests 444 MHz on its own. if a kernel overclock plugin lets it, it takes 500.

## 0. setting up your machine

pick the one you're on, then install vitasdk with vdpm (shown at the end of this section).

### macOS

```sh
xcode-select --install
brew install cmake python git wget
brew install --cask dotnet-sdk@8
```

### Linux (Debian/Ubuntu)

```sh
sudo apt update
sudo apt install build-essential cmake git python3 python3-venv curl wget xz-utils bzip2 libarchive-tools
sudo apt install dotnet-sdk-8.0
```

if your release doesn't ship `dotnet-sdk-8.0`, use [Microsoft's install script](https://learn.microsoft.com/dotnet/core/install/linux-scripted-manual). Fedora and Arch have `dotnet-sdk-8.0` in their own repos.

### Windows (WSL2)

in an admin PowerShell, install WSL2 with Ubuntu and restart when it asks:

```powershell
wsl --install -d Ubuntu
```

then open the Ubuntu terminal and follow the Linux steps above.

**clone and build inside the WSL filesystem** (for example `~/Wiitacompiled`), not on `C:` (`/mnt/c/...`). compiling through `/mnt/c` is several times slower, and `build-vita.sh` warns you if you try. if you want to reach the files from Windows, they're at `\\wsl$\Ubuntu\home\<you>\Wiitacompiled` in Explorer.

your disc image can stay on Windows. pass its `/mnt/c/...` path to wit.

### vitasdk (all three)

```sh
git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh
./install-all.sh
echo 'export VITASDK=/usr/local/vitasdk' >> ~/.bashrc   # ~/.zshrc on macOS
echo 'export PATH="$VITASDK/bin:$PATH"' >> ~/.bashrc
```

open a new terminal afterwards. `./scripts/bootstrap-vitasdk.sh` in this repo checks that it worked.

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
./scripts/link-mkw-assets.sh
```

the scripts expect `mariokartwii/DATA/sys/main.dol` and `mariokartwii/DATA/files/`. if your extractor didn't make the `DATA/` folder, move `sys` and `files` into one. `mariokartwii/` is gitignored.

`link-mkw-assets.sh` points the translator at the dump with symlinks, so nothing gets copied. it prints the hashes of `main.dol` and `StaticR.rel`.

## 3. translate the game

```sh
./scripts/translate-game.sh
```

this runs wiicompiled's translator over your dump: it builds the translator, translates the game, then generates the guest data and the build shards (base game only, retro rewind isn't supported on the Vita yet). it only has to run once, and again if you ever change the dump. the output (~400 MB) stays in `upstream/generated/`. `JOBS=N` limits the threads it uses.

## 4. build the VPK

```sh
./scripts/build-vita.sh
```

the first build is the slow one. the translated game is split into ~74 large shards, and each takes the compiler around half a minute and about 0.5 GB of RAM. the script uses all your CPU threads but caps the job count by RAM. `JOBS=N` overrides that. extra arguments go to the CMake configure step.

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

copy them over with VitaShell's FTP or USB mode, then install the VPK and launch it.

`./scripts/setup-ux0-data.sh` builds that same tree locally under `ux0-data/viicompiled`. if it finds Vita3K (in its usual place on macOS, Linux or Windows, or wherever `VITA3K_UX0_DATA` points), it fills Vita3K's `ux0` too. it uses symlinks, except for a Windows install of Vita3K reached from WSL: Windows can't follow WSL symlinks, so there it copies the files instead. that takes a couple of GB.

## if something goes wrong

the port writes `ux0:data/viicompiled/boot.log` (startup) and `ux0:data/viicompiled/watchdog.log` (per-second perf lines, profiler samples, hangs). attach both to an issue, and say which commit you built. crash dumps in `ux0:data/` can be read with `scripts/parse-vita-core.sh` against `build-vita/vita/wiicompiled-vita`, the unstripped ELF from the same build.

## toggles

behaviour can be switched by creating empty files in `ux0:data/viicompiled/` and restarting. the one worth knowing about:

- `perf_turbo` renders at 0.75 resolution, turns effects off and lowers the sound budget. it looks worse but runs faster.

the rest are mostly A/B switches for testing a single optimization. [handoff.md](handoff.md) lists all of them.

## tests

the host tests don't need a Vita or any game files. the renderer (gx) tests build anywhere; the runtime tests currently only build on Apple Silicon (on x86-64 the paired-single SSE code doesn't compile yet):

```sh
cmake -S . -B build-host -D MKWV_BUILD_VITA=OFF
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
```
