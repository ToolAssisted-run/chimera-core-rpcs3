// The adapter's C surface. Grown milestone by milestone; the gate harness
// and the waterbox ABI shim are its two callers.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* chimera_rpcs3_error(void);
// work_dir holds the emulator's config/cache/dev_* trees (host directories
// natively, the virtual devices in the box); game_path is what to boot.
// firmware_path (optional): Sony's PS3UPDAT.PUP, decrypted into the machine
// before it boots; without it only HLE-only executables run.
// dkey_path: an optional Redump disc key for an encrypted ISO (M4)
int chimera_rpcs3_init(const char* work_dir, const char* game_path, const char* firmware_path, const char* dkey_path);

// SAVE DATA (chimera docs/save-data.md). What the console keeps under
// /dev_hdd0/home/<user>/savedata - one directory per save, PARAM.SFO, icons and
// the game's own files - lives in the memory filesystem, so it is machine state:
// savestates and rewinds carry it. These take it OUT and put it back IN.
//
// _set_savedata names a zip to seed from, before _init: the very zip an export
// writes (entries "savedata/<save>/<file>"), unpacked before the machine starts
// so it lands in the sealed baseline. An entry that is not save data fails the
// load: progress that is silently ignored is worse than a project that will
// not start.
void chimera_rpcs3_set_savedata(const char* zip_path);
// A .pkg the project carries (the pkg slot, any number), installed by rpcs3's
// own package reader onto /dev_hdd0 before _init seals the machine: DLC,
// patches, unlocks. One that will not install fails the load.
void chimera_rpcs3_add_package(const char* pkg_path);
// A .rap the project carries (the rap slot, any number): the per-account
// licence for a package that was paid for. It is a 16-byte key file named
// after the content it licenses, and it goes into the console's user exdata
// under that exact name, where rpcs3 looks for it when it decrypts an NPDRM
// executable or an EDAT. A licensed game whose .rap is missing does not boot.
void chimera_rpcs3_add_rap(const char* rap_path);
// The export: a snapshot of every file, names as the zip above has them. The
// pointers are the files themselves and hold until the machine runs again.
int chimera_rpcs3_savedata_count(void);
const char* chimera_rpcs3_savedata_name(int index);
int64_t chimera_rpcs3_savedata_size(int index);
const uint8_t* chimera_rpcs3_savedata_data(int index);
// One vblank period of machine time.
void chimera_rpcs3_frame(void);
void chimera_rpcs3_shutdown(void);

// PS3 main memory as the machine sees it: 256 MiB from 0x00000000, unmapped
// pages read as zero. Copies `size` bytes from `offset` into dst.
void chimera_rpcs3_read_main_memory(uint32_t offset, uint32_t size, uint8_t* dst);
// The main memory block itself, 0x00010000 for 0x0FFF0000 bytes, always mapped.
uint8_t* chimera_rpcs3_main_memory_ptr(void);
// FNV-1a over every mapped page of main memory (cheaper than a copy).
uint64_t chimera_rpcs3_main_memory_digest(void);
// What the machine wrote to its TTY so far (bytes appended since init).
const uint8_t* chimera_rpcs3_tty(int64_t* size);

// Input: 7 ports of DualShock 3, buttons in wire order Up Down Left Right
// Select Start L3 R3 Triangle Circle Cross Square L1 R1 L2 R2 PS (17), axes
// LX LY RX RY (0..255, 128 centre). A port not present is disconnected.
void chimera_rpcs3_set_port(int port, int present);
int chimera_rpcs3_port_present(int port);
void chimera_rpcs3_set_button(int port, int index, int state);
void chimera_rpcs3_set_axis(int port, int index, int value);
// Whether any cellPadGetData happened during the last frame.
int chimera_rpcs3_input_was_read(void);
// The picture at the last flip (BGRA), 0x0 before the first.
const uint32_t* chimera_rpcs3_video(int* w, int* h);
// The last frame's audio: interleaved stereo s16, 800 frames at 48 kHz.
const int16_t* chimera_rpcs3_audio(int* frames);

