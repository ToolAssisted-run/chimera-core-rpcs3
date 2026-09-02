# RPCS3 -> Chimera waterbox core: the plan

Written 2026-09-02 at the start of the effort; update as milestones land. The goal is a
working, deterministic, waterboxed RPCS3 core package: the PlayStation 3 as a citable
machine. Interpreters first, the null RSX backend for every equivalence gate, real
pictures through the bridged OpenGL renderer, the LLVM recompilers as a later
optimisation. No user interface, no networking, no real audio or input devices.

## What the survey found (2026-09-02, upstream @ 677e13da4, v0.0.42-234)

- **Scale**: Emu/Cell 79k lines + Modules 133k + lv2 38k + RSX 22k + Io 17k + NP 16k +
  util/Utilities 48k, plus 2.4 GB of LLVM and 27 other vendored externals. Twice the
  size of dolphin's scope, and the externals are linked UNCONDITIONALLY (llvm, asmjit,
  ffmpeg, sdl3, opengl, vulkan, glew, libusb, wolfssl, openal, cubeb, soundtouch,
  miniupnpc, libevdev, protobuf, pugixml, glslang, libpng, rtmidi, yaml-cpp, zlib, zstd,
  curl). Options exist for llvm, vulkan, sdl, faudio, evdev only. The Android port left
  `WITHOUT_OPENGL` / `WITHOUT_OPENAL` and the `if (NOT ANDROID)` seams around Qt, GL,
  GLEW, OpenAL and ffmpeg - the emulator library `rpcs3_emu` is already Qt-free.
- **Language**: C++23, GCC floor 13 (host g++ 13.3 clears it exactly), `-fno-exceptions`,
  baseline `-msse -msse2 -mcx16` with runtime feature dispatch in `utils::has_*`.
- **Firmware is required for anything real**: LLE `liblv2.sprx` (and everything it
  pulls) comes from Sony's `PS3UPDAT.PUP`; the decrypt pipeline (`pup_object`,
  `SCEDecrypter`, `tar_object`, keys compiled in) is pure code behind a 200-line Qt
  wrapper. Plain PPU ELFs boot with no firmware through the HLE lv2 (a stub
  `liblv2.sprx` file on the host path skips the gate). The user supplies the PUP; it
  is never committed or shared.
- **Threads are the machine**: every PPU thread, SPU thread, the RSX thread, its vblank
  child, cellAudio, the timer thread and the pad thread are host threads paced by the
  wall clock (`get_system_time` = CLOCK_MONOTONIC or rdtsc) through ONE wait engine
  (`atomic_wait_engine::wait` = futex on Linux) and ONE sleep primitive
  (`thread_ctrl::wait_for`, timerfd or the engine). Preemption is a TSC heuristic
  that is off by default. 49 `busy_wait` sites spin on rdtsc; 44 `std::this_thread::yield`
  sites spin on the OS. There is no cycle model at all: the PS3 is "as fast as the
  host", and lv2's two-hardware-thread scheduler is emulated on top.
- **Memory**: `g_base_addr` is a flat 4 GiB view (`vm::base(addr) = base + addr`),
  `g_sudo_addr` a 4 GiB RW mirror of the same pages (memfd), `g_exec_addr` an 8 GiB
  table of one function pointer per PPU instruction that the STATIC INTERPRETER reads
  (`cache + cia*2`), plus 36 GiB of dead or debug reservations (`g_hook_addr`,
  `g_stat_addr`). SPU local storage is mapped 5 times in a ring for wrap-around DMA.
  The PS3 map: main 0x00010000+256M, user64k/user1m dynamic in [0x20000000,0xC0000000),
  video 0xC0000000+256M, stack 0xD0000000+256M, SPU 0xE0000000+512M. Only
  `sys_mmapper_map_shared_memory` twice on one object needs true aliasing.
- **Signals**: SIGSEGV serves the RSX texture cache (GL/VK only), RawSPU MMIO, ZCULL
  report write-watch, `sys_mmapper` page-fault notification, and demand-commit of the
  exec table. With interpreters + null RSX and uniform RW pages none of it is needed
  except the ZCULL protect (must be stubbed or it fault-loops).
