# Guest driver objects: the same adapter sources as native.mk, compiled with
# the miniBox musl toolchain and the flags CMake gave the guest libraries.
# build-guest.sh must have run first (build/guest holds the archives and
# compile_commands.json); build-core.sh links core.wbx from all of it.
#
# Usage: make -f guest.mk -j$(nproc)

ROOT   := ..
B      := $(ROOT)/build/guest
O      := obj-guest
MB     ?= $(HOME)/chimera/extern/tools/chimera-common-minibox
MBUILD := $(MB)/build/meson-cpp
SR     := $(MBUILD)/guest-sysroot
GCCVER := $(shell gcc -dumpfullversion)

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Emu/System.cpp)

WBFLAGS := -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector \
        -fno-pic -fno-pie -fcf-protection=none -O2 -msse -msse2 -mcx16 -fno-exceptions
SPECS   := -specs $(SR)/lib/musl-gcc.specs
CXXINCS := -nostdinc++ -I$(SR)/include/c++/$(GCCVER) -I$(SR)/include/c++/$(GCCVER)/x86_64-linux-musl
MBINCS  := -I$(MB)/extern/emulibc -I$(MB)/source/guest/include -I$(MB)/extern/jsmn

CXXFLAGS := $(WBFLAGS) $(TUFLAGS) -DCHIMERA_GUEST -DCHIMERA_CORE -DCHIMERA_NO_TLS $(MBINCS) -I. -isystem guest-include $(CXXINCS)
CFLAGS   := $(WBFLAGS) -DCHIMERA_GUEST -DCHIMERA_CORE

# upstream sources that live in rpcs3's user-interface library but are not
# user interface (the same list as native.mk)
UPSTREAM_TUS := Input/pad_thread.cpp Input/product_info.cpp Input/ps_move_tracker.cpp Input/ps_move_config.cpp rpcs3_version.cpp
UPSTREAM_OBJS := $(patsubst %.cpp,$(O)/upstream/%.o,$(UPSTREAM_TUS))

OBJS := $(O)/rpcs3-driver.o $(O)/host-stubs.o $(O)/memfs.o $(O)/wbx-entry.o $(O)/guest-syscalls.o $(O)/vsched.o $(UPSTREAM_OBJS)

all: $(OBJS)

$(O)/upstream/%.o: $(ROOT)/extern/rpcs3/rpcs3/%.cpp
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/%.o: %.cpp rpcs3-driver.h vsched.h
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/vsched.o: vsched.cpp vsched.h
	@mkdir -p $(O)
	g++ $(SPECS) $(WBFLAGS) -I. $(CXXINCS) -c -o $@ $<

clean:
	rm -rf $(O)

.PHONY: all clean
