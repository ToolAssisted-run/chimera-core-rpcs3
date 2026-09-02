# The native reference driver: compiles the adapter with EXACTLY the flags
# CMake gave the emulator library (extract-tu-flags.py mines them), links
# against everything build/native produced. Run build-native.sh first.
#
# Usage: make -f native.mk -j$(nproc)

ROOT := ..
B    := $(ROOT)/build/native
O    := obj-native

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Emu/System.cpp)
MB      ?= $(HOME)/chimera/extern/tools/chimera-common-minibox
GLINCS  := -I$(MB)/source/gl -Iglad/include -Igenerated-gl
CXXFLAGS := -O2 -g1 $(TUFLAGS) -msse -msse2 -mcx16 -fno-exceptions -DCHIMERA_GL_BRIDGE $(GLINCS) -I.
CFLAGS := -O2 -g1 -I.

# the CMake archives plus the source-built ffmpeg (never the prebuilt zip)
LIBS := $(shell find $(B) -name '*.a' | grep -v /3rdparty/ffmpeg/) $(shell find $(ROOT)/build/ffmpeg-native/lib -name '*.a') $(shell find $(ROOT)/build/llvm-native/lib -name 'libLLVM*.a' 2>/dev/null)

# upstream sources that live in rpcs3's user-interface library but are not
# user interface: the pad thread and the version strings
UPSTREAM_TUS := Input/pad_thread.cpp Input/product_info.cpp Input/ps_move_tracker.cpp Input/ps_move_config.cpp rpcs3_version.cpp
UPSTREAM_OBJS := $(patsubst %.cpp,$(O)/upstream/%.o,$(UPSTREAM_TUS))

all: $(O)/run-native

$(O)/upstream/%.o: $(ROOT)/extern/rpcs3/rpcs3/%.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/%.o: %.cpp rpcs3-driver.h vsched.h
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/vsched.o: vsched.cpp vsched.h
	@mkdir -p $(O)
	g++ -O2 -g1 -I. -c -o $@ $<

# the GPU bridge: the generated guest half (build-core.sh regenerates it from
# miniBox's master list), glad, and the host half with its EGL context
$(O)/gl-bridge-guest.o: generated-gl/gl-bridge-guest.cpp
	@mkdir -p $(O)
	g++ -O2 -g1 $(GLINCS) -c -o $@ $<

$(O)/glad-gl.o: glad/src/gl.c
	@mkdir -p $(O)
	gcc -O2 $(GLINCS) -c -o $@ $<

$(O)/gl-host.o: gl-host.c
	@mkdir -p $(O)
	gcc -O2 -DCHIMERA_GL_BRIDGE $(GLINCS) -c -o $@ $<

$(O)/gl-shim.o: gl-shim.cpp
	@mkdir -p $(O)
	g++ -O2 -g1 $(GLINCS) -c -o $@ $<

generated-gl/gl-traps.cpp: gen-traps.py generated-gl/gl-bridge-ops.h glad/include/glad/gl.h
	python3 gen-traps.py glad/include/glad/gl.h generated-gl/gl-bridge-ops.h $@

$(O)/gl-traps.o: generated-gl/gl-traps.cpp
	@mkdir -p $(O)
	g++ -O1 -c -o $@ $<

generated-assets.cpp: gen-assets.py $(wildcard $(ROOT)/extern/rpcs3/bin/Icons/ui/*.png)
	python3 gen-assets.py $(ROOT)/extern/rpcs3/bin/Icons/ui $@

$(O)/generated-assets.o: generated-assets.cpp chimera-assets.h
	@mkdir -p $(O)
	g++ -O1 -I. -c -o $@ $<

$(O)/run-native: $(O)/run-native.o $(O)/rpcs3-driver.o $(O)/host-stubs.o $(O)/host-plumbing.o $(O)/memfs.o $(O)/vsched.o $(O)/gl-shim.o $(O)/gl-bridge-guest.o $(O)/gl-traps.o $(O)/glad-gl.o $(O)/gl-host.o $(O)/generated-assets.o $(UPSTREAM_OBJS) $(LIBS)
	g++ -o $@ $(filter %.o,$^) -Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt -lasound -lEGL

clean:
	rm -rf $(O)

.PHONY: all clean
