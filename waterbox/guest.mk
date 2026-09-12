# Guest driver objects: the same adapter sources as native.mk, compiled with
# the miniBox musl toolchain and the flags CMake gave the guest libraries.
# build-guest.sh must have run first (build/guest holds the archives and
# compile_commands.json); build-core.sh links core.wbx from all of it.
#
# Usage: make -f guest.mk -j$(nproc)

ROOT   := ..
B      := $(ROOT)/build/guest
O      := obj-guest
MB     ?= $(HOME)/chimera/extern/chimera-common-minibox
MBUILD := $(MB)/build/meson-cpp
SR     := $(MBUILD)/guest-sysroot
GCCVER := $(shell gcc -dumpfullversion)

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Emu/System.cpp)

WBFLAGS := -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector \
        -fno-pic -fno-pie -fcf-protection=none -O2 -msse -msse2 -mcx16 -fno-exceptions
SPECS   := -specs $(SR)/lib/musl-gcc.specs
CXXINCS := -nostdinc++ -I$(SR)/include/c++/$(GCCVER) -I$(SR)/include/c++/$(GCCVER)/x86_64-linux-musl
MBINCS  := -I$(MB)/extern/emulibc -I$(MB)/source/guest/include -I$(MB)/extern/jsmn
GLINCS  := -I$(MB)/source/gl -I$(MB)/source/cache -Iglad/include -Igenerated-gl

CXXFLAGS := $(WBFLAGS) $(TUFLAGS) -DCHIMERA_GUEST -DCHIMERA_CORE -DCHIMERA_NO_TLS -DCHIMERA_GL_BRIDGE $(MBINCS) $(GLINCS) -I. -isystem guest-include $(CXXINCS)
CFLAGS   := $(WBFLAGS) -DCHIMERA_GUEST -DCHIMERA_CORE

# upstream sources that live in rpcs3's user-interface library but are not
# user interface (the same list as native.mk)
UPSTREAM_TUS := Input/pad_thread.cpp Input/product_info.cpp Input/ps_move_tracker.cpp Input/ps_move_config.cpp rpcs3_version.cpp
UPSTREAM_OBJS := $(patsubst %.cpp,$(O)/upstream/%.o,$(UPSTREAM_TUS))

# The 7-Zip reference decoder, which rpcs3 already vendors.
#
# 7zDec.c is here even though sevenzip.cpp never asks it to decode a file: a
# .7z's HEADER is itself usually compressed, and 7zArcIn.c decodes it by calling
# SzAr_DecodeFolder, which lives there. That drags in the filter coders (Bcj2,
# Bra, Delta, Ppmd7) as well. Only the header goes through it - a header is
# small and bounded - while file data is read a piece at a time by our own
# cursor, which is the whole point (see sevenzip.h).
SZDIR  := $(ROOT)/extern/rpcs3/3rdparty/7zip/7zip/C
SZ_SRCS := 7zAlloc.c 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c CpuArch.c \
           LzmaDec.c Lzma2Dec.c Bcj2.c Bra.c Bra86.c BraIA64.c Delta.c Ppmd7.c Ppmd7Dec.c
SZ_OBJS := $(patsubst %.c,$(O)/7z/%.o,$(SZ_SRCS))

OBJS := $(O)/rpcs3-driver.o $(O)/archive.o $(O)/sevenzip.o $(SZ_OBJS) $(O)/host-stubs.o $(O)/host-plumbing.o $(O)/memfs.o $(O)/wbx-entry.o $(O)/guest-syscalls.o $(O)/vsched.o $(O)/gl-shim.o $(O)/gl-bridge-guest.o $(O)/gl-traps.o $(O)/glad-gl.o $(O)/generated-assets.o $(UPSTREAM_OBJS)

all: $(OBJS)

$(O)/upstream/%.o: $(ROOT)/extern/rpcs3/rpcs3/%.cpp
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/%.o: %.cpp rpcs3-driver.h vsched.h
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

# Z7_ST: the guest is one host thread with green threads on it, so the SDK's
# threaded paths have nothing to gain and a thread it cannot have to lose.
$(O)/7z/%.o: $(SZDIR)/%.c
	@mkdir -p $(dir $@)
	gcc $(SPECS) $(CFLAGS) -DZ7_ST -I$(SZDIR) -c -o $@ $<

$(O)/sevenzip.o: sevenzip.cpp sevenzip.h
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -DZ7_ST -I$(SZDIR) -c -o $@ $<

$(O)/vsched.o: vsched.cpp vsched.h
	@mkdir -p $(O)
	g++ $(SPECS) $(WBFLAGS) -I. $(CXXINCS) -c -o $@ $<

# the GPU bridge's guest half: generated wrappers, glad, the shim
$(O)/gl-bridge-guest.o: generated-gl/gl-bridge-guest.cpp
	@mkdir -p $(O)
	g++ $(SPECS) $(WBFLAGS) -DCHIMERA_GUEST $(GLINCS) $(CXXINCS) -c -o $@ $<

$(O)/glad-gl.o: glad/src/gl.c
	@mkdir -p $(O)
	gcc $(SPECS) $(WBFLAGS) -DCHIMERA_GUEST $(GLINCS) -c -o $@ $<

generated-gl/gl-traps.cpp: gen-traps.py generated-gl/gl-bridge-ops.h glad/include/glad/gl.h
	python3 gen-traps.py glad/include/glad/gl.h generated-gl/gl-bridge-ops.h $@

$(O)/gl-traps.o: generated-gl/gl-traps.cpp
	@mkdir -p $(O)
	g++ $(SPECS) $(WBFLAGS) -DCHIMERA_GUEST $(CXXINCS) -c -o $@ $<

$(O)/gl-shim.o: gl-shim.cpp
	@mkdir -p $(O)
	g++ $(SPECS) $(WBFLAGS) -DCHIMERA_GUEST $(GLINCS) $(CXXINCS) -c -o $@ $<

generated-assets.cpp: gen-assets.py $(wildcard $(ROOT)/extern/rpcs3/bin/Icons/ui/*.png)
	python3 gen-assets.py $(ROOT)/extern/rpcs3/bin/Icons/ui $@

$(O)/generated-assets.o: generated-assets.cpp chimera-assets.h
	@mkdir -p $(O)
	g++ $(SPECS) $(WBFLAGS) -I. $(CXXINCS) -c -o $@ $<

clean:
	rm -rf $(O)

.PHONY: all clean
