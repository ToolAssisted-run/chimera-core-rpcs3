# Building the RPCS3 core

This repository builds one file, `rpcs3.chimeraCore`: RPCS3 compiled as a
sandboxed guest (`core.wbx`) together with the declarations Chimera reads.
The steps below are the ones `.github/workflows/chimera.yml` runs from a fresh
clone on a public Ubuntu runner. This is the heaviest build of the Chimera
cores: LLVM is built twice (host and guest) and RPCS3's emulator library twice.

Placeholders used below:

- `<core>` - the checkout of this repository. Commands run from it unless
  stated otherwise.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, the sandbox host and
  the guest toolchain.

## Requirements

Operating system: CI uses the `ubuntu-latest` runner. Cores are built on Linux
only. The package that comes out runs on Linux and on Windows.

System packages, as the `core-gate` job installs them:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3 mono-complete libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev g++-13
```

The `frontend-gate` job installs the same list without `g++-13` and with
`unzip` and `xvfb` added. You need those two only to run the frontend gate.

Compiler: GCC 13, pinned.

- `g++-13` is installed by name. g++ 14 cannot build this: it stops with an
  internal compiler error in abseil's `any_invocable.h`.
- `waterbox/guest-toolchain.cmake` picks `gcc-13` and `g++-13` for the guest
  when they exist, and falls back to `gcc` and `g++`.
- CI passes `CXX=g++-13` to `waterbox/native.mk` for the native reference.
- The other steps use the default `gcc` and `g++`. The workflow relies on the
  runner's default being GCC 13.

.NET: the workflow uses `actions/setup-dotnet@v4` with `dotnet-version: '8.0'`.
By hand, install the .NET SDK 8.0. Chimera's README gives the command it
expects: `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`.
.NET and `mono-complete` are needed for Chimera's contract tests and for the
frontend gate, not for building the package.

Built from source by the scripts, never taken from the system:

- LLVM 22, from RPCS3's own submodule (`extern/rpcs3/3rdparty/llvm`), X86
  target only, static, no threads. One build for the host, one for the guest.
- FFmpeg, from the `extern/ffmpeg` submodule, configured the same way for both
  flavours: no assembly, no programs, no network, decoders only.
- RPCS3's third-party libraries, from RPCS3's submodules.

Downloaded by the build: when miniBox builds its C++ guest toolchain it fetches
the GCC source that matches the host compiler (about 84 MB, with `curl`) to
build libstdc++ for the guest. `curl` is not in this workflow's package list;
make sure it is installed. Nothing else is downloaded.

Time: the workflow gives the `core-gate` job 360 minutes and says a cold build
takes hours on a runner. CI keeps `build/llvm-native`, `build/llvm-guest`,
`build/native` and the miniBox build directories between runs with
`actions/cache@v4`. By hand there is nothing to do: a local build keeps them
as long as you do not delete `build/`.

Sources: Chimera is taken at its `main` branch (`CHIMERA_REF` in the
workflow). RPCS3, its LLVM and FFmpeg are whatever commits the submodules
point at; `git submodule status` shows them.

## Get the sources

Clone this repository with its submodules, recursively. The workflow does it
with `actions/checkout@v6` and `submodules: recursive`:

```sh
git clone --recurse-submodules https://github.com/ToolAssisted-run/chimera-core-rpcs3.git <core>
```

Recursive is required. RPCS3 keeps its third parties, LLVM among them, as
submodules of the submodule. One level of init leaves
`extern/rpcs3/3rdparty/llvm/llvm` an empty directory and the build stops there.
In an existing clone: `git submodule update --init --recursive`.

Get Chimera and the sources it is built from (this includes miniBox):

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
cd <chimera>
git submodule update --init --recursive extern
```

CI checks out Chimera's `main` branch into `chimera-checkout` inside the core
checkout and passes that path to every script.

Where the scripts look when you pass nothing:

- `waterbox/build-package.sh` looks for Chimera at `../chimera` beside this
  repository, then at `$HOME/chimera`. `-r <chimera>` names it.
- miniBox is `<chimera>/extern/chimera-common-minibox`. `-m <miniBox>` or the
  `MINIBOX_DIR` variable names another place.
- The lower-level scripts and makefiles (`build-core.sh`, `build-ffmpeg.sh`,
  `guest-toolchain.cmake`, `guest.mk`, `native.mk`) fall back to
  `$HOME/chimera/extern/chimera-common-minibox`. `build-package.sh` exports
  `MINIBOX_DIR` and `MINIBOX_SYSROOT` for them. When you run one of them by
  hand, set those variables (or `MB=` for the makefiles) yourself.