- **CPU**: PPU decoders are `_static` and `llvm` only; SPU `_static`, `dynamic` (needs
  LLVM), `asmjit`, `llvm`. asmjit is mandatory even without LLVM: `ppu_gateway`, the
  SPU gateway/escape, thread trampolines, 24 vector ops and RSX vertex copies are
  generated at startup into a 16 MiB W+X arena. Floating point: `use_accurate_dfma`
  (default true, `std::fma`) is THE determinism switch; DAZ/FTZ and rounding are set
  per thread (PPU nearest, SPU toward-zero + FTZ) - host FPU state must travel with the
  green thread. Feature-dependent paths are encoding-only except `gv_fmafs` (compile
  time `__FMA__`).
- **RSX**: no software rasteriser exists. `NullGSRender` runs the whole FIFO, flips,
  vblanks and semaphores, and draws nothing. The GL backend wants GL 4.3 core +
  ARB_texture_buffer_object + (ARB|EXT)_direct_state_access, hand-written GLSL 430, no
  glslang; on Windows it resolves entry points through its own `GLProcTable.h`, which
  is the seam for the bridge. `write_color_buffers` is false by default, so GPU
  output never enters guest memory unless a title needs it.
- **Audio**: cellAudio mixes 256-frame blocks at 48 kHz on a 5333 us tick of
  `get_system_time`; `audio_ringbuffer::commit_data` is the post-mix, post-downmix
  drain point; buffering and time stretching are wall-clock feedback (off).
- **Pads**: `pad_thread` ticks every `pad_sleep` us calling each handler's `process()`;
  a `PadHandlerBase` that writes `m_buttons`/`m_sticks` from the frame's input is
  the whole injection; `cellPadGetData` reads it. No movie system exists.
- **API**: `EmuCallbacks` has 47 members and only one null check; fill all of them.
  `fs::device_base` + `fs::set_virtual_device` is an exercised virtual filesystem hook
  (the new ISO loader uses it: Redump ISO + .dkey mount as `/dev_bdvd`). Boot targets:
  ELF/SELF/EBOOT.BIN files, game folders, ISOs. Boot to `ready` with
  `misc.autostart=false`, then `Run()` yourself.

## Architecture decisions

- **Upstream pin**: `extern/rpcs3` = RPCS3/rpcs3 @ `677e13da4`, submodules recursive.
  Unmodified; local changes live in `patches/` (numbered, applied by
  `apply-patches.sh`), each a build option or a hook, never a deletion.
- **Own top-level CMake, rpcs3's CMake underneath**: `waterbox/CMakeLists.txt` sets
  the option set once and adds `extern/rpcs3/3rdparty` + `rpcs3/Emu` (patch 0001 puts
  a `CHIMERA_HEADLESS` seam where upstream has `NOT ANDROID`: no Qt, no rpcs3qt, no GL
  loader, no OpenAL, no prebuilt ffmpeg). Both flavors differ only by the toolchain
  file, the dolphin recipe. Externals we cannot avoid are built from their vendored
  sources for BOTH flavors (wolfssl, curl, miniupnpc, protobuf, libusb, hidapi,
  rtmidi-dummy, cubeb-null, zlib, zstd, libpng, pugixml, yaml-cpp, asmjit, soundtouch,
  7zip, stblib, unordered_dense). ffmpeg comes from source (`extern/ffmpeg`, no asm,
  only the decoders the PS3 modules ask for) for both flavors so decoded media is
  flavor-identical; the native build never uses upstream's prebuilt binaries.
- **The virtual-time scheduler ("vsched") is the core's, not miniBox's.** One thread
  runs at a time; the running thread hands off explicitly (a semaphore per thread,
  plain pthreads in both flavors, so the native build IS a reference and not just a
  faster nondeterministic cousin). Policy: round robin in creation order among
  runnable threads; a thread leaves the run set only through vsched (`wait(addr, old,
  timeout)`, `sleep(until)`, `yield`, exit); virtual time advances by (a) explicit
  charges from the interpreters (a per-instruction cost, charged when a thread's
  instruction budget runs out and it yields), (b) a fixed cost per yield so spinners
  make time pass, (c) a jump to the earliest deadline when nothing is runnable. RPCS3's
  wait engine (3 functions), `thread_ctrl::wait_for/wait_until/wait_for_accurate`,
  `busy_wait`, every `std::this_thread::yield`, `get_system_time`,
  `get_timebased_time`, `utils::get_tsc` (freq pinned to 0 so no path uses rdtsc) are
  routed to it. The thread trampoline and `initialize/finalize` register threads.
  MXCSR + fenv are saved and restored across handoff. The driver's main thread is a
  vsched participant that sleeps until the frame's end and runs the "call from main
  thread" queue when woken.
