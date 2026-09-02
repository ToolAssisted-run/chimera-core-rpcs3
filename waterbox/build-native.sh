#!/bin/sh
# Native reference build: rpcs3's own CMake, host toolchain, the canonical
# option set. Artifacts land in build/native.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
# ffmpeg from source (build-ffmpeg.sh native), the same configuration as the
# guest's, so decoded media is flavor-identical; the prebuilt is never used
ffmpeg_inc="$root/build/ffmpeg-native/include"
[ -d "$ffmpeg_inc" ] || { echo "build/ffmpeg-native missing - run build-ffmpeg.sh native first" >&2; exit 1; }
cmake -G Ninja -B "$root/build/native" $RPCS3_OPTS -DUSE_SYSTEM_FFMPEG=ON -DFFMPEG_INCLUDE_DIR="$ffmpeg_inc" -DFFMPEG_LIBRARIES=avcodec -DCMAKE_CXX_FLAGS="-DCHIMERA_CORE -I$here" "$root/extern/rpcs3"
ninja -C "$root/build/native" rpcs3_emu Fusion "$@"