## Build miniBox

Two build directories: the host library, and the C++ guest toolchain (musl and
libstdc++ in a guest sysroot).

```sh
mb=<chimera>/extern/chimera-common-minibox
meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

CI skips each `meson setup` when the directory's `build.ninja` is already there
(restored from its cache).

## Build the core

### Patches

`patches/` holds the numbered patch series against `extern/rpcs3` (0001 to
0049). `waterbox/apply-patches.sh` applies it, and `build-guest.sh` and
`build-native.sh` both run it first, so there is normally nothing to do by
hand. The series is judged as a whole:

- a pristine submodule gets every patch, in order;
- a tree that already carries the whole series is left alone
  (`already applied: all N patches`);
- anything in between is an error that names the files and prints the command
  that starts again from the submodule's HEAD;
- a series that does not apply to the submodule's HEAD is an error before the
  tree is touched.

`extern/ffmpeg` is not patched.

### The guest

One command builds everything the package needs and writes the package. It is
the command CI runs:

```sh
MINIBOX_DIR="$mb" ./waterbox/build-package.sh -m "$mb" -r <chimera>
```

It runs these steps. The first three run only when their output directory is
missing:

1. `sh waterbox/build-ffmpeg.sh guest` - FFmpeg for the guest, into
   `build/ffmpeg-guest`.
2. `sh waterbox/build-llvm.sh tblgen` and `sh waterbox/build-llvm.sh guest` -
   the table generator from the native LLVM flavour (`build/llvm-native`), then
   the guest LLVM (`build/llvm-guest`).
3. `sh waterbox/build-guest.sh` - applies the patches, configures RPCS3's own
   CMake with `waterbox/guest-toolchain.cmake` and the options in
   `waterbox/configure-flags.sh`, and builds `rpcs3_emu` and `Fusion` into
   `build/guest`.
4. `sh waterbox/build-core.sh -m "$mb"` - always. Generates
   `waterbox/generated-gl` from miniBox's GL entry list, compiles the adapter
   (`waterbox/guest.mk`), links `waterbox/bin/core.wbx`, runs miniBox's
   `check-wbx.sh` on it, and builds `waterbox/bin/run-wbx`, the host driver
   the gates use.

`build-core.sh` accepts `-m <miniBox dir>`, `-o <output dir>` and `-j N`.

### The native reference

RPCS3 built with the host toolchain, plus a driver (`run-native`). The gate
compares the sandboxed core against it, and it is where debugging is easiest.
A package does not contain it. Build it after the guest: its makefile consumes
`waterbox/generated-gl`, which the guest build generates.

```sh
sh waterbox/build-ffmpeg.sh native
sh waterbox/build-llvm.sh native
sh waterbox/build-native.sh
make -C waterbox -f native.mk -j"$(nproc)" CXX=g++-13 MB="$mb"
```

The result is `waterbox/obj-native/run-native`. Keep `-C waterbox` before
`-f native.mk`, and always pass `MB=`: the makefile's default is under `$HOME`.

### What CI does not build

- The `frontend-gate` job compiles no RPCS3, no LLVM, no FFmpeg, no native
  reference and no C++ guest toolchain. It downloads the package and `run-wbx`
  that `core-gate` produced, unzips `core.wbx` out of the package, and builds
  only Chimera (its natives and its solution) and the miniBox host library.
- The `core-gate` job builds Chimera's natives but not the Chimera solution.
- The PS3 test programs are not rebuilt. The ones the legs boot are committed
  under `tests/roms/` or assembled by the gate from `tests/asm` with
  `tests/asm/ppc.py`. `tests/ps3/build.sh` rebuilds `flip.elf`, `tone.elf` and
  `sputest.elf` and needs the PSL1GHT toolchain, which CI does not have.
- Nothing is built for Windows. There is no Windows job.

## Build the package

```
waterbox/build-package.sh [-m <miniBox dir>] [-r <chimera root>]
```

There is no `-o` option. The script stages `core.wbx`, `waterbox.config`,
`default_keybinds.json`, `file_slots.json`, the licence texts named by
`waterbox/package-licenses.json` and a `build.json` that records what built
the package, then zips them with sorted entries and fixed timestamps. It packs
twice and stops if the two SHA-1 values differ.

Version stamp, written into the packaged `waterbox.config`:

- CI sets `CORE_VERSION` to the commit:
  `CORE_VERSION=<commit> ./waterbox/build-package.sh -m "$mb" -r <chimera>`.
- Without `CORE_VERSION` the script stamps `<commit>+local`, or
  `<commit>-dirty+local` when the tree has changes. The build patches
  `extern/rpcs3` in place, which counts as a change, so a hand build normally
  says `-dirty`.
- `versionDate` is the commit's date in UTC, never the build's.

A hand-built package is for testing. Chimera's publishing script refuses a
version that carries `+local` or `-dirty`.

The file lands at `<chimera>/build/Cores/rpcs3.chimeraCore`. The script also
removes `<chimera>/build/CoreCache/rpcs3-*`, so the next load unpacks the new
build.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets into Chimera because somebody puts the file in its `Cores` folder.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`,
  and `build-package.sh -r <chimera>` has already written the package there.
