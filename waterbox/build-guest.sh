#!/bin/sh
# Guest (waterbox) build: the same CMake and options, under the miniBox musl
# toolchain. Artifacts land in build/guest.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
# ffmpeg: the guest configure only needs the headers (the static libraries are
# linked by guest.mk from the musl build of extern/ffmpeg)
ffmpeg_inc="${CHIMERA_FFMPEG_INCLUDE:-$root/build/ffmpeg-guest/include}"
[ -d "$ffmpeg_inc" ] || ffmpeg_inc="$root/extern/rpcs3/3rdparty/ffmpeg/include"
cmake -G Ninja -B "$root/build/guest" -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" $RPCS3_OPTS -DPKG_CONFIG_EXECUTABLE=/bin/false -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DUSE_SYSTEM_FFMPEG=ON -DFFMPEG_INCLUDE_DIR="$ffmpeg_inc" -DFFMPEG_LIBRARIES=avcodec "$root/extern/rpcs3"
ninja -C "$root/build/guest" rpcs3_emu Fusion "$@"
