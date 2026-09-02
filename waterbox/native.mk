# The native reference driver: compiles the adapter with EXACTLY the flags
# CMake gave the emulator library (extract-tu-flags.py mines them), links
# against everything build/native produced. Run build-native.sh first.
#
# Usage: make -f native.mk -j$(nproc)

ROOT := ..
B    := $(ROOT)/build/native
O    := obj-native

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Emu/System.cpp)
CXXFLAGS := -O2 -g1 $(TUFLAGS) -msse -msse2 -mcx16 -fno-exceptions -I.
CFLAGS := -O2 -g1 -I.

# the CMake archives plus the source-built ffmpeg (never the prebuilt zip)
LIBS := $(shell find $(B) -name '*.a' | grep -v /3rdparty/ffmpeg/) $(shell find $(ROOT)/build/ffmpeg-native/lib -name '*.a')

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

$(O)/run-native: $(O)/run-native.o $(O)/rpcs3-driver.o $(O)/host-stubs.o $(O)/host-plumbing.o $(O)/memfs.o $(O)/vsched.o $(UPSTREAM_OBJS) $(LIBS)
	g++ -o $@ $(filter %.o,$^) -Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt -lasound

clean:
	rm -rf $(O)

.PHONY: all clean
