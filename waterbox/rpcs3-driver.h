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

#ifdef __cplusplus
}
#endif
