# The miniBox waterbox guest as a CMake toolchain: musl + static libstdc++,
# large code model, no PIC, no host libraries. Mirrors the flag set proven by
# the PPSSPP/PCSX2 guest builds.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED ENV{MINIBOX_SYSROOT})
  set(SR "$ENV{HOME}/chimera/extern/chimera-common-minibox/build/meson-cpp/guest-sysroot")
else()
  set(SR "$ENV{MINIBOX_SYSROOT}")
endif()

execute_process(COMMAND gcc -dumpfullversion OUTPUT_VARIABLE GCCVER OUTPUT_STRIP_TRAILING_WHITESPACE)

# The compiler is PINNED to 13 where it exists, as native.mk and CI pin theirs:
# g++ 14 dies with an internal compiler error in abseil's any_invocable.h (the
# NP sources, through protobuf), and treats zstd's undeclared qsort_r - musl
# 1.2.0 has none; nothing links it - as an error rather than a warning. CI's
# runner defaults to 13, so a fresh configure there never met either; a fresh
# configure on a machine whose default is 14 met both.
find_program(GUEST_GCC NAMES gcc-13 gcc NO_CMAKE_FIND_ROOT_PATH)
find_program(GUEST_GXX NAMES g++-13 g++ NO_CMAKE_FIND_ROOT_PATH)
set(CMAKE_C_COMPILER "${GUEST_GCC}")
set(CMAKE_CXX_COMPILER "${GUEST_GXX}")

# This directory, wherever the checkout happens to be. It used to be one
# developer's absolute path: on that machine the headers were found (from the
# WRONG tree, whichever clone that path pointed at), and everywhere else - a
# fresh clone, a submodule, CI - they were not found at all.
set(WBDIR "${CMAKE_CURRENT_LIST_DIR}")

set(WB "-specs=${SR}/lib/musl-gcc.specs -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -DCHIMERA_NO_TLS -DSTBI_NO_THREAD_LOCALS -DZ_TLS= -DCHIMERA_CORE -Dthread_local= -D_Thread_local= -D__thread= -U__cpp_constinit -I${WBDIR} -isystem ${WBDIR}/guest-include")
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
