#!/bin/sh
# Native reference build: rpcs3's own CMake, host toolchain, the canonical
# option set. Artifacts land in build/native.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -G Ninja -B "$root/build/native" $RPCS3_OPTS -DCMAKE_CXX_FLAGS="-DCHIMERA_CORE -I$here" "$root/extern/rpcs3"
ninja -C "$root/build/native" rpcs3_emu Fusion "$@"
