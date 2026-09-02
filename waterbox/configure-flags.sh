#!/bin/sh
# The one place the RPCS3 configure options live. Both flavors source this;
# they differ ONLY in toolchain. The emulator library alone (patch 0001), no
# LLVM yet (the interpreters are the machine until the recompiler milestone),
# every host backend off, every external vendored. See docs/PLAN.md.
RPCS3_OPTS="
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
-DCHIMERA_HEADLESS=ON
-DCHIMERA_GL=ON
-DCHIMERA_GLAD_INCLUDE=$root/waterbox/glad/include
-DWITH_LLVM=OFF
-DUSE_NATIVE_INSTRUCTIONS=OFF
-DUSE_LTO=OFF
-DUSE_VULKAN=OFF
-DUSE_SDL=OFF
-DUSE_FAUDIO=OFF
-DUSE_LIBEVDEV=OFF
-DUSE_DISCORD_RPC=OFF
-DUSE_GAMEMODE=OFF
-DUSE_PRECOMPILED_HEADERS=OFF
-DUSE_SYSTEM_CURL=OFF
-DUSE_SYSTEM_ZLIB=OFF
-DUSE_SYSTEM_OPENCV=OFF
-DUSE_SYSTEM_SDL=OFF
-DUSE_SYSTEM_OPENAL=OFF
-DUSE_SYSTEM_FFMPEG=OFF
-DBUILD_RPCS3_TESTS=OFF
-DPNG_LIBCONF_HEADER=$root/extern/rpcs3/3rdparty/libpng/libpng/scripts/pnglibconf.h.prebuilt
"
