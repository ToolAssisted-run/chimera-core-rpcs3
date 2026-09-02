#!/bin/sh
# Builds the PS3 test programs with the PSL1GHT toolchain (ps3toolchain in
# $PS3DEV, default ~/ps3dev) into tests/roms/<name>.elf, stripped the way
# PSL1GHT's own self rule strips (an unstripped PSL1GHT ELF does not run).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
export PS3DEV="${PS3DEV:-$HOME/ps3dev}"
export PSL1GHT="${PSL1GHT:-$PS3DEV}"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH"
for t in flip tone; do
	make -C "$here/$t" --no-print-directory
	powerpc64-ps3-elf-strip "$here/$t/$t.elf" -o "$here/../roms/$t.elf"
	sprxlinker "$here/../roms/$t.elf"
	echo "built tests/roms/$t.elf"
done