int chimera_rpcs3_vsync_numerator(void);
int chimera_rpcs3_vsync_denominator(void);
uint64_t chimera_rpcs3_machine_time_ns(void);
int chimera_rpcs3_thread_count(void);
int chimera_rpcs3_is_running(void);
// The installed firmware's version ("4.82"), empty without firmware.
const char* chimera_rpcs3_firmware_version(void);
void chimera_rpcs3_debug_ppu(void);
// Every CPU thread, where it is and what it last called - for a machine that
// has gone quiet. Written to stderr; never machine state.
void chimera_rpcs3_debug_threads(void);
// The guest's own instructions and memory, for a thread that is spinning
// rather than parked in an lv2 wait: the loop says what it polls, and the
// polled word says whether the wait is reasonable. peek reads big-endian
// words; with deref it reads the word at addr first and dumps from there plus
// offset, which is how a field of an object a global points at is reached in
// one go. Both to stderr, both diagnosis, never machine state.
void chimera_rpcs3_disasm(uint32_t addr, int count);
void chimera_rpcs3_peek(uint32_t addr, int32_t offset, int count, int deref);

// The renderer the project asked for ("null" or "opengl-hw"), before init;
// opengl-hw draws only when a GPU bridge was installed (waterbox/gl-shim.cpp)
void chimera_rpcs3_set_renderer(const char* name);
// "interpreter" or "asmjit" (the default), before init: part of the machine
void chimera_rpcs3_set_spu_decoder(const char* name);
// "interpreter" (the default) or "llvm", before init: part of the machine
void chimera_rpcs3_set_ppu_decoder(const char* name);
// 1 (the default) to write finished color buffers back into the console's
// memory, where a game that reads its own picture expects them; before init
void chimera_rpcs3_set_write_color_buffers(int on);
// 1 when the GL renderer is drawing through the bridge
int chimera_rpcs3_gpu_active(void);
// a fault on a guest page (address, write?): 1 when the renderer handled it
int chimera_rpcs3_on_fault(uint64_t addr, int is_write);
// how many such faults the renderer handled (diagnostic, never machine state)
uint64_t chimera_rpcs3_fault_count(void);
// how many of the RSX's own faults, taken inside its cache, were served by
// opening the pages for it instead (diagnostic, never machine state)
uint64_t chimera_rpcs3_window_count(void);
// How a game's own data install fared: what the memory filesystem holds, how
// many of its files are held as a reference to the disc instead of copied, what
// those stand for, what was copied in full, and how much of the disc had to be
// decrypted to tell. Diagnostic, never machine state. `which`: 0 bytes held in
// memory files, 1 mirrors, 2 bytes mirrored, 3 bytes copied, 4 bytes decrypted.
uint64_t chimera_rpcs3_memfs_stat(int which);
// A game's own data install in miniature: copy a file off the mounted disc onto
// the console's hard disk the way an installer does, read it back, and leave
// what it cost in the figures above. `flags`: 1 reads the image as it lies
// rather than as the console reads it (the negative control), 2 skips the copy
// and only reads back what an earlier call wrote, which is how a machine that
// has been through a savestate and another process is asked. Diagnostic;
// returns the bytes copied or a negative number. No normal run calls it.
int64_t chimera_rpcs3_disc_copy_probe(const char* disc_rel, int flags);
// the compile cache bridge (cache-bridge.h): the host's dispatcher, before init
void chimera_rpcs3_install_cache_bridge(uint64_t addr);
uint64_t chimera_rpcs3_cache_fetched(void);
uint64_t chimera_rpcs3_cache_stored(void);
// a precompile session (before init): worker index of count; no run, the
// sweep compiles every Nth module into the cache bridge, then done
void chimera_rpcs3_set_precompile(int index, int count, int firmware_too);
int chimera_rpcs3_precompile_done(void);
void chimera_rpcs3_precompile_progress(uint32_t* done, uint32_t* total);

#ifdef __cplusplus
}
#endif
