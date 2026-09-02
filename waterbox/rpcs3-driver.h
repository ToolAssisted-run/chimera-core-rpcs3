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
int chimera_rpcs3_init(const char* work_dir, const char* game_path, const char* firmware_path);
// One vblank period of machine time.
void chimera_rpcs3_frame(void);
void chimera_rpcs3_shutdown(void);

// PS3 main memory as the machine sees it: 256 MiB from 0x00000000, unmapped
// pages read as zero. Copies `size` bytes from `offset` into dst.
void chimera_rpcs3_read_main_memory(uint32_t offset, uint32_t size, uint8_t* dst);
// FNV-1a over every mapped page of main memory (cheaper than a copy).
uint64_t chimera_rpcs3_main_memory_digest(void);
// What the machine wrote to its TTY so far (bytes appended since init).
const uint8_t* chimera_rpcs3_tty(int64_t* size);

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
