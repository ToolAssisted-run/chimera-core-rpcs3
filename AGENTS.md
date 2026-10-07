# AGENTS.md - RPCS3 core for Chimera

This repository turns RPCS3 (the PlayStation 3 emulator) into a core for
Chimera, a frontend for tool-assisted speedruns. Upstream is the `extern/rpcs3`
submodule, changed only by the numbered patches in `patches/`; `waterbox/`
holds the adapter, the build scripts and the gates. The product is one file,
`rpcs3.chimeraCore`, which Chimera loads and runs inside its sandbox (miniBox)
on Linux and on Windows. Detail for every step below is in `docs/BUILDING.md`.

## Layout

- `extern/rpcs3` - upstream RPCS3, a submodule with submodules (LLVM is one).
  `extern/ffmpeg` - FFmpeg, a submodule, built from source for both flavours.
- `patches/` - the patch series against `extern/rpcs3`, 0001 to 0049.
- `waterbox/apply-patches.sh` - applies the series; judges it as a whole.
- `waterbox/build-package.sh` - builds the guest and writes the package.
  `waterbox/build-{ffmpeg,llvm,guest,core}.sh` - the steps it calls.
- `waterbox/build-native.sh`, `waterbox/native.mk` - the native reference.
- `waterbox/configure-flags.sh` - RPCS3's CMake options, one set for both
  flavours. `waterbox/guest-toolchain.cmake`, `waterbox/guest.mk` - the guest.
- `waterbox/rpcs3-driver.cpp`, `wbx-entry.cpp`, `vsched.cpp`, `memfs.cpp` - the
  adapter: driver, guest ABI, virtual-time scheduler, memory filesystem.
- `waterbox/run-native.cpp`, `waterbox/run-wbx.c` - the two runners compared.
- `waterbox/run-gate.sh`, `waterbox/tests/run-frontend.sh` - the core gate
  and the frontend gate.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json` - what the package declares.
- `tests/asm`, `tests/ps3`, `tests/roms` - the free test programs and their
  sources. `tests/roms-local/` is gitignored: the user's firmware and games.
- `tools/` - turns the RPCS3 wiki's per-game settings into the committed table
  `waterbox/wiki-compat.json`.
- `docs/PLAN.md` - milestones, decisions, and what CI runs.

## Set up the build environment

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3 mono-complete libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev g++-13
# the contract tests need the .NET SDK 8.0; the frontend gate also needs xvfb

# in this repository: RECURSIVE, LLVM is a submodule of the submodule
git submodule update --init --recursive
# Chimera and miniBox (~/chimera is the scripts' fallback)
git clone https://github.com/ToolAssisted-run/chimera.git ~/chimera
git -C ~/chimera submodule update --init --recursive extern
mb=~/chimera/extern/chimera-common-minibox
meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The compiler is GCC 13. g++ 14 fails with an internal compiler error.

## Build

```sh
# from the root of this repository
mb=~/chimera/extern/chimera-common-minibox
MINIBOX_DIR="$mb" ./waterbox/build-package.sh -m "$mb" -r ~/chimera
```

The first run builds FFmpeg, LLVM and RPCS3's emulator library for the guest.
It is long: the workflow says hours on a runner. Later runs skip each of those
when its directory under `build/` exists, and only rebuild the adapter, link
`waterbox/bin/core.wbx` and write the package.

After a change to `patches/`, rebuild the emulator library yourself;
`build-package.sh` will not while `build/guest` exists:

```sh
MINIBOX_SYSROOT="$mb/build/meson-cpp/guest-sysroot" sh waterbox/build-guest.sh
```

The native reference, needed by the gate (build the guest first):

```sh
sh waterbox/build-ffmpeg.sh native
sh waterbox/build-llvm.sh native
sh waterbox/build-native.sh
make -C waterbox -f native.mk -j"$(nproc)" CXX=g++-13 MB="$mb"
```

## Install the core into Chimera

`build-package.sh -r <chimera>` writes `<chimera>/build/Cores/rpcs3.chimeraCore`:
the cores folder of a Chimera source checkout, so nothing else is needed. For
a release bundle, copy the file into the `Cores` folder beside `Chimera.exe`
(or the folder chosen in File > Core Manager > Change folder...). Chimera
downloads nothing; File > Core Manager lists the folder, Refresh List rescans.
A hand build stamps `<commit>+local` (usually `-dirty` too): testing only.

## Test before you commit

```sh
sh "$mb/source/guest/check-wbx.sh" waterbox/bin/core.wbx
sh waterbox/run-gate.sh
```

The gate must end with `0 failed`. Without `tests/roms-local/PS3UPDAT.PUP` it
runs `native:deterministic`, `sandbox:equivalent`, `sandbox:rewind`,
`sandbox:rerecord` and `sandbox:reservation-held`, and prints one SKIP line for
every other leg. That is all CI runs. If the firmware and games are on this
machine the other legs run too; say in the commit what the gate printed.

The package through Chimera (Chimera built first, see `docs/BUILDING.md`):

```sh
(cd ~/chimera && CHIMERA_CORES_DIR="$PWD/build/Cores" dotnet test \
  source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj -c Release --nologo \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests")
./waterbox/tests/run-frontend.sh --chimera-root ~/chimera
```

Run one gate and one sandboxed experiment at a time, under a memory cap
(`docs/PLAN.md`):
`systemd-run --user --scope --quiet -p MemoryMax=16G -p MemorySwapMax=0 <run-wbx ...>`.

## Rules of this repository

- Never commit inside `extern/rpcs3` or `extern/ffmpeg`. A change to RPCS3 is
  a numbered patch in `patches/`, applied by `waterbox/apply-patches.sh`. A
  patch adds a build option or a hook; it does not delete upstream code.
  `extern/ffmpeg` carries no patches.
- The patched tree must be exactly what the whole series leaves behind:
  `apply-patches.sh` refuses anything else. Do not revert single files in the
  submodule by hand.
- Determinism is the product. The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate must
  round-trip. The gate checks it; a change that breaks it is a bug.
- Both flavours must stay the same machine. A new adapter source goes into
  `waterbox/guest.mk`, `waterbox/native.mk` and the link line in
  `waterbox/build-core.sh`.
- Run the gate before committing. A new leg needs a negative control: show it
  fails when the thing it checks is broken, and say so in the commit.
- Never commit game files, firmware, disc keys or licences. They stay in
  `tests/roms-local/`. Never add network access: nothing is fetched at run
  time, and `waterbox/wiki-compat.json` is committed data.
- Shell scripts stay executable (git mode 100755). Documentation prose is
  plain ASCII.
- Commit messages: `type(scope): a full sentence saying what is now true`,
  with `(chimera#N)` when it fixes an issue (issues are filed in the chimera
  repository). Types in use: `fix`, `feat`, `docs`, `ci`, `test`, `build`,
  `data`. The body says what was wrong, what was measured, what the gate said
  and what the negative control was.
- A decision or a finding worth keeping gets a dated section in `docs/PLAN.md`.
- Do not edit `.github/workflows` unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every build step, option and known failure.
- `docs/PLAN.md` - the reasoning; "What CI runs, and what it does not" lists
  the legs and what each needs.
- `.github/workflows/chimera.yml` - the authoritative recipe, reasons included.
- In the Chimera checkout: `docs/porting-a-core.md`, `docs/gates.md` (how a
  gate goes green on a broken thing) and `docs/core-manager.md`.