- In a release bundle, copy `rpcs3.chimeraCore` into the `Cores` folder beside
  `Chimera.exe`, or into the folder chosen in File > Core Manager >
  Change folder... The same file works on Linux and on Windows.
- File > Core Manager lists what is in the folder. Refresh List rescans it.

Published builds are on this repository's Releases page: a rolling `dev`
release on every green push to `main`, and a dated `nightly-YYYY-MM-DD` release
from the scheduled run when `main` moved since the last one. Their asset is
named `rpcs3-<version>.chimeraCore`.

## Run the gates

### The sandbox check

```sh
sh "$mb/source/guest/check-wbx.sh" waterbox/bin/core.wbx
```

Proves the guest binary is one the sandbox accepts (no thread-local storage,
nothing addressed below the stack pointer). `build-core.sh` already runs it.

### The core gate

```sh
sh waterbox/run-gate.sh
```

Needs `waterbox/obj-native/run-native` (the gate fails without it) and
`waterbox/bin/core.wbx` with `waterbox/bin/run-wbx` (the sandbox legs SKIP
without them). `FRAMES` sets the frame count (default 600). Output goes to
`waterbox/work/gate`. Every leg prints PASS, FAIL or SKIP, the script ends
with `gate: N passed, M failed, K skipped`, and it exits non-zero when a leg
failed. Only one gate runs at a time: a second run waits for the first.

Legs that need no content, and so run in CI:

- `native:deterministic` - two native runs give the same memory and TTY
  digests.
- `sandbox:equivalent` - the sandboxed core gives the native run's lines.
- `sandbox:rewind` - save mid-run, finish, load, finish again: equal.
- `sandbox:rerecord` - a save and load around every frame changes nothing.
- `sandbox:reservation-held` - `rsrvtest.elf`, assembled by the gate, runs to
  its end: a CPU waiting for a locked reservation line gives way to the
  holder.

Every other leg needs `tests/roms-local/PS3UPDAT.PUP` and prints SKIP by name
without it. With the firmware alone the legs over the committed test programs
run (`firmware:lle`, `input:*`, `video:flip`, `rsx:held-flip`, `rsx:windows`,
`audio:tone`, `ppu:llvm`, `cache:*`, `spu:*`). The rest need more, all of it
the user's own and all under the gitignored `tests/roms-local/`:

- `disc:boot`, `disc:install`, `gpu:disc`, `gpu:rewind`, `gpu:context` - a
  decrypted `.iso`.
- `disc:install:encrypted`, `disc:install:state` - an `.iso` with its `.dkey`
  beside it.
- `pkg:boot`, `pkg:content`, `pkg:licence` - `.pkg` files, and a `.rap` for
  the licence leg.
- `rsx:drain`, `spu:barrier` - an `.iso` in `tests/roms-local/rsx-drain/`.
- `rsx:frame-limit` - a `.zip` or `.iso` in `tests/roms-local/frame-limit/`.
- The `gpu:*` legs also need a GL context from the host (EGL; llvmpipe will
  do) and SKIP when there is none.

CI has no firmware and no disc, so on a runner only the content-free legs run.
`docs/PLAN.md`, "What CI runs, and what it does not", says who runs the others
and when.

### The contract tests

Chimera's own tests, run against the package just built. They need Chimera's
natives, because they open the package through the engine:

