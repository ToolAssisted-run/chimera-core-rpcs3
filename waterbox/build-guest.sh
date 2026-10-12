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
# The Vulkan renderer, for the guest only (chimera issue 248). CMake's search
# for Vulkan is given what it would have found: one folder of headers - the
# two miniBox carries, a vulkan.h that is those two (waterbox/vk/vulkan), and
# volk's - and a library that exists. Nothing is linked from it: in the box
# the commands are the guest half of miniBox's Vulkan bridge (build-core.sh).
mb="${MINIBOX_DIR:-$HOME/chimera/extern/chimera-common-minibox}"
[ -f "$mb/extern/vulkan-headers/include/vulkan/vulkan_core.h" ] || {
	echo "miniBox at $mb has no Vulkan headers (extern/vulkan-headers): it predates the Vulkan bridge. Use Chimera 4ca6e30 or later." >&2
	exit 1
}
vkinc="$root/build/vk-include"
mkdir -p "$vkinc"
cp -rp "$mb/extern/vulkan-headers/include/." "$vkinc/"
cp -p "$here/vk/vulkan/vulkan.h" "$vkinc/vulkan/vulkan.h"
cp -p "$here/vk/volk.h" "$vkinc/volk.h"
vk_opts="-DUSE_VULKAN=ON -DVulkan_INCLUDE_DIR=$vkinc -DVulkan_LIBRARY=$mb/build/meson-cpp/guest-sysroot/lib/libc.a"

cmake -G Ninja -B "$root/build/guest" -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" $RPCS3_OPTS $vk_opts -DLLVM_DIR="$root/build/llvm-guest/lib/cmake/llvm" -DPKG_CONFIG_EXECUTABLE=/bin/false -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DUSE_SYSTEM_FFMPEG=ON -DFFMPEG_INCLUDE_DIR="$ffmpeg_inc" -DFFMPEG_LIBRARIES=avcodec "$root/extern/rpcs3"
ninja -C "$root/build/guest" rpcs3_emu Fusion "$@"