- **The frame**: one vblank period of virtual time. `vblank_rate=60`, the vblank
  thread already computes `start + n * 1000000 / 60` in integer microseconds, so the
  declared rate is 60 (the machine times itself by whole microseconds; revisit if a
  title's timing wants NTSC).
- **Memory in the box**: patch the layout, not the translation. `g_sudo_addr` becomes
  `g_base_addr` (uniform RW pages, `get_super_ptr` = `_ptr`), `map_self`/`g_shmem`
  aliasing skipped, `g_hook_addr`/`g_stat_addr` gone, the SPU LS ring replaced by
  wrap-aware copies, ZCULL write-watch stubbed, exec table demand-commit replaced by
  commit-on-register. The flat base (4 GiB) and exec table (8 GiB) stay flat: miniBox
  lifts its single-4 GiB-region rule (spec v2; nothing in the page bookkeeping is
  32-bit) and the guest layout carries a dedicated `vm` arena. True shm aliasing
  (`sys_mmapper` double maps) is deferred until a title needs it.
- **No signals, no TLS**: `s_exception_handler_set` becomes a build option; the ~93
  `thread_local` sites move to the SandboxTls pattern from dolphin (pthread keys, or
  vsched-indexed slots for the hot ones: `g_tls_this_thread`, `cpu_thread::g_tls_this_thread`,
  `vm::g_tls_locked`, `idm::g_id`, `g_tls_log_prefix`).
- **CPU**: `ppu_decoder=_static`, `spu_decoder=_static`, `use_accurate_dfma=true`,
  `spu_loop_detection=false`, `spu_cache=false`, `llvm_precompilation=false`,
  `ppu_threads=2`, `clocks_scale=100`, `sleep_timers_accuracy=_as_host` (all waits
  are ours anyway). `utils::has_*` pinned to a fixed baseline (SSE4.1 yes, AVX no,
  AVX-512 no) so the same movie syncs on any host. Instruction budgets: the PPU
  interpreter loop and `old_interpreter` charge vsched per instruction batch.
- **RSX**: `renderer=null` for every equivalence gate. Pictures via the GL backend
  through the glad bridge (`GLProcTable.h` filled by `chimera_gl_lookup`; the
  `-hw` renderer convention, hardware or llvmpipe on the host). The picture is
  taken at `rsx::thread::flip` from the display buffer in guest VRAM when the
  renderer is null (whatever the machine holds) and from the bridged framebuffer
  otherwise; `write_color_buffers` stays off so the machine never reads the GPU.
- **Audio**: `renderer=null`, `enable_buffering=false`, `enable_time_stretching=false`;
  `commit_data` pushes 256-frame blocks into our ring; the driver drains
  48000/60 = 800 frames per vblank (remainder accumulated), silence when short.
- **Input**: one `chimera_pad_handler` bound to 7 ports, `pad_mode=single_threaded`;
  `process()` copies the frame's buttons/sticks; a lag frame is a frame nobody
  polled (`cellPadGetData` hook). Wire order: DualShock 3 digital in `CELL_PAD_CTRL`
  bit order, then 4 axes, 7 ports.
- **Filesystem**: three `fs::device_base` devices - a read-only device over files
  the frontend mounts (`/dev_bdvd` from the ISO through rpcs3's own `iso_device`, the
  boot ELF, the PUP), a writable in-memory `/dev_hdd0` (savedata, trophies, caches)
  that is the save-data channel's payload, and `/dev_flash` populated in-memory from
  the PUP at init BEFORE the seal (baseline, not in savestates; the user points the
  firmware channel at Sony's own `PS3UPDAT.PUP`, hashed per version). `init_dirs=false`,
  config/cache/log roots redirected (the ANDROID globals in File.cpp are the seam),
  `games_config` never saves, no lock file, no log listener.
- **Sandbox time**: `console_time_offset` fixed, `system_name`/`console_psid` pinned,
  RTC = a constant epoch + virtual time.
- **Deferred by design**: LLVM in the box (the recompilers are the difference between
  1 fps and playable; LLVM builds for musl but is its own milestone), PKG install,
  RawSPU, `sys_mmapper` aliasing, network anything.

## Milestones

- **M0 native reference**: both CMake flavors configure; `run-native` boots a
  hand-assembled PPU ELF (`tests/roms/lv2test.elf`, built by `tests/asm/ppc.py`: lv2
  syscalls only - tty, timers, threads, memory) under vsched with interpreters and
  the null RSX; deterministic main-memory + TTY hashes over 600 frames across two
  runs. Decides the ffmpeg question and the CallFromMainThread cadence.
- **M1 guest**: `core.wbx` under the musl toolchain; miniBox spec v2 (region > 4 GiB,
  `vm` arena); native == sandbox byte-for-byte. The syscall stub inventory and the
  TLS conversion happen here.
- **M2 savestates**: save+load around every frame changes nothing (arena snapshot;
  vsched state, thread stacks, semaphores all in guest memory).
- **M3 firmware + a real homebrew**: PUP decrypt in the box, LLE liblv2, a homebrew
  that uses cellGcm/cellPad/cellAudio; input leg (scripted `--press` reaches
  `cellPadGetData`), audio leg, lag counting.
- **M4 a real disc**: Redump ISO + .dkey through `iso_device`, a commercial title
  (user-supplied, local only), `/dev_hdd0` through the save-data channel, gate legs
  SKIP-with-reason when content is absent.
- **M5 GPU bridge**: the GL backend through `GLProcTable.h` + `chimera_gl_lookup`,
  gl-host on the frontend side, machine RAM identical with and without the GPU.
- **M6 package + frontend**: `rpcs3.chimeraCore`, waterbox.config (buttons, axes,
  7 ports, machine settings), default_keybinds.json, file_slots (iso, dkey, elf),
  firmware channel (PUP versions), licences manifest, frontend gate 3/3, Windows.
- **M7+**: LLVM recompilers in the box, ffmpeg codecs if M0 stubbed them, PKG
  install, trophies UI-less, RawSPU.

## Status

- **M0 DONE (2026-09-02, same day as the survey)**: `build-native.sh` (rpcs3's
  CMake, patch 0001 seam) + `native.mk` (driver, vsched, host stubs, the pad
  thread and PS Move sources upstream keeps in its Qt library) -> `run-native`
  boots `tests/roms/lv2test.elf` (hand-assembled by `tests/asm/ppc.py`: two PPU
  threads, lv2 timers, memory, TTY) and `run-gate.sh` passes
  `native:deterministic` at 600 frames (RAM + TTY digests, both runs). Six
  patches, ~1300 lines, none a deletion. Findings that shaped it: the PS3
  function descriptor is two u32 (address, TOC), not two u64; syscall 53 is
  `sys_ppu_thread_start` (49 is get_stack_information); 64 KiB pages are
  `SYS_MEMORY_PAGE_SIZE_64K = 0x200`; a bare `std::thread` (the log writer,
  the RSX audio timer's epoll) calling into vsched corrupts the baton, so every
  such site became synchronous or virtual (patch 0006) and vsched now asserts
  baton integrity; the exception-handler installer exists twice (Windows and
  POSIX lambdas) and both need the seam; a link rule that does not depend on
  the library ships stale objects. ffmpeg is still upstream's prebuilt for
  native (fine until the guest build needs source).

- **M1 + M2 DONE (2026-09-02, same day)**: the guest library and every external
  compile under musl (kernel/glibc-private header shims in `waterbox/guest-include`,
  no pkg-config, curl without system libraries, hidapi excluded, ffmpeg n8.1.2 from
  source for BOTH flavors with no assembly, native with `--enable-pic`), `core.wbx`
  links with zero undefined symbols and passes check-wbx (no TLS symbols, no `%fs`),
  and `run-gate.sh` is 4/4: native deterministic, native == sandbox at 600 frames
  (RAM + TTY digests), rewind equal, rerecord lossless (21.7 MB arena states).
  What it took: miniBox spec v2 (the block may span 4 GiB regions); patch 0007
  keeps the flat guest view 4 GiB ALIGNED (`vm::get_addr` truncates host
  pointers) with no mirror and uniform RW pages, commits execution-table pages per
  mapping (`u64{addr} * 2`: the 32-bit expression overflowed above 2 GiB) filled
  with a decode-on-first-run stub (the interpreter tail-chains through the table
  without the loop's checks, so entries must never be empty), puts SPU local
  storage in the guest view, shrinks the dead hook/stat areas; patch 0008 pins
  every entropy source; patch 0009 turns 81 `thread_local` lines (67 variables)
  into slot arrays over vsched thread slots (`chimera_tls.h`; a delegated patch),
  and the third-party thread-locals (absl, glslang, wolfssl, libusb) vanish for
  the guest via `-Dthread_local= -D_Thread_local= -D__thread= -U__cpp_constinit`
  (one runner at a time makes a static equivalent; absl's constinit marker is
  what function-local thread-locals trip over); the emulator's own files
  (config, cache, logs, dev_flash, the grafted game) live in `waterbox/memfs.cpp`
  behind rpcs3's virtual-device layer in both flavors; host plumbing the machine
  has none of (pipe, sockets, eventfd, epoll, timerfd, times, getrusage) fails
  identically in both flavors (`host-plumbing.cpp`) so both start the same 29
  threads. The nine patches are a consecutive series (regenerated from commits
  replayed on a temp branch; per-file diffs had overlapped).

- **M3 DONE (2026-09-02)**: Sony's PUP decrypts into /dev_flash of the memory
  filesystem before the seal (1.3 s, 194 MB; rpcs3's own pipeline freed from
  its Qt dialog), liblv2 LLE; the core's pad handler (patch 0010: the pad
  thread's "null" handler is ours; fill `m_buttons`/`m_sticks`, the thread
  mirrors them into the external lists cellPad reads), `cellPadGetData` marks
  the frame polled, `commit_data` hands every mixed block to the driver (800
  frames per vblank), the RSX flip copies the display buffer from VRAM.
  `padtest.elf` imports cellPad through the real stub-table shape (PT_LOOS+2
  PRX param, ppu_prx_module_info, SHA-1 NIDs; the assembler grew `.nid` and
  `.prxparam`). Gate 7/7 (firmware:lle, input:press, input:lag). Not yet: an
  audio tone leg and a flip leg need a program that uses cellAudio/cellGcm
  (the homebrew toolchain waits on host packages; hand-assembly possible).
  Patch-series rule learned: a later patch may not rewrite an earlier patch's
  hunk (apply-patches.sh checks each patch alone); fold it by replaying the
  series in a worktree.

- **M6 DONE on Linux (2026-09-02)**: `rpcs3.chimeraCore` (13 MB, deterministic
  SHA-1, 20 licences under GPL-2.0-only) loads in Chimera: `tests/run-frontend.sh`
  runs lv2test 200 frames with main memory identical to the sandbox reference
  and the package's DualShock 3 bindings adopted. The frontend learned the PS3
  (VSystemID, SystemNames, mnemonics: L2 '[', R2 ']', PS 'H'); chimera's miniBox
  submodule moved to spec v2. The memory domain is the main block itself
  (0x00010000 for 0x0FFF0000, mapped whole at init, never unmapped); axes in
  waterbox.config are objects (name/min/max/neutral). Windows untested: the
  Windows host backs the block with a pagefile section of the FULL size, and a
  20 GiB commit will not fit most machines - SEC_RESERVE with commit-on-allocate
  is the fix (miniBox pal_win.c). Also open: the disc-key slot (M4).

## Risks, ranked

1. **vsched completeness**: any wait outside the engine (a `std::mutex` contended
   across a handoff, a `timerfd` read, a `sleep_for`) deadlocks the single runner.
   Symptom is a hang, diagnosis is "who holds the baton". Every such site found is
   one more route into vsched.
2. **Address space**: 4 + 8 GiB flat tables plus a 2 GiB asmjit arena plus the game
   in a single miniBox block. Page bookkeeping is 1 byte per page (24 GiB = 6M pages);
   savestate status arrays grow to match. Dirty tracking must stay sparse.
3. **Hidden host state**: asmjit runtime arena contents (generated at init, before
   seal: fine), `spu_cache`, the exec table (guest memory: fine), the lv2 scheduler's
   `g_waiting`/`g_ppu` (guest memory: fine), anything `thread_local` we miss (a
   per-thread cache that survives a savestate load wrongly).
4. **Externals for musl**: wolfssl/curl/protobuf/libusb compile but may drag in
   `getrandom`, sockets, `dlopen` at init. Stubs answered in `guest-syscalls.cpp` as
   found, or the module stubbed.
5. **Speed**: interpreters in a box at 1 fps on a real game would be a proof, not a
   product. The recompiler milestone is the product.
6. **Firmware in the box**: 200 MB of PUP decrypt per init; if it costs a minute,
   cache the decrypted tree as a frontend-side derived file.
