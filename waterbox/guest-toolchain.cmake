# The miniBox waterbox guest as a CMake toolchain: musl + static libstdc++,
# large code model, no PIC, no host libraries. Mirrors the flag set proven by
# the PPSSPP/PCSX2 guest builds.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED ENV{MINIBOX_SYSROOT})
  set(SR "$ENV{HOME}/chimera/extern/tools/chimera-common-minibox/build/meson-cpp/guest-sysroot")
else()
  set(SR "$ENV{MINIBOX_SYSROOT}")
endif()

execute_process(COMMAND gcc -dumpfullversion OUTPUT_VARIABLE GCCVER OUTPUT_STRIP_TRAILING_WHITESPACE)

set(WB "-specs=${SR}/lib/musl-gcc.specs -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -DCHIMERA_NO_TLS -DSTBI_NO_THREAD_LOCALS -DZ_TLS= -DCHIMERA_CORE -I/home/jaffar/chimera-core-rpcs3/waterbox -isystem /home/jaffar/chimera-core-rpcs3/waterbox/guest-include")
set(CMAKE_C_FLAGS_INIT "${WB}")
set(CMAKE_CXX_FLAGS_INIT "${WB} -nostdinc++ -I${SR}/include/c++/${GCCVER} -I${SR}/include/c++/${GCCVER}/x86_64-linux-musl")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -nostdlib++")

# feature probes must not try to run guest binaries, and full-exe links need
# the wbx entry glue - compile-and-archive is the honest probe
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${SR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
