#!/bin/sh
# Links core.wbx - rpcs3 as a chimera waterbox core - from the guest CMake
# archives, the musl ffmpeg and the adapter objects, and builds run-wbx, the
# host driver the equivalence gate uses.
#
# Prereq: ./build-guest.sh (the archives), ./build-ffmpeg.sh guest, and a
# miniBox checkout built WITH the C++ guest toolchain:
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true
#   ninja -C <miniBox>/build/meson-cpp
#
# Usage: ./build-core.sh [-m <miniBox dir>] [-o <output dir>] [-j N]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/tools/chimera-common-minibox}"
out="$here/bin"
jobs="$(nproc)"
while getopts "m:o:j:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		o) out="$OPTARG" ;;
		j) jobs="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"

[ -f "$sr/lib/libstdc++.a" ] || {
	echo "miniBox C++ guest toolchain missing at $sr." >&2
	exit 1
}
[ -d "$root/build/guest" ] || {
	echo "build/guest missing - run build-guest.sh first." >&2
	exit 1
}
[ -d "$root/build/ffmpeg-guest/lib" ] || {
	echo "build/ffmpeg-guest missing - run build-ffmpeg.sh guest first." >&2
	exit 1
}

# the guest half of the GPU bridge, generated from miniBox's master list in
# full: rpcs3 reaches GL through glad, which asks for every name it knows
mkdir -p "$here/generated-gl"
python3 "$mb/source/gl/gen-gl-bridge.py" "$here/glad/include/glad/gl.h" \
	"$mb/source/gl/gl-entry-points.txt" "$here/generated-gl"

make -f "$here/guest.mk" -C "$here" -j"$jobs" MB="$mb"

mkdir -p "$out"

# Library order: one big group; the CMake archives cross-reference freely.
libs="$(find "$root/build/guest" -name '*.a' | sort | tr '\n' ' ') $(find "$root/build/ffmpeg-guest/lib" -name '*.a' | sort | tr '\n' ' ') $(find "$root/build/llvm-guest/lib" -name 'libLLVM*.a' 2>/dev/null | sort | tr '\n' ' ')"

g++ -specs "$sr/lib/musl-gcc.specs" -mcmodel=large -fno-pic -fno-pie \
	-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax,-z,stack-size=8388608 -T "$mb/source/guest/linkscript.T" \
	-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create \
	-Wl,-u,pthread_mutexattr_init -Wl,-u,pthread_mutexattr_settype -Wl,-u,pthread_mutexattr_destroy \
	-o "$out/core.wbx" \
	"$here"/obj-guest/wbx-entry.o "$here"/obj-guest/rpcs3-driver.o "$here"/obj-guest/memfs.o \
	"$here"/obj-guest/host-stubs.o "$here"/obj-guest/host-plumbing.o "$here"/obj-guest/guest-syscalls.o "$here"/obj-guest/vsched.o \
	"$here"/obj-guest/upstream/Input/pad_thread.o "$here"/obj-guest/upstream/Input/product_info.o \
	"$here"/obj-guest/upstream/Input/ps_move_tracker.o "$here"/obj-guest/upstream/Input/ps_move_config.o \
	"$here"/obj-guest/upstream/rpcs3_version.o \
	"$here"/obj-guest/gl-shim.o "$here"/obj-guest/gl-bridge-guest.o "$here"/obj-guest/gl-traps.o "$here"/obj-guest/glad-gl.o "$here"/obj-guest/generated-assets.o \
	"$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
	-Wl,--start-group $libs -Wl,--end-group \
	-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc
sh "$mb/source/guest/check-wbx.sh" "$out/core.wbx"
echo "built $out/core.wbx"

# host driver for the gate
# (MINIBOX_HOST_DIR overrides where libminiboxhost.so comes from: the PS3
# needs a host at spec v2, the multi-region block)
mbhost="${MINIBOX_HOST_DIR:-$mb/build/meson-linux/source/host}"
[ -f "$mbhost/libminiboxhost.so" ] || mbhost="$mbuild/source/host"
gcc -O2 -Wall -DCHIMERA_GL_BRIDGE -I"$here" -I"$mb/source/host" -I"$mb/source/gl" -I"$mb/source/cache" \
	-I"$here/glad/include" -I"$here/generated-gl" \
	-o "$out/run-wbx" "$here/run-wbx.c" "$here/gl-host.c" "$here/cache-host.c" "$here/glad/src/gl.c" \
	"$mbhost/libminiboxhost.so" -Wl,-rpath,"$mbhost" -lEGL
echo "built $out/run-wbx"