```sh
cd <chimera>
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
CHIMERA_CORES_DIR="$PWD/build/Cores" dotnet test source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj \
  -c Release --nologo \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

They prove the package is readable, is built for an ABI this frontend runs,
becomes a working factory, binds only buttons its controller declares, and
stamps a version. No game and no firmware are involved.

### The frontend gate

Runs the package inside Chimera, headless under Mono on a private Xvfb
display. It needs the Chimera solution as well as the natives:

```sh
cd <chimera>
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
cd <core>
./waterbox/tests/run-frontend.sh --chimera-root <chimera>
```

It also accepts `--frames N` (default 200). It needs
`<chimera>/build/Chimera.exe`, the installed package and an executable
`waterbox/bin/run-wbx`. It ends with `N ok, M failed, K skipped`.

Two legs need no content and run in CI:

- `game:frontend` - `tests/roms/lv2test.elf` booted from the package in
  Chimera; the first 1 MiB of main memory after the run must equal the sandbox
  reference's (`run-wbx`), byte for byte.
- `keybinds` - the package's default bindings became the frontend's.

The other legs need the firmware, and most a disc, and SKIP without them.

## Files the core needs at run time

None of these is in the repository or in the package. The user provides them.

Firmware, declared in `waterbox/waterbox.config`:

- `PS3UPDAT.PUP` - the PlayStation 3 system software update file that Sony
  distributes, version 4.80 or newer. Required when the game is an `.iso` or a
  `.pkg`. A homebrew executable that uses only lv2 syscalls boots without it.

Project files, declared in `waterbox/file_slots.json`:

- Game (exactly one): a PS3 executable (`.elf`, `.self`, `.bin`), a disc image
  (`.iso`), a disc dumped as a folder and packed into one `.zip` or `.7z`, or a
  digital-only title's `.pkg`. A `.7z` must be packed non-solid
  (`7z a -ms=off`); a solid archive is refused.
- Disc key (optional): the `.dkey` or `.key` that decrypts an encrypted disc
  image. Not needed for a decrypted image.
- Packages (any number): `.pkg` files installed before the machine starts.
- Licences (any number): `.rap` files. A `.rap` must keep the name it came
  with.
- Save data (optional): the `.zip` that Emulator > Export Save Data... writes.

## Troubleshooting

- `Can't find LLVM libraries`, or `extern/rpcs3/3rdparty/llvm/llvm` is empty:
  the submodules were not initialised recursively. Run
  `git submodule update --init --recursive`.
- An internal compiler error in abseil's `any_invocable.h`: the compiler is
  g++ 14. Install `gcc-13` and `g++-13`. For the native reference on a machine
  whose default is 14, `docs/PLAN.md` gives
  `CC=gcc-13 CXX=g++-13 sh waterbox/build-native.sh`, and `native.mk` takes
  `CXX=g++-13`.
- `miniBox guest toolchain missing at ...`: build `<miniBox>/build/meson-cpp`
  with `-Dguest_cpp=true` (see "Build miniBox").
- `make` looks for `waterbox/waterbox/native.mk`: put `-C waterbox` before
  `-f native.mk`. `make` changes directory before it looks for the makefile.
- `extern/rpcs3 is partly patched`: the tree is neither pristine nor exactly
  what the series leaves. The script prints the command that resets the
  submodule and applies the series again. That command discards edits made in
  the tree; turn them into a patch first.
- `the series does not apply to the submodule's HEAD at <patch>`: the
  submodule was moved without rebasing the patches.
- A change under `extern/rpcs3` or `patches/` does not reach the package:
  `build-package.sh` runs `build-guest.sh` only when `build/guest` is missing.
  Run it yourself, then package again:
  `MINIBOX_SYSROOT="$mb/build/meson-cpp/guest-sysroot" sh waterbox/build-guest.sh`.
- `check-wbx.sh` refuses the core after the miniBox toolchain's flags changed:
  FFmpeg, LLVM and the emulator library for the guest were built with the old
  flags, and the scripts skip a component whose build directory exists. Remove
  `build/ffmpeg-guest`, `build/llvm-guest` and `build/guest` and build again.
- The gate ends without its summary line when a GL disc leg is killed for
  memory: the kill takes the gate's shell with it. `GATE_SKIP_GPU_DISC=1`
  turns the three GL disc legs into named SKIPs. `docs/PLAN.md` runs sandboxed
  experiments under a memory cap:
  `systemd-run --user --scope --quiet -p MemoryMax=16G -p MemorySwapMax=0 <run-wbx ...>`.
- The frontend gate reports `reference runner error`: `run-wbx` finds
  `libminiboxhost.so` only through the path it was linked against, and the
  gate runs it with `LD_LIBRARY_PATH` unset. Check with
  `ldd waterbox/bin/run-wbx`.
- The contract tests fail to load a native library: build and install
  Chimera's natives first (see "The contract tests").
- `Xvfb not found`: install `xvfb`. The frontend gate starts its own display
  when `DISPLAY` is not set.
