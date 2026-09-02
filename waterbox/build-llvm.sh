#!/bin/sh
# LLVM 22 from RPCS3's submodule, for the PPU/SPU LLVM recompilers: the
# native flavor here, and (after it, for its table generator) the guest.
#   build-llvm.sh native
# Options: X86 only, no threads (the machine's scheduler is the only
# scheduler), static, no tools, no compression or terminfo libraries.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
flavor="${1:-native}"
src="$root/extern/rpcs3/3rdparty/llvm/llvm/llvm"
out="$root/build/llvm-$flavor"
common="-DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_THREADS=OFF -DLLVM_ENABLE_PIC=OFF
-DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_ENABLE_ASSERTIONS=OFF
-DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
-DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF
-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF
-DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF
-DLLVM_ENABLE_WARNINGS=OFF -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON -DLLVM_USE_INTEL_JITEVENTS=OFF -DLLVM_USE_PERF=OFF
-DLLVM_ENABLE_UNWIND_TABLES=ON -DCMAKE_BUILD_TYPE=Release"
case "$flavor" in
native)
	# the native rpcs3 links -fno-pic objects? no: native rpcs3 is PIE; LLVM static libs need PIC for that
	cmake -G Ninja -S "$src" -B "$out" $common -DLLVM_ENABLE_PIC=ON -DLLVM_INCLUDE_UTILS=ON -DLLVM_BUILD_UTILS=ON \
		-DCMAKE_C_FLAGS="-msse -msse2 -mcx16" -DCMAKE_CXX_FLAGS="-msse -msse2 -mcx16"
	ninja -C "$out" llvm-tblgen
	ninja -C "$out"
	;;
guest)
	native="$root/build/llvm-native"
	[ -x "$native/bin/llvm-tblgen" ] || { echo "build the native flavor first (its llvm-tblgen)" >&2; exit 1; }
	cmake -G Ninja -S "$src" -B "$out" $common -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" \
		-DLLVM_TABLEGEN="$native/bin/llvm-tblgen" -DLLVM_NATIVE_TOOL_DIR="$native/bin" \
		-DLLVM_HOST_TRIPLE=x86_64-linux-musl -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-linux-musl \
		-DCMAKE_CROSSCOMPILING=ON -DHAVE_LIBPTHREAD=ON \
		-DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--defsym=__dso_handle=0 -Wl,--defsym=_dl_find_object=0" -DCMAKE_CXX_STANDARD_LIBRARIES="-lstdc++ -lgcc -lgcc_eh -lc"
	ninja -C "$out"
	;;
esac
echo "built $out"
