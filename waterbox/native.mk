# The native reference driver: compiles the adapter with EXACTLY the flags
# CMake gave the emulator library (extract-tu-flags.py mines them), links
# against everything build/native produced. Run build-native.sh first.
#
# Usage: make -f native.mk -j$(nproc)

ROOT := ..
B    := $(ROOT)/build/native
O    := obj-native

# The compiler is NAMED, and overridable, because it matters which one this is:
# g++ 14 cannot build this at all - a hard internal compiler error in abseil's
# any_invocable.h - so build/native here and in CI are both g++-13. Every rule
# below said `g++` literally, which meant CXX= on the command line did nothing
# and the pin could not even be expressed. Set CXX to move it.
CXX ?= g++

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Emu/System.cpp)
MB      ?= $(HOME)/chimera/extern/chimera-common-minibox
GLINCS  := -I$(MB)/source/gl -I$(MB)/source/cache -Iglad/include -Igenerated-gl
CXXFLAGS := -O2 -g1 $(TUFLAGS) -msse -msse2 -mcx16 -fno-exceptions -DCHIMERA_GL_BRIDGE $(GLINCS) -I.
CFLAGS := -O2 -g1 -I.

# the CMake archives plus the source-built ffmpeg (never the prebuilt zip)
LIBS := $(shell find $(B) -name '*.a' | grep -v /3rdparty/ffmpeg/) $(shell find $(ROOT)/build/ffmpeg-native/lib -name '*.a') $(shell find $(ROOT)/build/llvm-native/lib -name 'libLLVM*.a' 2>/dev/null)

# upstream sources that live in rpcs3's user-interface library but are not
# user interface: the pad thread and the version strings
UPSTREAM_TUS := Input/pad_thread.cpp Input/product_info.cpp Input/ps_move_tracker.cpp Input/ps_move_config.cpp rpcs3_version.cpp
UPSTREAM_OBJS := $(patsubst %.cpp,$(O)/upstream/%.o,$(UPSTREAM_TUS))

# The archive readers the memory filesystem grafts a disc dump through, and
# the 7-Zip reference decoder they need - the same list guest.mk builds and
# with the same -DZ7_ST, so both flavors read a disc archive the same way. The
# native reference could not link at all without them once a disc could arrive
# as an archive.
SZDIR   := $(ROOT)/extern/rpcs3/3rdparty/7zip/7zip/C
SZ_SRCS := 7zAlloc.c 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c CpuArch.c \
           LzmaDec.c Lzma2Dec.c Bcj2.c Bra.c Bra86.c BraIA64.c Delta.c Ppmd7.c Ppmd7Dec.c
SZ_OBJS := $(patsubst %.c,$(O)/7z/%.o,$(SZ_SRCS))

all: $(O)/run-native

$(O)/upstream/%.o: $(ROOT)/extern/rpcs3/rpcs3/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(O)/%.o: %.cpp rpcs3-driver.h vsched.h
	@mkdir -p $(O)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(O)/7z/%.o: $(SZDIR)/%.c
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -DZ7_ST -I$(SZDIR) -c -o $@ $<

$(O)/sevenzip.o: sevenzip.cpp sevenzip.h
	@mkdir -p $(O)
	$(CXX) $(CXXFLAGS) -DZ7_ST -I$(SZDIR) -c -o $@ $<

$(O)/vsched.o: vsched.cpp vsched.h
	@mkdir -p $(O)
	$(CXX) -O2 -g1 -I. -c -o $@ $<

# the GPU bridge: the generated guest half (build-core.sh regenerates it from
# miniBox's master list), glad, and the host half with its EGL context
$(O)/gl-bridge-guest.o: generated-gl/gl-bridge-guest.cpp
	@mkdir -p $(O)
	$(CXX) -O2 -g1 $(GLINCS) -c -o $@ $<

$(O)/glad-gl.o: glad/src/gl.c
	@mkdir -p $(O)
	gcc -O2 $(GLINCS) -c -o $@ $<

$(O)/gl-host.o: gl-host.c
	@mkdir -p $(O)
	gcc -O2 -DCHIMERA_GL_BRIDGE $(GLINCS) -c -o $@ $<

$(O)/cache-host.o: cache-host.c $(MB)/source/cache/cache-bridge.h
	@mkdir -p $(O)
	gcc -O2 -I. -I$(MB)/source/cache -c -o $@ $<

$(O)/gl-shim.o: gl-shim.cpp
	@mkdir -p $(O)
	$(CXX) -O2 -g1 $(GLINCS) -c -o $@ $<

generated-gl/gl-traps.cpp: gen-traps.py generated-gl/gl-bridge-ops.h glad/include/glad/gl.h
	python3 gen-traps.py glad/include/glad/gl.h generated-gl/gl-bridge-ops.h $@

$(O)/gl-traps.o: generated-gl/gl-traps.cpp
	@mkdir -p $(O)
	$(CXX) -O1 -c -o $@ $<

generated-assets.cpp: gen-assets.py $(ROOT)/extern/rpcs3/rpcs3/Emu/localized_string_id.h $(ROOT)/extern/rpcs3/rpcs3/rpcs3qt/localized_emu.h $(wildcard $(ROOT)/extern/rpcs3/bin/Icons/ui/*.png)
	python3 gen-assets.py $(ROOT)/extern/rpcs3/bin/Icons/ui $@ $(ROOT)/extern/rpcs3/rpcs3/Emu/localized_string_id.h $(ROOT)/extern/rpcs3/rpcs3/rpcs3qt/localized_emu.h

$(O)/generated-assets.o: generated-assets.cpp chimera-assets.h
	@mkdir -p $(O)
	$(CXX) -O1 -I. -c -o $@ $<

$(O)/run-native: $(O)/run-native.o $(O)/rpcs3-driver.o $(O)/archive.o $(O)/sevenzip.o $(SZ_OBJS) $(O)/host-stubs.o $(O)/host-plumbing.o $(O)/memfs.o $(O)/vsched.o $(O)/gl-shim.o $(O)/gl-bridge-guest.o $(O)/gl-traps.o $(O)/glad-gl.o $(O)/gl-host.o $(O)/cache-host.o $(O)/generated-assets.o $(UPSTREAM_OBJS) $(LIBS)
	$(CXX) -o $@ $(filter %.o,$^) -Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt -lasound -lEGL

clean:
	rm -rf $(O)

.PHONY: all clean
