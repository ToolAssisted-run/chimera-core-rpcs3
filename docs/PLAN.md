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

- **M4 DONE (2026-09-02): a real disc boots.** GTA San Andreas (decrypted
  Redump ISO, the user's) mounts, links its modules, runs 300 frames at 33 fps
  native, memory different every frame, native == sandbox (gate leg
  `disc:boot`). Three patches the game found: 0011 cross-thread spins yield
  (rsx pause handshake, timestamp wait, SPU exit wait, vm cpu-flag wait,
  overlay join: a host spin never lets the other thread run under vsched),
  0012 an unmapped page is cleared and stays readable (the frontend reads main
  memory as one plain buffer; a no-access page inside it faulted the frontend), 0013 copy_file within the memory filesystem (the trophy installer).
  The dkey slot rides as `game/<stem>.dkey` for encrypted discs. Encrypted
  discs need their Redump key; nothing else is missing.
- **Audio and video proven (2026-09-02)**: PSL1GHT toolchain built in
  `~/ps3dev` (ps3toolchain, GCC 7.2; `make_self` needs OpenSSL and is left
  out, ELFs are what rpcs3 loads; an UNSTRIPPED PSL1GHT ELF crashes at a
  descriptor read, so `tests/ps3/build.sh` strips and sprxlinks). Two
  programs of our own in `tests/ps3`: `flip` (CPU-drawn pattern, RSX flip,
  TTY per frame) and `tone` (cellAudio square wave). Gate legs `video:flip`
  (12 different images in 12 reports, native == sandbox) and `audio:tone`
  (the audio cycles with the wave's period, native == sandbox). Patch 0014:
  the audio ring marks its backend active (only a backend callback ever did,
  and the null backend has none) and the null backend resolves "automatic" to
  stereo (the downmixer throws on automatic; the cellAudio thread died
  silently on the first mixed block). The driver pins the stereo layout and
  takes CHIMERA_LOG_TRACE=chan,chan for trace-level logs.

- **M5 DONE on Linux (2026-09-02): the GL renderer in the box, through the GPU
  bridge.** RPCS3's own OpenGL renderer runs inside the guest; every GL entry
  point is a glad pointer filled from the bridge lookup (patch 0015: the GL
  sources under CHIMERA_HEADLESS when CHIMERA_GL, `OpenGL.h` includes glad
  instead of GLEW, `gl::init()` is `gladLoadGLUserPtr(bridge lookup)`, no swap
  interval, no glDrawPixels, headless may pick opengl, and `rsx::mm_protect` /
  the ZCULL protects are no-ops: the renderer's caches watch guest pages by
  protecting them and catching the fault, and this build has no fault handler -
  a texture the CPU rewrites in place is served stale until the cache drops
  it). The picture leaves through RPCS3's own frame consumer: `g_recording_mode
  = rpcs3` makes `GLPresent` read the flipped image back and hand it to
  `present_frame`, so no present patch. The overlay icons (23 PNGs from
  extern/rpcs3/bin/Icons/ui, `gen-assets.py`) travel inside the core. Host
  half: Dolphin's `gl-host.c` (EGL surfaceless + pbuffer) in run-wbx and
  run-native; libchimera's for the frontend. The master list grew by 40 names
  (glProgramUniform*, glGetIntegeri_v and siblings, EXT DSA, bindless).
  **The native reference is one binary with one glad table**: natively the
  renderer resolves the real driver (eglGetProcAddress) and the wrappers stay
  unused, or the dispatcher recurses into them; the host context is bound on
  the RSX thread (`chimera_gl_host_bind_current` from `set_current`). Any
  unbridged name a renderer reaches lands in a generated trap that says which
  (`gen-traps.py`). Results on llvmpipe: the flip program's GL pictures are
  pixel-identical to the VRAM copies; GTA San Andreas draws its legal screen
  at 47 fps native (1800 frames in 38 s), main memory identical to the null
  renderer's run at every report, native == sandbox. Gate legs `gpu:flip`,
  `gpu:disc`; frontend leg `gpu:frontend` keeps a screenshot. Setting
  `renderer` = null | opengl-hw (default opengl-hw; without a bridge the core
  falls back to null).
- **CPU-write detection for the GL caches (2026-09-02)**: miniBox spec v2.1
  (commit 4a4bb05) forwards faults on pages the guest protected itself to a
  guest export `GuestFaultHandler(addr, is_write)` on the faulting thread and
  retries when it returns nonzero; wbx-entry exports it and the driver's
  `chimera_rpcs3_on_fault` mirrors the renderer part of RPCS3's own
  `handle_access_violation` (temporary_unlock, `rsx::g_access_violation_handler`).
  Natively run-native's SIGSEGV handler does the same. So `rsx::mm_protect` and
  the ZCULL protects are real again (patch 0015 no longer touches them). GTA:
  1380 faults served in 1800 frames, memory unchanged, the pictures now
  refresh where the CPU rewrote textures. The gate's gpu:disc leg requires
  both flavors to serve the same number of faults.
- **Savestates under the GL renderer (2026-09-02)**: a state taken and loaded
  while GTA draws through the bridge replays to the same memory (run-wbx
  --rewind, 240 frames, EQUAL; gate leg `gpu:rewind`). The renderer's caches
  live in guest memory and the pages they protected are part of the state, so
  a load restores both together; the GL objects themselves stay in the host
  context and are only as current as the last draw, which is the accepted
  cost of a GPU outside the savestate.
- **M7a: the ASMJIT SPU recompiler in the box (2026-09-02)**. Patch 0016: a
  per-thread budget field in spu_thread; the emitter charges it at function
  entry (the function's length) and at every local backward branch (the loop
  body), calling the scheduler when it runs dry, so recompiled code keeps the
  machine's clock the way the interpreter's per-instruction charge does (a
  different but equally deterministic timing: the SPU decoder is a machine
  setting, `spu_decoder` = asmjit | interpreter, default asmjit). TRAP: the
  emitter's position is "none" (all ones) on fallthrough paths; charging
  (none - target) cost a billion instructions per event and made each SPU
  answer take 24 frames. Our tests/ps3/sputest (a PPU signalling one SPU
  thread every frame, the SPU looping an LCG over local store and answering
  with a checksum event) gives identical answers under both decoders, one per
  frame, and runs in two thirds of the interpreter's host time. Gate legs
  spu:interpreter, spu:asmjit (native == sandbox each), spu:agree. GTA's
  first minute runs no SPU code at all, so it proved nothing here.
  The PPU has only the interpreter and LLVM: LLVM in the box remains the big
  one (M7b).
- **M7b: the LLVM PPU recompiler in the box (2026-09-02, both flavors, gate leg
  ppu:llvm native == sandbox)**. In the guest three more things: musl's static
  `dlopen` answers "Dynamic loading not supported" and LLVM's JIT takes that
  as fatal when it loads the process's own symbols, so the guest defines
  dlopen/dlsym/dlclose/dlerror itself (guest-syscalls.cpp: the process is the
  library, and dlsym resolves memcpy and the libm names generated code may
  call); a `[[gnu::target]]` attribute resets the stack-protector guard to
  %fs:0x28, so GNUC_X64_TARGET adds no_stack_protector; and the memory layout
  grew to 1 GiB of heap and a 26 GiB arena (LLVM's 768 MiB code space did not
  fit beside the view, the execution table and the JIT arena; the failed
  reservation's null became an EINVAL commit at 0x10000000). The core is 129
  MB with LLVM inside. Fatal log lines now also reach the host's stderr,
  the only way to read a sandboxed death. LLVM 22 from RPCS3's submodule (`waterbox/build-llvm.sh`:
  X86 only, no threads, static, no tools; the guest flavor needs the native
  flavor's llvm-tblgen and links its own executables against the guest C++
  runtime through CMAKE_CXX_STANDARD_LIBRARIES). Patch 0017: a per-thread
  budget in ppu_thread, the translator counts every instruction it emits and
  charges the budget at every point control may leave the straight line
  (CallFunction, UseCondition, TestAborted), calling `__chimera_budget` when it
  runs dry; and no boot-time sweep of the firmware's libraries (that alone was
  three minutes of host time per session). Pins: one compile thread, target
  CPU x86-64-v3, no precompilation. lv2test on LLVM: deterministic natively,
  liblv2 + the program compile in about ten seconds. GTA: 80 modules compile
  in 464 s, then 94 fps against the interpreter's 47 in the same phase.
  **The cache is the problem**: compiled objects live in memfs and die with
  the session, so every boot pays the compile. The default stays the
  interpreter (`ppu_decoder` = interpreter | llvm) until compiled modules can
  persist between sessions: the persistent-data ABI is explicit and
  bundle-driven by design, so a compile cache wants its own host-managed
  channel (never machine state: the same source compiles to the same code).
- **The compile cache (2026-09-02, phase 1 of three)**: miniBox
  `source/cache/cache-bridge.h` is the contract (SetCacheBridge before Init;
  FETCH and STORE over the sandbox's callback; relative names only). Patch
  0018 hooks RPCS3's ObjectCache: `load()` asks the host before opening,
  `notifyObjectCompiled` hands the committed .obj.gz over. The driver keys
  by the path under the emulator's cache directory (RPCS3's own naming:
  module hash, version, CPU); the host owns the directory, keyed by package
  identity. run-wbx and run-native take `--cache DIR` (waterbox/cache-host.c).
  lv2test: a cold native run stores 5 objects in 16 s, a warm one fetches
  them and boots in 1.5 s, the same machine. Gate legs cache:objects (the
  flavors' objects byte-identical) and cache:warm. TRAP: LLVM's X86 backend
  emits endbr64 into JIT code whenever the compiler binary itself was built
  with CET (`#ifdef __CET__` in X86IndirectBranchTracking.cpp), which Ubuntu's
  GCC does by default: the native LLVM is built with -fcf-protection=none so
  both flavors emit the same bytes. Phase 2 DONE (2026-09-02): NOT threads in one
  sandbox (guest threads are green threads on one host thread; concurrency
  would need a miniBox redesign) but N sandboxes side by side, each one
  compiling its share. Patch 0019: a precompile session (SetPrecompile(index,
  count, firmware_too) before Init) boots and never runs; every module's parts
  go to the worker whose index the part's name hashes to (stable whatever the
  others already stored), the sweep's files to every Nth; a session returns
  after compiling, before loading; parts done/total drive the progress
  (GetPrecompileDone/Total, IsPrecompileDone; the runners print "Precompiled
  D/T modules"). The precompile thread must be one of RPCS3's named threads
  (a bare std::thread runs beside the scheduler's baton and trips its guard).
  GTA game scope: 4 workers 130 s wall against 434 s for one, 72 objects
  split 13/20/21/18, a warm boot fetches all 72; the firmware scope
  (default) also covers libraries a game loads later. Gate leg
  cache:precompile. Phase 3 DONE (2026-09-02): the engine takes a cache directory
  (`ce_cache_dir`, named `<Compile Cache path>/<core>/<package version>/` by
  the frontend) and a precompile request (`ce_precompile_request`) before a
  session opens, hands SetCacheBridge and SetPrecompile to the core before
  Init, and answers `ce_session_precompile_done/progress`; the frontend runs
  itself as `--headless --precompile=INDEX/COUNT[/game]` child processes (a
  package with `"precompile": true`, before a rom's first boot, up to eight,
  a dialog reading "Precompiled D/T modules", a marker per rom SHA1), and the
  core's menu offers Precompile This Game / Clear Compile Cache. Frontend gate
  leg precompile:frontend. TRAPS: a mingw sysv-ABI function may carry no C++
  unwind data (forward to a noinline worker); the cache directory chain must
  be created whole; the frontend needs an X display and ALSOFT_DRIVERS=null
  even headless; a worker leaves through the run loop (_exitRequestPending),
  not Environment.Exit, or a control's finaliser turns the exit code to 255.
  The design is docs/compile-cache.md in the chimera repository.
- **Windows end to end, on real hardware (2026-09-11)**: GTA San Andreas boots
  and draws through the GPU bridge on a GTX 1060 - 1280x720, the Rockstar North
  logo by frame 900. Determinism and rewind were measured with the rest of the
  bridged cores (chimera docs/gpu-bridge.md): a straight run twice is identical
  in MainRAM and in the picture, and three passes of seek-back-and-replay are
  identical too. This core needs no `video.drawEveryFrame`: it exports no
  `SetRenderingEnabled`, so it was never told to stop drawing in the first
  place.

  Getting there took one fix. **A disc with a boot jingle on it killed the core
  before its first frame.** `g_cfg.misc.play_music_during_boot` is on by
  default; RSXThread hands the path to `display_manager::start_audio`, which
  builds an `audio_player`, which `ensure()`s `Emu.GetCallbacks()
  .make_video_source()` - null in a build with no Qt. The abort then leaves
  through `tkill`, which miniBox does not implement, so the visible failure was
  an unimplemented syscall two layers from the cause. Overlay audio is not the
  machine's audio and Chimera has no overlays, so the setting is now off in
  rpcs3-driver.cpp beside `autostart` and `enable_gamemode`.

- **RawSPU: the window cannot be a fault (2026-09-11).** Bejeweled 3
  [BLUS30865] died during boot on both platforms and with both renderers, and
  the reason turned out to be the RawSPU milestone arriving as a crash.

  A Raw SPU shows its problem-state registers to the PPU as a window of memory
  at 0xE0000000 + index * 0x100000 + 0x40000. Upstream leaves that window
  UNMAPPED on purpose: every access faults, and `handle_access_violation`
  decodes the x64 instruction that faulted, performs the register access and
  resumes past it. A sandboxed core cannot do that - the fault leaves the
  sandbox and what comes back is an address and a direction, with no way to
  rewrite the interrupted context. So the access is caught one level up
  instead, in `vm::write` and `ppu_feed_data` (patch 0022), and never becomes a
  fault at all. `chimera_rpcs3_raw_spu_read/write` do the register access; the
  test in front of them is two instructions on the interpreter's hot path and
  the call only happens for the window itself. The write the game died on was
  0xE0043004 - MFC_LSA of raw SPU 0 - followed by EAH, EAL, Size/Tag and Class
  CMD: an ordinary proxy DMA.

  Two things had to be found first. Linux was dying in SILENCE, with no
  diagnosis from anyone, because the core's own fault callback explained its
  decline with `fprintf` - stdio, inside the host's signal handler, where
  musl's file lock reads a thread pointer that is not the guest's. That
  second fault arrives with SIGSEGV already blocked and the process is gone
  before a word reaches anyone. It says the same thing through `write(2)` now,
  and miniBox grew `MB_FAULT_TRAIL` and a nested-fault report so the next one
  is not invisible.

  Verified: GTA San Andreas is byte-identical in MainRAM to the run before the
  change, on Windows through the GPU bridge, and two runs of it agree.

- **A conditional branch charges the clock too (2026-09-11).** With the Raw SPU
  window served, Bejeweled 3 stopped crashing and started HANGING: one SPU
  spinning forever while twenty-eight threads waited on it, no syscall, no
  further MMIO. Risk #1 in this document, and the cause was in patch 0016.

  Recompiled SPU code charges `chimera_budget` on function entry and on a
  backward branch, and `branch_fixed` decided "backward" from `m_pos`, the
  address of the instruction the main loop is emitting. But a CONDITIONAL
  branch does not emit its taken path there: BRZ, BRNZ, BRHZ and BRHNZ defer it
  into `after`, which runs after the loop, when `m_pos` has moved on or is
  "none". A conditional backward branch is how an SPU loop is normally
  written - so the commonest loop of all charged nothing, and a thread that
  charges nothing never gives way. `m_chimera_from` carries the branch's own
  address across that deferral, and a branch whose origin is genuinely unknown
  now charges one, so no loop can run for free however it is closed.

  How it was found, for the next time a machine goes quiet: vsched's statics
  are in the symbol table, so `g_switches`, `g_cur` and the thread ring read
  straight out of a live process (`nm --defined-only bin/core.wbx | grep g_cur`
  gives the address; core.wbx is EXEC at a fixed base). 47 switches and not
  moving, one RUNNABLE thread, and a `%r13`-relative register file with
  `and $0x3fff0` local-store masks said "SPU recompiler" before anything else
  was known. `--settings '{"spu_decoder":"interpreter"}'` then confirmed it in
  one run.

  Verified: Bejeweled 3 runs, and the recompiler's digests are the
  interpreter's, frame for frame. GTA San Andreas is byte-identical to the run
  before this change and deterministic across two runs, natively and on Windows
  through the GPU bridge.

- **A disc dumped as a folder arrives as one .zip (2026-09-12).** A chimera
  project carries FILES, never directory trees, so an 18 GB dump of 1002 files
  had no way in and the core told people to build an .iso first. An archive is
  a file, so it fits - as long as it is read the way a disc image is read.

  `waterbox/archive.{h,cpp}` is a zip reader that never unpacks: the central
  directory is read once, and each open file pulls its own bytes where they
  lie. memfs gained a third kind of node beside "bytes in memory" and "grafted
  host file" - an archive entry - so nothing in rpcs3 can tell the difference.
  Every open file holds its own handle and its own inflate cursor, so no lock
  is needed when a green thread yields mid-read. ZIP64 is mandatory at this
  size and is read. The disc root is wherever PS3_DISC.SFB sits, so an archive
  of the folder and one of its contents both work, and the boot path is the
  EBOOT.BIN beside it - which is what Emulator::GetBdvdDir walks up to find,
  so /dev_bdvd mounts by itself.

  STORE rather than compress for a disc. A deflated member has no index, so
  reaching an offset means decompressing from that member's start: reads that
  walk forward carry on from the cursor, a read that goes backwards restarts
  the member. On a 2.6 GB file that is the difference between instant and
  unusable.

  Verified: Ultra Street Fighter IV, the 18 GB folder packed as a 17.87 GB
  stored zip, boots and plays - 600 frames at 1280x720, memory changing every
  report, the pad read from frame 187, which is frame for frame what the same
  game does from an .iso. A synthetic archive whose EBOOT is garbage gets all
  the way into rpcs3's loader ("Failed to decrypt content"), which is the proof
  the tree and the boot path are built correctly. The reader itself was smoke
  tested natively against stored, deflated and forced-ZIP64 members, including
  forward seeks, backward seeks and reads past the end. GTA San Andreas is
  byte-identical to the baseline and deterministic, so the shared memfs change
  costs the other paths nothing.

  Refused, each with a message that says what to do: **.rar**, because the only
  decoder is unRAR, whose licence does not sit with this core's GPL terms, so
  no build of this core can carry one; **.7z**, and the reason is measured, not
  assumed: the LZMA C sources in 3rdparty do compile, but SzArEx_Extract has no
  partial read - it decompresses a whole solid block into one buffer. Reading a
  64-byte file out of a 4 MB solid test archive allocated 4,194,368 bytes, the
  whole archive; with `-ms=off` the buffer was exactly the file being read
  (1 MB, 64 bytes, 3 MB for the three members). Scaled to a disc that is either
  the whole 18 GB or the whole of USF4's 2.6 GB largest member, per read, and
  the sandbox has nowhere to put either. .zip is the format that works here
  because a stored member is simply bytes at an offset; and an archive with no PS3_DISC.SFB in it.

- **A Raw SPU's local storage is its guest window (2026-09-11).** The
  Bejeweled 3 launcher stall, traced to the end. In the sandbox a
  `utils::shm` object does not alias - patch 0007 says so ("each map commits
  its own pages ... true guest aliasing waits for a title that needs it"),
  and miniBox refuses any mmap that is not MAP_ANONYMOUS. RPCS3 gives every
  SPU a separately reserved `ls` and maps its storage object there AND into
  the vm::spu block, so the two became two copies. For a threaded SPU that is
  harmless: its window is allocated hidden, and the PPU reaches the store only
  through syscalls that go through `ls`. A Raw SPU is the one case where the
  PPU writes into the window directly. This is that title.

  Proof, at the stall: the SPU's `ls` and the PPU's window sat at different
  offsets of the MiniBoxBlock memfd (0x909990000 and 0x8e0000000). The
  launcher had written a parameter block through its window
  (`3002de80 00000000 c0005480 00001000` at +0xa04, a table at +0x10c04); the
  SPU's copy held zeros at both offsets. The SPU polled its copy, the PPU
  polled SPU_Out_Mbox for 0xDEADBEEF, and neither could ever move.

  Fix, patch 0023: for Raw and isolated SPUs `ls` is the guest window itself -
  the constructor that loads a savestate already did this, the one that
  creates an SPU did not - and `map_ls` returns early for such a pointer
  instead of building a mirror ring of copies. The guard is deliberately
  narrow, inside the 4 GiB base view at or above RAW_SPU_BASE_ADDR. The first
  version asked vm::try_get_addr, which answers yes for 8 GiB past the base -
  exactly where a threaded SPU's private storage is reserved - so it left the
  SPURS kernel thread's storage uncommitted and the image copy into it
  faulted "outside the guest's 4 GiB view".

  Verified: the launcher clears the handshake and SPURS init, opens audio
  (the TTY grows from "PopCap Launcher App" to 178 bytes: channel count,
  Dolby/DTS, cellAudioPortOpen), starts SpursHdlr0/1, spu_printf_handler and
  FMOD's MultiStream thread, and runs 1500 frames without a fault on Linux and
  on Windows through the GPU bridge. GTA San Andreas is byte-identical to the
  baseline and deterministic on Linux, and its MainRAM after 900 frames on
  Windows through the GPU is still 6d3c69a242d216dd2c5b98d6d923650b.

- **Open: Bejeweled 3 then runs without ever drawing.** Measured to 3000
  frames: no fault, main memory changes every 250 frames, the pad is read on
  all but 6 of the 3000 frames (all 6 before frame 500), and the SPURS kernel
  SPU is executing (pc 0xbc30 at frame 1500, 0x7564 at 3000). But the TTY stays
  at the 178 bytes printed by frame 500, the thread list does not change after
  frame 500, and the picture never changes: the video digest is
  00a287b3051f8383 from frame 250 to 3000, the digest GTA San Andreas reports
  while its screen is still black at boot, and through the GPU bridge frames
  600, 1000 and 1499 are solid black. Every PPU is parked in an lv2 wait the
  whole time - main_thread in sys_semaphore_wait, SpursHdlr0 in
  sys_spu_thread_group_join. So it is a new wait, not loading; the next pass
  starts from what main_thread's semaphore is waiting for.

- **Diagnosis that a sandboxed machine can answer (2026-09-11).** Three tools,
  added while chasing the above:

  `run-wbx --debug-at N` calls the new `DebugThreads` export after frame N: the
  machine's status and vsched's switch count, then every PPU (id, cia, state
  flags, priority, the HLE function it is in and the last one it called, its
  name, and a call stack) and every SPU (pc, state, status, mailbox counts, tag
  mask and status, pending tag update, MFC queue depth, barrier and fence,
  events, stall mask, and the instructions around its pc).

  `run-wbx --log-trace <chans>` raises those RPCS3 log channels to trace and
  mirrors every message to stderr; `all` means every registered channel. It
  arrives as a MOUNTED FILE, because **a sandboxed guest is handed no
  environment at all** - `getenv` answers null in the box, which is why
  `CHIMERA_LOG_TRACE` and `CHIMERA_SPU_TRACE` had never once worked there. The
  levels are applied again after the boot, because loading a game re-applies
  the configured ones over the top.

  `MB_ALLOW_PTRACE=1` in run-wbx's own environment calls `prctl(PR_SET_PTRACER)`
  so a gdb that is not an ancestor can attach (yama scope 1). The guest is
  green threads on ONE host thread and core.wbx is linked EXEC at a fixed base,
  so `nm --defined-only -n bin/core.wbx` symbolises any guest address and an
  attached gdb can read guest statics by name - which is how vsched's own ring
  was read while it was stuck.

  Still missing: a way to pull /cache/RPCS3.log out of the memory filesystem
  (`--log-out`, beside `--tty-out`) for the messages that are written before a
  listener could be added.

- **Ultra Street Fighter IV [BLUS31218]: a disc executable without its disc
  (2026-09-11).** The title exists here only as a dumped FOLDER - PS3_DISC.SFB
  beside PS3_GAME/, 18 GB over 1002 files - and there is no image of it.
  Pointed at `PS3_GAME/USRDIR/EBOOT.BIN`, the core boots, reaches 28 threads
  and then does nothing whatever: main memory's digest is 325942431de025e1 at
  frame 30 and still 325942431de025e1 at frame 60, the TTY is empty, every
  frame is a lag frame, and `--debug-at 40` finds the whole machine parked -
  main_thread in `sys_timer_sleep`, "Em File Thread" in
  `_sys_lwcond_queue_wait`, six CriThreads in `sys_cond_wait`. A precompile
  session over the same file reports "Precompiled 0/0 modules".

  One cause, and it is not the recompiler. `chimera_rpcs3_init` grafts the game
  into the memory filesystem as ONE file, `game/<basename>`, because one file
  is all a project ever hands over; so the executable's directory is `game/`,
  and `Emulator::GetBdvdDir` - which walks UP from the executable looking for a
  directory named PS3_GAME whose parent holds a valid PS3_DISC.SFB - finds
  neither, and /dev_bdvd is never mounted. The log says so:

      Mounted path "/app_home" to "/vfsv0_..._memory_fs_dev/game/"
      Title: EBOOT.BIN
      Version: APP_VER=Unknown VERSION=Unknown
      Ignoring empty vfs BDVD directory: '..._memory_fs_dev/config/dev_bdvd/'
      Elf path: /app_home/EBOOT.BIN

  and then the game says it in its own words:

      PPU[0x1000000] Thread (main_thread) [liblv2: 0x010a3c0c]
        '_sys_process_get_paramsfo' failed with 0x80010006 : CELL_ENOENT [1]

  It asked the system for its own PARAM.SFO, and there is not one, because
  there is no game here - only a program. It imports cellGameBootCheck,
  cellGameDataCheck and cellGameContentPermit and never gets past them.
  `Emu.GetGameDirs()` is empty for exactly the same reason, and that is the
  precompile's zero.

  **A disc folder is not something a project can carry, by design.** chimera's
  content model is a flat set of named files, every byte hashed into the movie
  (docs/multi-file.md: "a name with a path separator is structurally invalid").
  A disc belongs to it as one image, which is also what a Redump dump is. So
  the fix is not folder support; the fix is that this stops being SILENT. A
  disc game's EBOOT.BIN booted with no /dev_bdvd and no title id is now refused
  by `chimera_rpcs3_init`, naming the cause and the remedy, instead of handing
  back a machine that runs forever and arrives nowhere. `file_slots.json` no
  longer offers EBOOT.BIN as a thing to pick.

  **Verified by turning the folder into a disc.** The same content as one image
  boots and plays: `Title: ULTRA STREET FIGHTER IV`, `Category: DG`,
  `Registered BDVD game directory for title 'BLUS31218'`,
  `Elf path: /dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN`; 600 frames with the null
  renderer, main memory different at every report (c79b7e19, db59128f,
  473ac050, ce12638a, 0d4db066, 2703cb1c), 1280x720 from frame 200, 48 threads,
  and the lag count stops at 186 - from frame 187 the game is reading the pad.

  A trap for whoever makes that image: **an ISO9660 image is not a PS3 disc
  image.** `iso_file_decryption::init` reads sector 0 as the PS3 region table -
  a big-endian region count at offset 0 (1..127, or it gives up) and each
  region's last LBA from offset 12 - and a plain `xorriso -as mkisofs` image
  has zeros there, so the loader refuses with "Corrupt ISO file: Decryption
  failed" and the boot ends as `Invalid file or folder`. A fully decrypted
  single-region disc wants `00 00 00 01` at offset 0 and the last sector index
  at offset 12; that is inside ISO9660's unused system area, so it can be
  written into a finished image. Joliet has to be on (`-J -joliet-long`) or the
  names come back uppercased: rpcs3 takes the LAST volume descriptor of type 1
  or 2, which is the Joliet SVD, and only that one preserves case.

- **`--log-trace` did nothing in the box (2026-09-11).** The diagnostic added
  the same day raised the channels from the mounted `logtrace` file but
  installed the stderr mirror only `if (getenv("CHIMERA_LOG_TRACE"))` - and a
  sandboxed guest is handed no environment, which is the whole reason the file
  exists. So the levels went up and the messages went into a log file inside
  the memory filesystem that nothing outside can read. Both now ask the same
  question, and every line above came out of a sandboxed run.

- **Open: a USF4 precompile session eats the host - this is the "fails to
  compile".** Measured twice, both over the game's 12 MB EBOOT.BIN:

  - Over the bare EBOOT (no disc): 62 GB resident after 16 minutes and 114
    objects stored, `miniBox: sbrk heap exhausted` thirty-three times, then
    death on a write to a guard page (status 32 is `MB_ST_NONE`):

        chimera fault: declined 0x0000037480000000 (write): outside the guest's 4 GiB view
        [tripguard] unhandled fault: addr=0x37480000000 write rip=0x36fa80fe4bd,
          inside a registered block (page 5767168 status=32 ...) in the mmap arena

    The rip is past core.wbx's image (base 0x36f00000000, `_end` at
    +0xa7f5900), so generated code or a mapped region, not the compiler's own
    text. The host's RAM was already exhausted at that moment, so this fault
    is not cleanly the guest's alone.
  - Over the disc image, on an otherwise quiet machine: 61 objects in 8.5
    minutes, then the kernel's OOM killer, at 19:58:59, five seconds after the
    last object was written - and it took the whole WSL session down with it:
    `Out of memory: Killed process 1952 (wbxu2) ... anon-rss:7312kB,
    shmem-rss:36794872kB`.

  Nearly all of it is SHMEM, the memfd miniBox backs the guest block with, and
  the likeliest reason is in miniBox rather than in the core. This is READ,
  not yet measured with a purpose-built guest: `free_pages` in memblock.c
  memsets every page of every munmap and MADV_DONTNEED range unless the page is
  `uncommitted`, and on Linux no block is ever lazy (`pal_linux.c: out->lazy =
  false`), so no page ever is. A range a guest mapped and never wrote costs
  nothing on a memfd; UNMAPPING it writes zeros into every page, which is what
  allocates them, and nothing punches them out again. A guest's footprint is
  therefore the high-water mark of everything it has ever unmapped. A compile
  maps, fills and frees scratch space part after part (patch 0020 also
  releases 192 MiB of JIT reservation after every compile), so a session of
  hundreds of parts walks the whole 40 GiB arena; GTA San Andreas and
  Bejeweled 3 unmap little and never showed it. The fix wants miniBox to give
  the pages back (`fallocate(FALLOC_FL_PUNCH_HOLE)` or `madvise(MADV_REMOVE)` on
  the memfd instead of a memset), after a measurement proves this is the
  mechanism.

  **Every sandboxed experiment runs under a cap from now on**: `systemd-run
  --user --scope --quiet -p MemoryMax=16G -p MemorySwapMax=0 <run-wbx ...>`.
  The memory controller is delegated to the user manager on this box and it
  does stop memfd growth (a python memfd toucher under a 1 G cap is killed at
  0.9 GB with status 137). A capped run that exits 137 hit the cap: a memory
  finding, not the game crashing. One RPCS3 guest at a time.

  Also measured, so nobody chases it again: `tests/roms/flip.elf` booted
  WITHOUT firmware reaches a 16 G cap in about 35 seconds, after `vm::writer_lock
  is being used without cpu_flag::wait set by the caller!` in main_thread -
  identically on the committed core (peak 15.35 GB) and on this change's
  (15.32 GB). flip needs the firmware, which the gate always passes; with it,
  it peaks at 5.7 GB and finishes. Not a regression, and not USF4.

- **A cold precompile of a big game died after about thirty modules (2026-09-15, chimera
  issue #74): patch 0020 undid itself.** Dark Souls [BLUS30782], eight sessions, a fresh
  cache: every session stopped with miniBox "arena exhausted: 768 MiB wanted" and
  0xC000001D, while a second attempt on the warm cache worked. A 4 GiB sbrk heap changed
  nothing (no heap exhaustion at all, the same module count), so the heap overflow in
  the first run was a symptom. `MINIBOX_TRACE_SYSCALLS` on one session settled it: after
  29 modules, 30 live 768 MiB mappings (23 GiB), and every `~MemoryManager1` doing
  `munmap(range) -> 0` with the very next syscall `mmap(range, PROT_NONE)`. Patch 0020's
  `memory_release` ran BEFORE upstream's `memory_decommit`, and on POSIX a decommit is
  `mmap(MAP_FIXED, PROT_NONE)` over the range: it put the whole reservation straight back.
  0020 now releases under CHIMERA_CORE INSTEAD of decommitting. Warm caches hid it
  because they compile a handful of modules per session; USF4's 114 objects spread over
  eight sessions stayed under thirty each. Verified on the real game (Windows, GTX 1060,
  a fresh cache): all eight sessions finished, 31 to 45 objects stored each, 316 in all -
  the count the report showed for its successful second attempt - with no arena
  exhaustion and no crash. The "sbrk heap exhausted" lines remain and are harmless: the
  heap spills into the arena, which no longer fills.

- **Not carrying the disc twice (2026-09-18).** A PS3 state of Oblivion at
  frame 2400 weighed 5.45 GiB and zstd gave back 1.27x, because four fifths
  of it was the game's own installation: its caching thread copies 41 files,
  4.30 GiB, from the disc to /dev_hdd0, and /dev_hdd0 is memfs, which is the
  machine's memory, which is every state (chimera design log, 2026-09-17).

  memfs now holds such a file as a MIRROR: a reference (source node, offset,
  length) into the read-only file the project already has, with no bytes of
  its own. The copy is the game's loop of reads and writes, so memfs sees a
  read from a read-only node and, a moment later, a write of the same bytes
  to a fresh one. It remembers the last 64 reads from read-only nodes; a
  write of 4 KiB or more into a fresh empty file whose bytes equal a
  remembered read's start makes the file a mirror from that offset, and a
  write at the end of a mirror whose bytes equal the source at the matching
  offset extends it. Compared, never assumed. Any other write, a truncation
  included, materialises the file (the source's stretch is copied in) and
  goes ahead as before, so a game can do nothing that reads differently.

  Two facts cost a wrong first version. The game reads the disc through
  rpcs3's ISO layer, which serves /dev_bdvd out of the ISO node with
  `read_at` (never `read`), so the recorder has to sit in `read_at`. And the
  write offset is the DESTINATION file's, 0 for a fresh file, while the
  source offset is where that file lies inside the ISO - a mirror carries
  its own source offset, established by the first write. A Redump ISO with a
  .dkey is decrypted by that layer, so its bytes never match and nothing is
  mirrored: correct, and no gain, for encrypted discs.

  Measured on the GTX 1060 (`CHIMERA_STATE_RAW=1`, `--save-state-file`):
  frame 2400 raw 5,850,939,984 -> 1,233,338,966 bytes, save 12.7 s -> 1.7 s,
  load 4.1 s -> 1.0 s. MainRAM at 2400 byte-identical to the control core,
  also after a state-file round trip at 1800. The whole 4200-frame movie (the
  world loaded out of the cache, the game saved) with a round trip at 2800:
  MainRAM at the end byte-identical to the control, the state at 3700 raw
  1,238,860,318 bytes, save 1.4 s, load 0.8 s. Every greenzone anchor of the
  machine shrinks by the same four gigabytes. Not covered: a game that writes
  its copy through a path other than a fresh file written front to back.

- **A flush that faults on the pages it is flushing to (2026-09-18).** Arcana
  Heart 3 (BLUS31424) booted, drew two frames and stopped: black screen, the
  pad polled once in six hundred frames, the main thread waiting forever in
  cellRescGetFlipStatus for a flip the RSX never made. The RSX thread was
  parked at frame 3 inside libresc's full-screen NV3089_IMAGE_IN blit, waiting
  on the texture cache's own `m_cache_mutex` - a value of two writers, and the
  last writer was itself.

  The chain, read off the RSX thread's stack: the blit reads a page the GL
  texture cache has locked; the fault handler runs `invalidate_address`, which
  takes the cache mutex as writer and flushes the section - reads the texture
  back and writes it into guest memory through the "super pointer"
  `g_sudo_addr`. Upstream that pointer is a SECOND mapping of guest memory that
  no page protection reaches. This core has no such mapping (patch 0007 makes
  `g_sudo_addr` the base address itself - miniBox memory does not alias), so
  the write lands on the very pages the section locked and faults again, on
  the same thread, inside the handler. The nested handler asks for the mutex
  as reader and waits on its own writer lock for good. Other GPU titles
  survive because their flushes come from PPU faults and are done later from
  the RSX's work queue, with no handler frame on the stack; this is the first
  title seen to fault from within the RSX's own blit and then need a flush
  write to a page still protected. The Linux null renderer never protects a
  page, which is why the headless leg flipped every frame.

  Patch 0027: for the copy, the pages of the flushing section that the copy
  lands on are made writable and put back to the section's protection the
  moment it is done (`chimera_flush_window` around `imp_flush_memcpy`,
  Emu/RSX/Common/texture_cache_utils.h). The cache's own idea of their
  protection is never wrong, and only this section's pages are touched.
  Verified on the GTX 1060: the game reaches its first dialog ("no save data")
  by frame 200, answers the movie's Cross at 2001 with its second dialog, the
  picture changing with the input where before it was 533 pixels of OSD;
  Oblivion's MainRAM at 2400 byte-identical to the control with the same raw
  state size; Prince of Persia after three rewinds 12.34% near-black, 44,375
  colours - the known-good picture. The core gate could not be run here (no
  native build); the diagnostics that found this - a vsched thread dump with
  wait address and caller, a frame trigger, a pad-poll counter, the mutex's
  last-writer record - are kept beside the session as patches.

- **Packages install onto the console's hard disk (2026-09-18).** A PS3 gets
  its downloadable content, its patches and its unlocks as `.pkg` files, which
  the console's XMB installs to `/dev_hdd0/game/<title>` (a licence to the
  user's `exdata`). The project now carries them: a `pkg` slot in
  `file_slots.json` (any number, `.pkg`), each file pinned by hash and cited
  by the movie like the disc is. The driver (`chimera_rpcs3_add_package`,
  `install_packages` in rpcs3-driver.cpp) grafts each one into the memory
  filesystem at `packages/<name>` and hands the whole list to rpcs3's own
  `package_reader::extract_data` right after the firmware install, before
  `Emu.Init` seals the machine - so what a package installed is baseline, the
  same in both flavours, never something a savestate carries or loses. The
  reader runs single-threaded (`from_optical_drive = true`: the sandbox's
  threads are cooperative, and an installation is a sequence). A file that is
  not a package, or a patch for another version of the game
  (`check_target_app_version`), fails the load with a sentence; DLC and
  anything else the reader accepts installs. `run-native --pkg F` repeats it
  for the harness. Verified on the GTX 1060 with a 100 KB DLC-unlock package
  for a disc game (the debug package type, 15 entries): the log shows
  rpcs3 creating `/dev_hdd0/game/<title>/USRDIR/dlc/*.edat` plus the
  package's PARAM.SFO and icon, the game then boots as without the package,
  asks to install its 3.8 GB of game data (its own first-boot behaviour,
  Cross at 1550 answers it, complete by 2400), and a state at 2600 weighs
  23 MB compressed because the copied game data is a mirror of the disc
  (the not-carrying-the-disc-twice mechanism). What a real DLC needs beyond
  its package - its licence (`.rap`, a package of the licence type) for the
  encrypted content - is the game's business and rpcs3's log says so; a
  package repacked to need none works alone. Open: a package of gigabytes
  copies its content into memory files at install (the reader decrypts and
  writes; a mirror cannot describe decrypted bytes), so the state grows by
  the package's size - measure before caring.

- **A save carries the console's date (2026-09-18, issue #96).** The memory
  filesystem stamped every file with a counter that started at 1, so the
  game's own save list showed January 1970. A file's mtime is now the
  machine's clock: the frozen date miniBox answers `clock_gettime` with
  (1495889068, 2017-05-27) plus the virtual seconds the machine has run
  (`vsched_now_ns`), which is also what `sys_time_get_current_time` tells
  the game. The same movie stamps the same save with the same date on every
  run; a counter was as deterministic but a lie. Verified on the GTX 1060:
  the three Oblivion saves the movie makes export as before (12 files).

- **Color buffers are written back to memory (2026-09-18, issue #97).** A
  PS3 game reads its own picture whenever it likes - Oblivion draws the
  save's thumbnail from the screen - and rpcs3 does not write finished
  render targets into the console's memory unless "Write Color Buffers" is
  on, so the icon was black. `writeColorBuffers` is a project setting,
  default on (`g_cfg.video.write_color_buffers`, pinned in
  `pin_configuration`), a bool like xemu's `idleSkip`. It costs a readback
  when the game reads its picture; a movie is unaffected either way, only
  pixels move. Measured on the GTX 1060: the 3800-frame Oblivion leg 4m41
  off, 5m06 on (+9 percent); the exported ICON0.PNG 70 KB of prison cell
  where it was 187 bytes of black; the game's own save list shows the
  thumbnail and "Sat May 27 12:45:06 2017", the console's clock. The
  setting exposed the two stalls below, which were there all along.

- **The RSX reads through the pages it locked (2026-09-18, patch 0029).**
  With write-back on, Oblivion's third frame stopped the machine: every
  thread waiting, the RSX on the texture cache's own `m_cache_mutex`
  (value 0x4001: one reader in, one writer waiting - both the RSX). Found
  with a ring of every shared_mutex operation (mutex, op, vthread, return
  address) printed from a frame-triggered thread dump: `GLGSRender::end`
  takes the cache's reader lock for a texture search, the search asks a
  surface to load its contents from memory (`memory_barrier` under
  `get_merged_texture_memory_region`), the read lands on a page the cache
  locked (no access, so that a CPU write is seen), faults, and the fault
  handler asks for the writer lock. Upstream that read goes through the
  super pointer, a second mapping of guest memory that no protection
  reaches; this core has one mapping (patch 0007). Patch 0027 fixed one
  instance of this (libresc's copy, one level down, inside the handler
  itself); the general rule replaces guessing at sites: a fault the RSX
  takes while it is inside its cache (`chimera_texture_cache_busy`, the
  mutex not free), on a page the emulator itself locked, is served as the
  super pointer would have served the access - the page is opened (rw) and
  the access retried - and put back to what the emulator believes it is
  before any other thread runs. The belief is the last protection the
  emulator asked for, noted per page in `utils::memory_protect`; the moment
  is the RSX giving the machine away (vsched's new leave hook, run on the
  thread that switches out), exact because the scheduler is cooperative.
  A whole locked run of pages opens at the first fault, so a 720p surface
  costs one fault, not nine hundred. `GetWindowCount` (run-native, run-wbx)
  counts the faults served this way: two or three a frame on Oblivion, a
  few hundred pages, no measurable cost.

- **A thread waiting for the RSX's flush gives way (2026-09-18, patch
  0030).** Past that, the machine stopped again at frame ~1450, burning a
  core with the frame counter frozen: the CPU reading its picture faults on
  the locked buffer, and a fault from a thread that is not the RSX becomes
  a deferred flush the RSX performs - the faulting thread waits for it in
  `work_item::producer_wait`, `utils::spin_wait`, a pure spin. The
  machine's threads are cooperative, so the spinner never let the RSX run.
  `CHIMERA_YIELD()` in the loop, the rule of patch 0011 applied to the one
  spin it had missed (the only `spin_wait` user in the tree). A frozen frame
  counter with a busy process is this shape: a spin that never reaches
  vsched; a quiet process with every thread parked is the other (a lock).

- **A machine with no network still boots its games (2026-09-19, patch
  0031, chimera#109).** Two of three boot crashes read straight off the
  crash notes players attached. Dragon Ball Z Burst Limit: its NpMatching
  thread binds a P2P socket at boot; `nt_p2p_port`'s constructor threw when
  `socket()` failed (it always fails here: host-plumbing answers ENETDOWN),
  and a thrown exception on a PPU thread ends it and leaves every other
  thread waiting - the deadlock miniBox reported. The port now stays
  unbound and `create_p2p_port` says so, so the game's bind returns
  ENETDOWN, the answer the #84 socket fix already gave plain sockets.
  Injustice: `sys_ss_random_number_generator` read `/dev/urandom`, which
  the sandbox has not got, and threw; it now draws from a xoshiro256**
  inside the machine, seeded once - a host's entropy is a number a movie
  cannot replay anyway, and the generator's state lives in guest memory,
  so a savestate carries it. The third (Mortal Kombat Komplete) is a
  `memcmp(NULL, heap, 65)` in guest code at 0x36f052f15c1 of the 09-19
  core; without its minidump the caller is unknown - asked for.
  How to read such a note: `rip` inside `0x36f00000000 + core.wbx` resolves
  with `addr2line -e core.wbx` on the package's own core.wbx (not
  stripped); a fault "in host code" at process exit in `RtlFreeHeap` under
  `LdrShutdownProcess` is the CLR tearing down after the real event, not
  the event.

- **A disc installs its own content too (2026-09-19, chimera#108).** A PS3
  disc can carry packages the console must INSTALL before it runs the game:
  `PS3_GAME/INSDIR`, `PS3_GAME/PKGDIR` and `PS3_EXTRA`, which a real console
  unpacks onto `/dev_hdd0/game` the first time the disc goes in. Emulator::Load
  finds them and hands the list to the `on_install_pkgs` callback; this driver
  answered `return false`, and that one word is the whole of the boot result
  "Game install failed" that Resident Evil 5 Gold Edition and Strider Hiryuu
  stopped on. The callback now installs them exactly as the project's own
  `.pkg` files are installed - `install_pkg_paths` is the reader loop both
  share, one thread and in order - and the paths need no grafting because they
  are already the emulator's own, into the ISO's virtual device or into the
  memory files a disc archive was read into, so the reader decrypts them where
  they lie. It runs inside `BootGame`, which is inside the load, so what the
  disc installed is baseline like a project package's is, the same in both
  flavours. A failure now says WHY: the boot result alone names no file, so
  the installer's sentence is what the core reports.
  Proved on a disc built for it - Bejeweled 3 dumped to a folder with a
  15-entry package dropped into `PS3_GAME/INSDIR` and repacked as a .zip: the
  core before the change stops at "boot failed: Game install failed", the core
  after it logs `Found INSDIR`, `1 package(s) from the disc installed into the
  machine (313054 bytes of memory files)`, and runs 300 frames with RAM moving
  every report. A synthetic ISO is NOT a way to test this leg: rpcs3's ISO
  loader wants a real PS3 disc image (region information in the header) and
  refuses a plain ISO 9660 one built with xorrisofs as "Corrupt ISO file".
  Proved again on the disc the issue names, Resident Evil 5 Gold Edition (USA)
  v02.00, 14.4 GB: the 2026-09-16 core stops at "boot failed: Game install
  failed" and this one reads `PS3_GAME/INSDIR/DATA000.PKG` (15.7 MB, 22
  entries) straight off the ISO's virtual device and installs it - the Gold
  Edition's own update, `/dev_hdd0/game/BLUS30491/USRDIR/EBOOT.BIN` and its
  .arc resources, 210 MB of memory files, about 45 seconds. That disc then
  stops for a reason of its own: it is a Redump image and its key was not
  given (below).
  And proved to a PICTURE on the 1060, because Bejeweled 3 still never draws
  (the open item above): Oblivion's disc dumped to a folder with the same
  package in `PS3_GAME/INSDIR`, repacked as a 4.99 GB stored .zip, played
  through `chimera-run --gpu --draw-every-frame` with that project's own input
  log. The 2026-09-16 core refuses the disc outright ("boot failed: Game
  install failed", no frames); this one installs the package and draws - the
  screenshot at frame 600 is Oblivion's character-creation screen, the same
  picture the unmodified disc gives at that frame.

- **A Redump image says it needs its key (2026-09-19).** A Redump PS3 disc
  image keeps the disc's filesystem in the clear and its DATA regions
  encrypted; the key is a separate file (`<image>.dkey`, the Disc key slot,
  which this core already mounts where rpcs3's loader looks). Handed over
  without one, the image reads as a disc whose executable is not an
  executable - rpcs3 logs `Invalid or unsupported file format ... Not an ELF`
  and the boot result is "Invalid file or folder", which names neither the
  cause nor the cure. The core now tells the two apart: every bootable PS3
  disc begins `PS3_GAME/USRDIR/EBOOT.BIN` with an SCE header, so when a boot
  of a disc IMAGE fails and that file starts with neither SCE nor ELF, the
  data never decrypted - the core says so, and says to give the .dkey (and
  says it differently when a key was given and did not fit). The test costs
  nothing on a good disc, because it runs only after a failed boot, and a
  decrypted image (Bejeweled 3) still boots untouched.

- **Resident Evil 5 Gold Edition, end to end (2026-09-20, chimera#108).** The
  disc the issue names, with its Redump key in the Disc key slot, now plays to
  its own title screen. The key is the image's `.dkey` - 32 ASCII hex
  characters, no newline needed, since rpcs3's loader reads exactly 32 bytes
  and takes a 16-byte file as raw instead. The log reads: `Found INSDIR`,
  `package .../PS3_GAME/INSDIR/DATA000.PKG (from the disc): content type 0x4,
  22 entries`, `Created file .../dev_hdd0/game/BLUS30491/USRDIR/EBOOT.BIN`,
  `1 package(s) from the disc installed into the machine (210569145 bytes of
  memory files)`, `Updates found at /dev_hdd0/game/BLUS30491/` - so the machine
  then boots the EBOOT the DISC installed, not the one on the disc, which is
  what INSDIR is for. Waterboxed, null renderer, 60 frames twice: identical
  RAM, TTY, video and audio digests, memory different at every report.
  The picture, on the 1060 through `chimera-run --gpu --draw-every-frame`:
  frame 500 is the game's own "game data must be installed" prompt, frame 900
  its progress bar a quarter of the way, frame 2800 the RESIDENT EVIL 5 title
  screen with PRESS START. Three controls on the same disc and project: the
  2026-09-16 core fails with "Game install failed" WITH the key as well as
  without it (the key is not what fixes this), and this core without the key
  gives the Redump sentence above.
  The cost worth knowing: this game's own data install writes several GB into
  the memory filesystem (the run's process grew from 4.9 GB to about 10.9 GB
  while the bar filled), because /dev_hdd0 is memory. Whatever a PS3 game
  installs is machine state, and a savestate carries it.

- **A rebuilt renderer lets go of its surfaces before it frees them
  (2026-09-20, chimera#110, patch 0032).** Loading a state on the GL renderer
  killed the machine every time on a real game: `the core crashed: it read or
  ran address 0000000000000008`, inside `__dynamic_cast`, or - when the freed
  memory still read like an object - rpcs3's own
  `Verification failed (object: 0x0)` out of `gl::as_rtt` in
  GLRenderTargets.h. Oblivion on the GTX 1060, one save and load at frame 400
  and then play: dead within twenty frames.

  `chimera_gl_teardown` (patch 0021) inherited upstream's `on_exit` order,
  which destroys the surface cache before the texture cache. Those two are not
  independent: a texture-cache section that covers a render target keeps the
  surface's address in `vram_texture` and holds a LOCK on it, and clearing the
  cache unprotects each locked section - which asks the section for its surface
  again (`post_protect` -> `get_render_target()`, a `dynamic_cast`) so it can
  give the lock back. `gl_render_targets::destroy()` has already run
  `invalidate_all()` and cleared `invalidated_resources` by then, so every one
  of those pointers names a freed object, and the cast walks a vtable musl's
  allocator has written its own bookkeeping over.

  Upstream has the same order (VK too) and gets away with it: `on_exit` runs
  once, at the end of a process, where a freed object usually still reads like
  itself. Here the same code runs on EVERY state load, so it is a crash rather
  than a warning. The fix is the order - the texture cache goes first, while
  the surfaces it points into are still there - and nothing else in the
  teardown depends on the texture cache, so moving it earlier only leaves more
  alive around it.

  Proved by two packages built from the same tree, differing only in patch
  0032: the control dies at frame 400 with the fault above, the fixed core
  finishes 620 frames, and its picture after the load is the picture the SAME
  core gives with the rebuild turned off (CHIMERA_GL_KEEP_OBJECTS_ON_LOAD=1) -
  0.03% of pixels differ at frame 420, 2768 colours either way; 0.65% at frame
  600. Five saves and
  loads in one run (400, 450, 500, 550, 600) all rebuild cleanly and the run
  ends on a 105,182-colour picture. A cross-PROCESS reopen - the state saved
  in one run, loaded in another, which is what opening a saved project is -
  crashed identically before and is clean after.

  Why the gate did not catch it: `gpu:rewind` and `gpu:context` load a state on
  the GL renderer, but flip.elf draws with the CPU and flips, so its texture
  cache has no section covering a render target and nothing holds a lock to
  give back. The shape that finds this needs a game.

- **A digital-only title is its .pkg (2026-09-20, chimera#66).** Half of that
  issue shipped in 59bd3d3: a project's Packages slot installs .pkg files onto
  the console before it starts - DLC, patches, unlocks, a .rap of its own. The
  other half is a game that was never pressed on a disc, where the package IS
  the game and there is no disc image to put in the Game slot.

  The Game slot now takes a .pkg, and what happens to it is what a console
  does: `install_game_package` runs the same `package_reader` that installs a
  DLC, into the memory filesystem, before the seal - so what it installed is
  baseline and not a savestate's - and then hands `Emulator::BootGame` the
  USRDIR/EBOOT.BIN the install CREATED, under /dev_hdd0/game/<title id>/.
  rpcs3 boots an installed title by that path every day: `Emulator::Load` sees
  the path is inside /dev_hdd0/game (`from_hdd0_game`), takes argv[0] and the
  game directory from it, and never looks for a /dev_bdvd. Nothing new had to
  be taught to the emulator; the work was deciding what to hand it.

  The game package installs FIRST and the project's own packages after it, so
  a title's patch and its DLC land on top of the game in the order a console
  would install them.

  Which packages are a game is decided by WHAT THE INSTALL PRODUCED and by the
  package's own HEADER, never by guessing. `package_reader` already reports the
  USRDIR/EBOOT.BIN each package created (`bootable_paths`, which the Qt front
  end uses to offer a freshly installed game), so `install_pkg_paths` passes
  that back: a package that created none is content FOR a game - a DLC, a
  licence, a theme - and is refused by name.

  That is not enough on its own, and the content proved it. A digital title's
  UPDATE is indistinguishable from its game by everything except the header:
  Super Stardust HD's 6.00 update says CATEGORY=HG exactly as the full game
  does, carries the same TITLE_ID, installs into the same
  /dev_hdd0/game/NPEA00014, and produces a bootable USRDIR/EBOOT.BIN - and
  booting it is booting part of a game. (A DISC game's patch says CATEGORY=GD
  and is a different shape; rpcs3's own `check_target_app_version` only checks
  GD, so it lets a digital update install over nothing without complaint.) The
  header is honest where the file system is not, and the two packages differ
  there exactly:

      Super-Stardust-HD_Full.pkg    type 0x4a  EBOOT, HDD_MC, RENAME_DIRECTORY
      Super-Stardust-HD_Update.pkg  type 0x5e  ... plus REQUIRE_LICENSE, PATCH
      echochrome (NPUA80134)        type 0x4e  ... plus REQUIRE_LICENSE

  So the Game slot refuses PKG_FLAG_PATCH, read from the header BEFORE the
  install, because the install is gigabytes and the answer is in the first
  kilobyte.

  **The licence.** PKG_FLAG_REQUIRE_LICENSE is the other half. A PSN title that
  was paid for is NPDRM: its executable is encrypted and the key is a 16-byte
  per-account licence, a `.rap` named after the content it unlocks
  (UP9000-NPUA80134_00-ECHOCHROME000000.rap). rpcs3 looks it up by that exact
  name - `rpcs3::utils::get_rap_file_path`, asked by unself when it decrypts an
  NPDRM executable and by unedat for EDAT data - under
  /dev_hdd0/home/<user>/exdata/, and converts it to a RIF key in memory. So the
  new Licence slot is almost no code: the file goes into exdata under its own
  name and the name is the whole of the plumbing. Without it echochrome stops
  at rpcs3's "Failed to decrypt content", which names neither the cause nor the
  cure; with the header flag in hand the core can say what is missing instead.

  In the project: `.pkg` joins the Game slot's formats, a Licence slot takes
  `.rap` files (any number, name pinned by `namePattern` to rpcs3's own content
  id rule, because renaming one makes it invisible), the Disc key slot is
  `exposedWhen` NOT a package game (a package carries no encrypted disc), and
  the firmware requirement grows an `any` so a package game asks for the PUP
  exactly as a disc game does.

  One trap found by reading rather than by running. Upstream's desktop
  `resolve_path` is Qt's canonical path, which has NO trailing separator, and
  `Emulator::Load` leans on that: it appends its own '/' to a resolved
  directory and cuts the boot path by the result's length. The default callback
  is the identity, which leaves the separator on - so a title booted out of
  /dev_hdd0/game would have got an argv[0] and a game directory one character
  short. It never showed before because no path this core booted came from
  there. The callback now strips trailing separators, which is all there is to
  canonicalise in a memory filesystem.

  What a package game costs: the install writes the whole game into
  /dev_hdd0, which is memory, so its machine state is as large as the game -
  the same bill a disc game's own data install runs up (chimera#108).

  PROVEN, on two real digital-only titles:

  - **Super Stardust HD, NPEA00014** (Super-Stardust-HD_Full.pkg, 305 MB,
    CATEGORY HG, APP_VER 04.00, no REQUIRE_LICENSE): installs and boots to
    running frames with firmware 4.82. It is a slow machine - 42 threads on the
    PPU interpreter - so it is not what the gate leg runs.
  - **echochrome, NPUA80134** (107 MB, CATEGORY HG, APP_VER 01.02,
    REQUIRE_LICENSE): with its .rap in the Licence slot it installs, decrypts
    and runs 120 frames, memory different at every report. Without the .rap it
    is refused with a sentence naming the licence.
  - The 6.00 update and the Saint Seiya all-DLC package are both refused by
    name in the Game slot, for their two different reasons.

  Found on the way and nothing to do with packages: **native and sandbox did
  not agree on the memory of every real game** (chimera#120). Chased with
  `--ram-out`, which `run-native` grew for it, it turned out to be two separate
  things, and one of them is fixed.

  **The wall clock, FIXED (patch 0033).** `sys_time_get_current_time` was the
  one clock in the emulator patch 0003 did not route through vsched: it read
  the host's CLOCK_REALTIME. So the native reference wrote today's date into
  the machine's memory where the box - whose clock miniBox freezes - wrote
  2017-05-27, and two native runs a minute apart differed from EACH OTHER.
  `sys_time_get_timezone` read the host's timezone for the same reason. Both
  are now the machine's own: the console's calendar starts at
  `VSCHED_EPOCH_SECONDS` (the date the box's clock is frozen at, and the date
  memfs already stamps its files with) and advances with `vsched_now_ns`, and
  the console keeps UTC with no summer time.

  Prince of Persia (BLUS30214) is the clean case: a plain disc image, no
  package and no licence anywhere near it, and its whole divergence was **8
  bytes out of 256 MiB** - two copies of one `time_t` at guest 0x1491904 and
  0x1492d17, the second inside a record named MEMORYLOGGERFILE. Arcana Heart 3
  is the loud one: 4580 bytes, a ring of records each holding the `time_t`, the
  broken-down fields and the `ctime` string - "Sun Sep 20 14:27:04 2026"
  natively against "Sat May 27 12:44:28 2017" in the box. Both now agree, and
  both are deterministic natively again.

  **The virtual clock, STILL OPEN.** What remains is smaller and different: on
  titles that drive the SPUs the two flavours' VIRTUAL time drifts apart by
  microseconds, and a game that measures elapsed time stores the difference.
  echochrome still differs by 35 bytes at frame 120 (guest 0x3463b6, 0x40269a,
  0x47bd22 and neighbours: floats and doubles holding frame times, with machine
  time 4 us apart at frame 3); Saint Seiya by 27; Arcana Heart 3 by 19 tick
  counters once the calendar is out of the way; Resident Evil 5 Gold by 2093,
  of which 2081 are one 0x824-byte high-entropy block at guest 0x17bd79c that
  differs end to end and 12 are tick counters. It grows barely at all -
  echochrome is 35 bytes at 120 frames, 37 at 600, 45 at 1800 - so the two
  machines are not running away from each other, and each flavour stays
  bit-deterministic run after run, which is the property a movie rests on. It
  is not the host clock and not host addresses: two native runs with different
  ASLR bases (vm::g_base_addr 0x737b00000000 against 0x728900000000) are
  byte-identical, and forcing the SPU interpreter in both flavours does not
  remove it (it moves echochrome's first differing frame from 3 to 2). The two
  builds are the same source through two toolchains, and somewhere one of them
  charges the scheduler differently.

  So `pkg:boot` still reports the flavour comparison rather than failing on it,
  and now says what the difference IS. Byte equality between the flavours holds
  for Bejeweled 3, GTA San Andreas, Oblivion, Ultra Street Fighter IV and
  Prince of Persia and not for echochrome, Saint Seiya, Arcana Heart 3 or
  Resident Evil 5 - five of the nine titles here that boot, at 300 frames on
  the null renderer. What the gate can honestly claim about a real game is that
  each flavour reproduces ITSELF exactly, which is what a movie rests on, and
  not that the two flavours reproduce each other.

  Re-measured on current main after the wall-clock fix (patch 0033), --ram-out,
  300 frames, null renderer (chimera#120, the three titles the issue names):
  Prince of Persia (BLUS30214, plain iso) is now 0 bytes - native == sandbox,
  read both off the Windows share and off a local copy - and Bejeweled 3
  (BLUS30865) is 0. echochrome (pkg + rap) still differs, 43 bytes in 256 MiB,
  and the words are what the issue guessed: big-endian doubles holding frame
  times (1981.975 native / 1981.960 box; elapsed deltas 0.000156 / 0.000155 s)
  and small tick counters (0x14d4 / 0x14d1), the two machines microseconds
  apart in virtual time. A game CAN observe them - it stored them - so this is
  not "state a movie never reads"; the honest guarantee is the narrower one:
  each flavour is bit-deterministic run to run (two native runs with different
  ASLR are byte-identical, checked here), and a movie replays in ONE flavour,
  so a native/sandbox difference is not a desync. Because the three are not all
  equal, `pkg:boot` is left reporting the comparison, NOT tightened to fail.

  Also measured: Super Stardust HD is SLOW here - 42 threads, about half a
  minute of wall time per frame on the PPU interpreter, and the RSX FIFO asks
  for a bigger wake-up delay in the log. It boots and it runs; it is not what
  the gate leg should run, which is why the leg takes the packages in tests/
  roms-local smallest first and stops once each of its questions has an answer.

  Also fixed on the way: `native.mk` never built archive.cpp, sevenzip.cpp or
  the 7-Zip decoder, so the native reference could not LINK at all once a disc
  could arrive as an archive. guest.mk had them; the two lists are the same
  list now.

- **A game's install off an ENCRYPTED disc costs the machine nothing either
  (2026-09-20, chimera#108).** A PS3 game that installs its own data onto
  /dev_hdd0 writes gigabytes into the memory filesystem, which is the machine's
  memory and so is every savestate. A state with one in it is larger than the
  ENTIRE default greenzone budget (4 GB), so such a game holds no frames in its
  greenzone at all and cannot be TASed. The disc mirror (76b992c, "not carrying
  the disc twice") removed that for Oblivion, whose image is in the clear; for a
  Redump image it did nothing, and Resident Evil 5 Gold is a Redump image.

  Two reasons, both measured, both now fixed:

  1. **The comparison read the image, not the disc.** rpcs3's ISO layer serves
     /dev_bdvd out of the image, and for a Redump dump that means decrypting its
     DATA regions on the way past. So what the game wrote to the hard disk was
     nowhere in the file the mirror compared it against. The filesystem now
     reads a disc IMAGE the way the CONSOLE reads it: `memfs_mark_disc_image`
     builds rpcs3's own `iso_file_decryption` from the image and the .dkey
     beside it, once, before the machine runs, and `source_reader` decrypts the
     whole sectors a request falls in (the IV is the sector's number) before
     comparing or serving. A decrypted image gets no decryption object and is
     read exactly as before.
  2. **The comparison required one read as long as the write.** It had to be at
     least as long, which quietly assumed the layer serving the disc hands over
     a whole chunk in one go. A decrypted image's does; a Redump image's does
     not - `iso_file_encrypted::read_at` reads the first sector, then the
     middle, then the last, so for a 64 KiB chunk the longest read remembered
     was 61 KiB and nothing ever matched. The length was never part of the
     judgement - the bytes are compared in full either way - so it is gone.

  Measured on this box, native, with the counters the runners now print
  (`memory files: N bytes, M file(s) held as the disc's (H bytes), K bytes
  copied in, D bytes decrypted to tell`; the figures start again once the
  machine is built, because what the emulator installed before that is baseline
  and costs a state nothing):

  | disc | what the 25 MB EBOOT.BIN copy costs the machine |
  |---|---|
  | RE5 Gold, Redump + key, read through the key | 0 bytes: 1 file held as the disc's, 25,067,328 of them |
  | the same, read as it lies (the control) | 25,067,328 bytes, held nothing |
  | Oblivion, in the clear | 0 bytes, unchanged from before this |

  And on the savestate itself, sandboxed, RE5 Gold at frame 30, the state
  `run-wbx --save-state` writes (uncompressed, what miniBox hands over):

  | | state |
  |---|---|
  | nothing installed | 1,138,129,704 |
  | the 25 MB file installed, held as the disc's | 1,138,432,808 (+303,104) |
  | the same, read as it lies (the control) | 1,164,237,608 (+26,107,904) |

  So the install costs a third of a megabyte instead of twenty-six, and the
  saving scales with the install: this game's real one is several gigabytes.
  The 1.138 GB underneath is the machine proper, which is the flat figure the
  chimera design log's 2026-09-20 entry measured.

  And Oblivion's real install, 3000 frames of its own caching thread: 43 files
  and 4,617,596,644 bytes held as the disc's, nothing copied, and the machine
  byte-identical to a run before the change at every report.

  One thing this direction DEPENDS on, and it is not settled here: a mirror
  keeps no second copy of the bytes, so it is only as good as the host handle
  it reads the image through - and a savestate cannot carry that handle
  (#118, `8dd8727`, which reopens one the sandbox has closed). That plaster
  heals a descriptor that is GONE; one the sandbox has since handed to another
  open of another file would read the wrong bytes silently. The cure is for
  miniBox to carry its open files in a state, which its format already
  describes and its host does not yet write. Everything above - a game's whole
  install held as a reference rather than copied - rests on that handle, so it
  is not a footnote to this work but a condition of it.

  Found writing the legs, and it is not this change's: on a real game the two
  flavours are NOT frame-line identical. RE5 Gold ends five frames on
  `time 1083339` natively and `1083353` in the box, with every memory, TTY,
  video and audio digest identical and identical with the copy probe or without
  it. That is the divergence issue #120 is about, and it is why these legs use
  `same_memory` (the ram digests and the TTY) rather than `same_both`, and say
  so rather than claiming an equality they did not check. `disc:boot` on
  Bejeweled 3 still passes `same_both`; a disc that agrees, agrees.

  The gate legs: `disc:install` (a disc in the clear), `disc:install:encrypted`
  (a Redump image, with the control IN the leg - the same copy read as it lies
  must keep every byte, or the leg fails for having proved nothing) and
  `disc:install:state` (the reference saved in one process and read back in
  another, which is what opening the project tomorrow is). Each copies a file
  off the disc onto the hard disk 64 KiB at a time, the way an installer does,
  and then READS IT BACK against the disc byte for byte, because a reference
  that answered with the wrong bytes would be a corruption nothing else here
  would catch. `--disc-copy` drives it in both runners.

  It is a STAND-IN (chimera docs/gates.md, mode E) and does not run a game's
  installer. What it does not cover: a game that TRANSFORMS what it installs -
  unpacks an archive, re-encodes, writes a file of its own composition - for
  which no reference can stand and every byte is machine state. Nothing here
  makes that case worse; nothing here helps it either.

  Not measured: Resident Evil 5's own installer end to end. It REACHES its
  prompt on the null renderer - `cellMsgDialogOpen2(type=0xa5, msgString="The
  HDD access indicator will blink when data is being written.")` every frame
  from about frame 1000 - and cannot get past it there, for a reason that is
  structural rather than a matter of pressing harder: rpcs3 creates the overlay
  `display_manager` only for the OpenGL and Vulkan renderers
  (`RSXThread.cpp`, `use_native_interface && renderer == opengl|vulkan`), so
  with the null renderer there is no dialog to accept and the game asks again
  forever. Worth knowing generally: **any game that gates its progress behind a
  system dialog is unreachable on the null renderer**, which is what every leg
  but the gpu ones runs. Driving that installer needs a renderer.

- **Dead or Alive 5 Last Round dies in the main menu (2026-09-21, chimera#127).**
  Reported against chimera 09d76ccd with this core at 8dd8727. NOT FIXED; what
  follows is what the evidence says and what has been ruled out, so the next
  person does not start from guesswork.

  The guest's own last words, out of `minibox-diag.log`:

      rpcs3 fatal: RSX [0x05b8bf0] Thread terminated due to fatal error:
      Verification failed (object: 0x0)
      (in .../Emu/RSX/GL/../Common/surface_store.h:622, in function
       'void rsx::surface_store<Traits>::free_rsx_memory()')

  Line 622 is `ensure(surface->has_refs()); // "Surface memory double free"`.
  A surface in the cache had its reference count at zero when the cache came to
  free it. The RSX thread died there; every other thread then waited on it, and
  what miniBox reported - a 26-thread deadlock at frame 742 - is the
  CONSEQUENCE, not the cause. Chimera handled the death correctly: the dialog,
  the frame, the inputs safe. (The process died ninety seconds later of an
  unrelated host bug, miniBox's fault handler reading a layout pointer that a
  destroyed machine had left behind. Fixed in miniBox; see chimera's design log
  for 2026-09-21. The two are independent and neither causes the other.)

  **The session had no state loads, and that rules out a whole family.** The
  report's screenshot shows the OSD: `13 fps`, `545/431 (Finished)`, then two
  numbers - 92 in the alert colour and 0 under it. In `OSDManager.Draw`'s
  order, with the default positions (y=42, y=56), those are the lag counter and
  the RERECORD COUNT. Zero rerecords: nothing was rewound, no state was loaded,
  so the context id never moved and the renderer never rebuilt. Every GL crash
  this core has had before went through that rebuild - issue #110's teardown
  order (patch 0032), issue #43's moved context id (patch 0021), the black
  picture of 2026-09-17 (patches 0024/0025) - and none of them is this. This
  one happens in ordinary forward play.

  **Confirmed by running it.** `run-native --gl-rebuild-at 620,630,640` (new,
  below) forces the renderer to tear down and build again three times over
  while the menu is on screen, with no state restored. The machine carries on:
  no assert, no change of behaviour. So the teardown path is not it either.

  **It does not reproduce headless on Linux in 900 frames.** With the default
  settings - PPU interpreter, GL through the bridge on Mesa's llvmpipe - the
  game boots, plays its menu video and sits in the menu for 900 frames with
  main memory different at every report and no assert. So whatever the surface
  cache did wrong needs something this run did not have: a real GPU's timing,
  the reported core commit, or a decoder setting the crash note does not
  record.

  **Found on the way, and it is its own bug: with `ppu_decoder=llvm` the
  machine stops dead at frame 640.** Two runs reach frame 640 with the same
  main-memory digest `b496354fcaf8bd52` and stay there for as long as they are
  left - one for eighteen minutes - with the process burning a core.
  `chimera_rpcs3_debug_ppu` on the stopped machine (`kill -SEGV` on the runner,
  whose fault handler prints the report) shows all 86 threads parked and the
  virtual clock stopped at 11.778 s: every PPU is waiting, so what is burning
  the core is the RSX, and by the rule in patch 0030's entry above, a busy
  process with a frozen frame counter is a spin that never reaches vsched. The
  interpreter runs straight past the same point, so this is the recompiler's
  timing, not the game's. Reproduce with
  `CHIMERA_PPU_DECODER=llvm run-native --renderer opengl-hw --frames 800`.

  One difference from the reported build that must be closed before any of this
  is called a reproduction attempt: this tree carries patches 0034 and 0035
  (the two RSX FIFO fixes of 2026-09-20, commits d8d78d4 and 1268e8b) which the
  reported core at 8dd8727 does NOT. And the decoder the user ran is unknown -
  13 fps suggests the LLVM recompiler rather than the default interpreter, but
  the crash note does not say. Worth fixing on its own: **the crash note should
  carry the core's settings**, or a report like this cannot be re-run.

  Where to go next, in order: the GTX 1060 through the frontend (a real GPU
  gets past frame 640), the reported core commit rather than this tree, and an
  instrument on `free_rsx_memory` that names the surface and the call site
  before the ensure fires (drafted, not applied - `std::source_location` as a
  defaulted argument gives the caller for free).

  **New harness: `run-native --gl-rebuild-at N[,N...]`.** It calls
  `chimera_gl_host_state_loaded`, which moves the context id exactly as the
  engine does on a state load, so the renderer's teardown and build-again can
  be driven with NO state restored. That separation is worth having on its own:
  every previous crash in this area was only reachable through a savestate
  round trip that costs minutes on a real game, and the two halves of a load -
  the memory restore and the GL rebuild - had never been tested apart.

- **A licensed .pkg with no .rap crashes while compiling, and half of it is the core's (2026-09-21, chimera#123).** Five licensed titles (After Burner Climax, DuckTales Remastered, House of the Dead 4, Resogun, Super Street Fighter II Turbo HD) each crashed at the frontend's Pre-compiled modules step with a minibox-diag.log; two trials (Hard Corps Uprising, Sonic 4 Ep II) were fine. The five diag logs all show the SAME fault: a WRITE in host code at ntdll's heap coalesce (`mov %ax,0xc(%rdi,%rdx,8)`, `[0 block(s) registered]` - the box already torn down), i.e. late process teardown, not the guest. The trials are demos (no REQUIRE_LICENSE) and compile; the five are NPDRM and the reporter has no .rap. The logs decide the reading: it is a licensed pkg WITHOUT its .rap that crashes during precompile, AFTER the graceful refusal text is emitted (reading (a)). With the .rap, echochrome precompiles (152/155) and boots.

  It is TWO crashes on one refused path, reproduced headless with echochrome (NPUA80134) + its .rap in tests/roms-local:

  (A) FIXED, and it is the core's. `Emulator::Load` creates every g_fxo object BEFORE it decrypts the NPDRM executable; a Load that returns `decryption_error` destroys none of them, and `Emulator::Kill` returns at once on a machine that never ran, so the objects live until process exit where `manual_typemap::~manual_typemap()` ensure()s they are gone and aborts. The native reference died with SIGABRT (exit 134) AFTER printing the licence sentence - proven by name: `run-native --precompile 0/1 echochrome.pkg` (no rap) exits 134 on the pre-fix build, exit 1 (clean refusal) after; with the rap it precompiles either way. The sandbox never showed it on Linux (the box is dropped without running guest static dtors). Fix: `chimera_rpcs3_init` takes a refused machine down (`discard_refused_machine`: `Emu.Kill` + pump-to-stopped + `Emu.CleanUp`, the last of which is `g_fxo->clear()` and nulls the typemap) on every refusal AFTER `Emu.BootGame` was asked. No effect on the success path (with-rap still 152/155, exit 0). The `pkg:licence` gate leg is tightened to match (gates.md mode B - it had passed on the sentence alone): it now requires the refusal to RETURN 1, in both flavours and once more as a `--precompile` session (the shape the report came in), and it goes red on the pre-fix build (`exited 134 instead of 1`) and green after.

  (B) STILL OPEN, and it is not the core's g_fxo. The Windows SANDBOX still writes that ntdll teardown fault with the FIXED core.wbx (stock engine, GTX 1060 box), so it is not (A); it also fires for a CONTENT pkg refused before `Emu.Load` ever runs (no g_fxo at all); and withholding the GL bridge does not remove it (not the bridge). It does NOT reproduce on Linux - the Linux frontend and run-wbx exit 64/1 cleanly on the same refused pkg with the same core, broken or fixed - and the Windows SUCCESS path (Super Stardust, no licence) tears down clean (exit 1, no diag), matching the reporter (demos compile, licensed crash). So (B) is a Windows-only host-side heap corruption planted on the refused-init path and surfacing when ntdll walks the heap at process exit; the stuck 1-thread Chimera.exe it leaves cannot be taskkill'd. Not localized: Linux ASan on `chimera-run` OOMs the box under the mandated 16-18G cap (SKIP, cap not raised), the fault is Windows-only so Linux gdb/valgrind cannot see it, and Windows PageHeap/gflags via an IFEO key was refused as persistence. Next step for whoever has the box: Application Verifier or PageHeap on Chimera.exe, then the abort/free is in `ce_session_free`/`wbx_destroy_host` of a session whose Init returned 0 (created + mounted + Init'd, never sealed).

- **Still open after M5**: the lazy 20 GiB block on Windows under memory
  pressure, LLVM recompilers, and the recompilers' half of RawSPU - the
  interpreter reaches the window through vm::write and ppu_feed_data, but
  compiled code does not go through either and would still fault.

## Risks, ranked

1. **vsched completeness**: any wait outside the engine (a `std::mutex` contended
   across a handoff, a `timerfd` read, a `sleep_for`) deadlocks the single runner.
   Symptom is a hang, diagnosis is "who holds the baton". Every such site found is
   one more route into vsched.
2. **Address space**: 4 + 8 GiB flat tables plus a 2 GiB asmjit arena plus the game
   in a single miniBox block. Page bookkeeping is 1 byte per page (24 GiB = 6M pages);
   savestate status arrays grow to match. Dirty tracking must stay sparse.

   Shrinking a region to save arena is not free, and one attempt cost a day
   (2026-09-12). `MemoryManager1` in JITLLVM.cpp returns `block + (pos % c_max_size)`:
   once its allocation pointer passes the end of the region the addresses WRAP, and a
   later section is loaded on top of one already there - so the next relocation writes
   into somebody else's code, silently. Patch 0020 had lowered that bound from
   upstream's 256 MiB to 64 MiB; Ultra Street Fighter IV's main module is 87.9 MB of
   generated code in 114 objects, up to 100 of which share one compiler
   (`c_modules_per_jit`), so a warm cache walked past 64 MiB and a 4-byte relocation
   landed inside a symbol resolver's `movabs` immediate. The byte after it became
   `add %bl,(%rdi)`: a store to the first byte of the execution table, which no page
   backs. Three of eight precompile sessions died with 0xC0000005 and no explanation.
   The bound is upstream's again and the wrap is now refused with a sentence, so a
   game bigger than the region says so instead of corrupting itself. Any future
   region that is sized to fit an arena needs the same treatment: refuse, never wrap.
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

## What CI runs, and what it does not (2026-09-20)

Chimera's `docs/gates.md` calls this failure mode G: *whatever CI does not run
is not gated, whatever the script says.* The rule it sets is that the gap is
kept as a table rather than discovered, and that **if CI cannot run a leg,
somebody owns running it, and the PLAN.md says who and when.** This section is
that record. Where it proposes rather than states, it says so.

Why the rule was needed here: until today `.github/workflows/chimera.yml` ran
`check-wbx.sh` and then `echo "SKIP emulation legs"` in place of
`waterbox/run-gate.sh`. The gate was run by nobody but a developer who
remembered - and that is the structural reason `waterbox/native.mk` could go
eight days without the native reference linking at all (it had never been
taught about `archive.cpp`, `sevenzip.cpp` and the 7-Zip decoder) with every
push green. The first leg in the table below catches that on the first push.

### The table

Twenty-five legs in `waterbox/run-gate.sh` (569 lines), six in
`waterbox/tests/run-frontend.sh` (316 lines). CI now runs six of the
thirty-one, in two jobs; before today it ran none.

The two jobs are `core-gate` (everything that is compiled, plus
`run-gate.sh`) and `frontend-gate` (`run-frontend.sh` against the package
`core-gate` produced, and no rpcs3 compiled at all). `publish` needs both, so
a red frontend gate now blocks the release.

| Leg | CI | Needs |
| --- | --- | --- |
| native:deterministic | RUNS | nothing (lv2test.elf is assembled from `tests/asm`) |
| sandbox:equivalent | RUNS | nothing |
| sandbox:rewind | RUNS | nothing |
| sandbox:rerecord | RUNS | nothing |
| firmware:lle | SKIP | PS3UPDAT.PUP |
| input:press | SKIP | the PUP |
| input:port2 | SKIP | the PUP |
| input:lag | SKIP | the PUP |
| video:flip | SKIP | the PUP |
| audio:tone | SKIP | the PUP |
| ppu:llvm | SKIP | the PUP |
| cache:objects | SKIP | the PUP |
| cache:warm | SKIP | the PUP |
| cache:precompile | SKIP | the PUP |
| spu:interpreter | SKIP | the PUP |
| spu:asmjit | SKIP | the PUP |
| spu:agree | SKIP | the PUP |
| disc:boot | SKIP | the PUP + a decrypted .iso |
| gpu:flip | SKIP | the PUP + a host EGL context |
| gpu:disc | SKIP | the PUP + a disc + an EGL context |
| gpu:rewind | SKIP | the PUP + a disc + an EGL context |
| gpu:context | SKIP | the PUP + a disc + an EGL context |
| pkg:boot | SKIP | the PUP + a digital-only game .pkg |
| pkg:content | SKIP | the PUP + a .pkg that is DLC or an update |
| pkg:licence | SKIP | the PUP + a REQUIRE_LICENSE .pkg and its .rap |
| game:frontend | RUNS | nothing |
| keybinds | RUNS | nothing |
| precompile:frontend | SKIP | the PUP |
| disc:frontend | SKIP | the PUP + a disc |
| gpu:frontend | SKIP | the PUP + a disc |
| gl:rebuild-at-zero | SKIP | the PUP + chimera-run + a built package (no disc) |
| project:frontend | SKIP | the PUP + a disc |

The test programs are not a blocker: `flip.elf`, `tone.elf`, `sputest.elf`,
`padtest.elf` and `padtest2.elf` are committed under `tests/roms`, and
`lv2test.elf` the gate assembles itself from `tests/asm` when it is absent.
Sony's firmware is the single condition on seventeen of the twenty-one skipped
core legs.

Every skipped leg now prints its own SKIP line, by name, and the script ends
with `gate: N passed, M failed, K skipped`. It used to fold four of them into
one line and leave seventeen entirely silent, which is how twenty-one missing
legs look like a handful. `run-frontend.sh` already did this; on a runner it
prints all six lines and ends `2 ok, 0 failed, 4 skipped`.

### Why - cost, and what it actually cost

The blocker on the four content-free legs was **cost**, not content: content
only starts at `firmware:lle`. The cost is that the gate compares the
sandboxed core against a NATIVE reference, and building that reference means a
second LLVM and a second ffmpeg for the host, and rpcs3's emulator library
compiled a second time.

Measured on the dev box (20 cores) from empty build directories at the
2026-09-20 pin. CPU seconds are the honest figure because they are what
transfers to a 4-vCPU runner; the last column is CPU/4 and is an ESTIMATE, and
an optimistic one, because a GitHub core is slower than one of these:

| Step | wall here | CPU s | est. 4-vCPU wall |
| --- | --- | --- | --- |
| ffmpeg, native (`-j8`) | 17 s | 88 | 22 s |
| LLVM native: configure + `llvm-tblgen` (`-j8`) - CI already paid this | 46 s | 317 | 1 min 20 s |
| LLVM native: the rest (`-j8`) | 11 min 36 s | 5314 | 22 min |
| rpcs3_emu + Fusion, native (`-j6`) | 4 min 55 s | 1747 | 7 min 17 s |
| the adapter and the 124 MB link, `native.mk` (`-j6`) | 16 s | 36 | 9 s |
| **new cost, cold cache** | | **7201 (2.0 CPU-hours)** | **about 30 min** |

And the gate itself, once the binaries exist - measured with the real
`run-native` and `core.wbx`, no firmware, `FRAMES=600`:

| Leg | wall |
| --- | --- |
| native:deterministic (two native runs) | 2.6 s |
| sandbox:equivalent | 14.3 s |
| sandbox:rewind | 11.4 s |
| sandbox:rerecord (200 frames) plus its plain comparison run | 38 s |
| **the whole runnable gate** | **about 67 s** |

So the gate is nearly free and the reference is not, which is why the answer
is caching rather than either brute force or giving up:

- **`build/llvm-native` was already a cache path**, and the job already built
  its `llvm-tblgen` because the guest cross-build needs it. Only the rest of
  that flavor was missing - 22 estimated runner-minutes, paid when the LLVM
  pin moves and not otherwise. The cache key gained a `v2`, and that is
  load-bearing: `actions/cache` re-saves nothing on an exact key hit, so a
  cache stored under the old key - holding a `build/llvm-native` with only the
  table generator in it - would be restored half-built for ever, the rest
  rebuilt on every push and stored never.
- **`build/native` is cached as a build TREE**, keyed on the rpcs3 pin, the
  LLVM pin, `patches/*.patch`, `configure-flags.sh` and `waterbox/*.h`, with
  `restore-keys` falling back to the nearest older tree. Unlike LLVM it moves
  with the patch series, and ninja is what knows which translation units
  actually need rebuilding. A push touching only `waterbox/*.cpp` rebuilds
  nothing in it: the adapter objects are `native.mk`'s, not CMake's.
- **ffmpeg is rebuilt every run** rather than cached. At 88 CPU seconds it is
  not worth a cache entry, and a stale ffmpeg header set is exactly what a
  cache key forgets.

So the whole thing lands at roughly **thirty estimated runner-minutes once,
on an LLVM pin bump**, and on every other push:

| Warm push | est. 4-vCPU |
| --- | --- |
| ffmpeg native (not cached, on purpose) | 22 s |
| both cmake configures, ninja with nothing to do | under 10 s |
| `native.mk` - only what changed, plus the 124 MB link | 9 s and up |
| the gate | 67 s |
| **added to every push** | **under 2 min** |

The remaining risk is GitHub's six-hour job cap, which `timeout-minutes`
cannot raise (the job asks for 360 because that is the ceiling). Thirty
minutes is not what will hit it - the guest halves already in the job are the
bulk - but the margin is thinner than it was. **If a cold run ever does hit
the cap, the fix is to split the native reference into its own job with its
own cache, not to stop building it.**

### What the frontend gate costs, measured

`frontend-gate` is a second job. It compiles no rpcs3: the package and
`run-wbx` are `core-gate`'s artifacts, and `core.wbx` is unzipped back out of
the package, so the bytes this job's two sides compare are the bytes that
ship. It does not build the native reference either - "native == sandbox" is
`core-gate`'s claim and this job's reference is the sandbox - so none of the
LLVM, ffmpeg or `native.mk` cost above appears here.

What it does pay for is Chimera. Those steps are not estimates: flycast's
`frontend gate (Chimera)` job already runs every one of them on a public
runner, so these are that job's own step timings (run 35511256013,
2026-09-20), with the rpcs3-specific steps measured on the dev box:

| Step | on a runner | note |
| --- | --- | --- |
| checkout, no submodules | ~5 s | `core-gate` pays 2 min 48 s for the recursive init; nothing here compiles rpcs3 |
| check out Chimera + its `extern` | 45 s | |
| apt (adds `mono-complete`, `xvfb`) | 45 s | |
| set up .NET | 6 s | |
| **Chimera's natives (meson)** | **7 min 9 s** | the bulk, and not cached - see below |
| `dotnet build Chimera.sln` | 47 s | |
| miniBox host library only | under 1 min 30 s | flycast's figure builds the C++ guest toolchain too, which this job does not |
| download the package + `run-wbx`, unzip `core.wbx` | ~30 s | about 47 MB |
| **`run-frontend.sh`** | **under 2 min** | 30 s wall / 32 CPU s here, single-threaded, so a 4-vCPU runner is about the same |
| **the whole job** | **about 12 min** | |

So: **about twelve runner-minutes added per push, and about twelve minutes of
extra wall clock**, because `needs: core-gate` serialises it behind the
package it consumes.

Three things were considered and rejected, so the next person does not
rediscover them:

- **Folding the legs into `core-gate` instead** would cost about three
  minutes rather than twelve - that job already builds Chimera's natives, sets
  up .NET and installs `mono-complete` for the contract tests. It was not done
  because `core-gate` is the job that sits against GitHub's six-hour cap (see
  above), and because a frontend failure should be attributable and re-runnable
  without repeating a multi-hour build.
- **Caching Chimera's meson tree** to save that seven minutes. A restored
  build tree sits beside a FRESH checkout whose sources all have newer
  timestamps, so ninja rebuilds it anyway; the cache buys a download in place
  of a build. flycast does not cache it either.
- **Sharing `core-gate`'s guest-toolchain cache.** The key is the same in both
  jobs, and a save from `frontend-gate` - which never builds
  `build/meson-cpp` - would be restored by `core-gate` for ever after, since
  `actions/cache` re-saves nothing on an exact key hit. That is the same trap
  the `v2` in the LLVM key exists to escape. `frontend-gate` therefore builds
  the miniBox host library from scratch and touches no cache at all.

`run-wbx` is passed as an artifact rather than rebuilt in the second job
because building it needs `waterbox/generated-gl`, which is the guest build's
output. Rebuilding it there would be CI doing something subtly different from
`build-core.sh` - gates.md failure mode G, which is the thing this whole
section exists to prevent. Its `RUNPATH` is an absolute path under
`GITHUB_WORKSPACE`, identical in both jobs, and the job runs `ldd` on it and
fails by name if the miniBox host library is not there - otherwise that
failure arrives disguised as "reference runner error" from inside the gate.

### Two traps found while doing this, both still live

- **`build-native.sh` picks whatever `c++` is, and g++ 14 cannot build it.**
  On this box the default is g++ 14.2, and it dies with an internal compiler
  error (SIGSEGV) in abseil's `any_invocable.h` on every translation unit that
  includes `NP/pb_helpers.h`. The tree in `build/native` here was configured
  with `g++-13`, which is why nobody had noticed. CI is fine today - Ubuntu
  24.04's default is 13.3 - but `ubuntu-latest` will move, and when it does
  this step goes red for a reason that has nothing to do with the core.
  Locally: `CC=gcc-13 CXX=g++-13 sh waterbox/build-native.sh`. **Decision for
  Sergio: pin the native reference's compiler, or leave it to follow the
  runner?** Pinning makes the gate stable; not pinning means the native
  reference is built by the same compiler as everything else, which is what
  `cache:objects` is about.
- **`native.mk` writes `g++` into its rules literally**, so `CXX=` on the
  command line does nothing and the adapter is always built by the default
  compiler - even when the CMake tree beside it was built by another one.

### What is still not gated, and why not

**The six legs CI runs are all on `lv2test.elf`**, which is a stand-in
(gates.md mode E). They prove the core starts, is deterministic, survives a
savestate round-trip, that the sandbox equals the host build ON THAT PROGRAM,
and that the shipped package run through the real frontend equals the sandbox
ON THAT PROGRAM. They do not stand in for a game: issue #120 is precisely a
native-vs-sandbox divergence that real titles show and these programs do not.
Green here is not a claim about games.

**`game:frontend` compares a 1 MiB window, not all of main memory.** The
frontend and the reference each write the first 1,048,576 bytes of the PS3's
256 MB of main memory and those are what `cmp` sees. The window is not inert -
lv2test's frame 100 and frame 200 differ inside it, at byte 689 - but it is
narrow, and here is the measured consequence: swapping the package for a
genuinely different build of this core (2026-09-12's, seven days and a miniBox
bump older) and running the leg against today's `core.wbx` as the reference
gave a **PASS**. Two builds that far apart happen to leave that window
identical on this program. So the leg does bite when the package is broken -
it went red at once when the package shipped a half-written `core.wbx` - but
it is not a fingerprint of the build, and it should not be read as one.
Widening it means changing what `frontend-ram.lua` writes out; nobody has
needed that yet.

### Who owns running the other twenty-five, and when (proposal for Sergio)

By hand, on the development machine that holds `tests/roms-local`, with the
output pasted into this file under a dated heading - one line per leg, plus
which disc and which packages. "When did this last actually execute?" should
be a question with an answer.

```
FRAMES=600 waterbox/run-gate.sh
waterbox/tests/run-frontend.sh --chimera-root ~/chimera
```

Four occasions, chosen because they are when this repo actually moves:

1. **Before an `extern/rpcs3` pin bump lands.** A new upstream is the change
   most likely to move the machine, and the firmware legs are the only thing
   that would notice.
2. **Before a release is cut** - a dated `nightly-*`, or any tag a movie could
   cite. A movie cites a package by hash; a package nobody ran the gate
   against is a citation with nothing behind it.
3. **After any change to vsched, the savestate format, the GPU bridge or the
   compile cache** - the four subsystems whose faults only the firmware legs
   catch.
4. **Whenever a `patches/` number is added or renumbered.** The patch series
   is what makes this tree different from upstream, and nothing else tests it
   end to end.

Not "every push": the firmware and the discs are one machine's, and a rule
nobody can keep is worse than a rule that names its four occasions.

## The bridge had no case for its own context id (2026-09-21)

`waterbox/gl-host.c` is the dispatcher the SANDBOX harness hands a guest, and
it had no case for `GL_OP_CONTEXT_ID` - the opcode miniBox added for chimera
issue #43 - for as long as the opcode has existed. The default arm logged
`opcode 4 has no case` and returned 0, and 0 is the contract's "cannot tell":
`GLGSRender::chimera_check_gl_context` reads it, concludes nothing moved, and
keeps the GL object names it already had. Which is issue #43 exactly, in the
one harness that exists to prove #43 fixed.

**What it cost, measured, not reasoned.** The rest of the command stream was
never at risk: one sandbox `gpu:flip` run (60 frames, llvmpipe) printed the line
365 times and its six frame lines are byte-identical to the same run with the
case present. So the cost was the answer alone - the renderer was told "cannot
tell" 365 times a minute and so could never rebuild. Natively the cost is nil
and always was: `chimera_rpcs3_install_gpu_bridge` deliberately does NOT call
`chimera_gl_install` in the single-binary build (the dispatcher would recurse
into the wrappers), so `chimera_gl_context_id` sees a null bridge and returns 0
without ever reaching the dispatcher - which is why the native run printed the
line zero times. `run-native` has no `--state` either, so nothing native can
meet a context that moved.

**The case, and a load hook to go with it.** `mint_context_id` is the engine's
(`~/chimera/source/engine/source/gl_bridge.cpp`), for its reasons: the pid and a
high-resolution counter carry the per-process entropy, because an address is not
per-process on Windows and `time()` at one-second granularity would hand two
runs started in the same second the SAME id. And because chimera's host mints
again on every state load (`ce_gl_state_loaded`; rpcs3 declares no
`video.rebuildOnStateLoad`, so it rebuilds), `run-wbx` now calls
`chimera_gl_host_state_loaded` at all three of its load sites. Without that a
load in this runner was a strictly EASIER test than a load in Chimera, and the
gate stood behind the easier one.

**The gate could not see any of it, and now can.** A leg that passes while the
log says "no case for this opcode" 365 times is not a leg (gates.md mode C:
absent is indistinguishable from working). So: the default arm counts as well
as logs, caps its own chatter at eight lines - 365 a run is how this stayed
invisible - `chimera_gl_host_unhandled` hands the count to the runners, they
print it at the end, and `bridge_answered` in `run-gate.sh` fails `gpu:flip`,
`gpu:disc`, `gpu:rewind` and `gpu:context` when either flavour's stderr carries
that line.

Proved by breaking it. With the case label changed to a number nothing sends
and run-wbx rebuilt from that, the gate said `FAIL: gpu:flip - the GPU bridge
had no case for opcode 4 and answered 0 (gflip-wbx.err)` - the same leg, on the
same program, that had been green for as long as the opcode existed. The
control was run through the real `run-gate.sh` on a tree whose
`tests/roms-local` held only the firmware (with `bin`, `obj-native` and
`tests/roms` symlinked in), so the disc and package legs skipped and it reached
`gpu:flip` in forty minutes rather than taking the machine for an hour and a
half: 17 passed, 1 failed, 8 skipped, the one failure being the leg under test.
With the case back it passes.

**gpu:disc, gpu:rewind and gpu:context still fail, for a different reason, NOT
fixed here.** The disc the gate picks is whichever `tests/roms-local/*.iso` has
no key beside it, and today that is `bejeweled3.iso`. In the sandbox on the GL
renderer that disc never completes frame 1: it sits at 4.8 GiB for about 95
seconds, then faults guest pages into the miniBox block at roughly 370 MiB/s
until it is OOM-killed - 30 GiB with no frame line printed, in a run asked for
one frame. The three controls say where it is not:

| run | result |
|---|---|
| bejeweled3, GL, native | 20 frames in ~3 s, 1.7 GiB |
| bejeweled3, null renderer, sandbox | 20 frames in seconds, under 16 GiB |
| bejeweled3, GL, sandbox | frame 1 never returns, killed at 30 GiB |
| re5gold, GL, sandbox | frame 15 reached, killed at a 14 GiB cap |

So it is not the bridge, not the opcode, not the RSX patches landed on
2026-09-20 (established that day by somebody else: 0034 and 0035 removed from
the series and the core rebuilt, still no frame), and not every game - it is this disc through the sandbox on GL, and
it is open. An `RSX` trace ends at `cellVideoOutConfigure` and the two
`setDisplayBuffer` calls, with nothing after it, so the FIFO is where to look
next. `gpu:disc` used to be proved on GTA San Andreas (2026-09-02, 1800 frames
at 47 fps), which is a disc this machine no longer holds - the leg's subject
changed when the folder did, and nothing said so.

**The same gap is in three sibling cores.** PCSX2's, Dolphin's and xemu's
`waterbox/gl-host.c` have no case for `GL_OP_CONTEXT_ID` either, and all three
have guests that ask for it (pcsx2's `waterbox/cinterface.cpp`, dolphin's
`OGLGfx.cpp`, xemu's `pgraph/gl/renderer.c`), so their sandbox harnesses are
telling those renderers "cannot tell" exactly as this one was. Flycast has no
`gl-host.c` at all. Not touched from here - it is their repositories - but
recorded so nobody has to find it a fourth time.

**And a gate run cannot survive it.** The full gate on 2026-09-21 reported 25
legs PASS, 0 FAIL, 0 SKIP up to and including `gpu:flip`, and then stopped
without a summary: the cgroup OOM kill that ends the `gpu:disc` sandbox run
takes the gate's own shell with it (`systemd-run --scope -p MemoryMax=26G`,
killed at 09:32). Uncapped it would take the machine instead. So today
`gpu:disc`, `gpu:rewind` and `gpu:context` are not legs that fail; they are
legs that end the run, and the summary line - the one thing that says how many
legs were skipped - never prints. Nothing in the script can bound a child's
memory portably (`ulimit -v` is meaningless for a waterbox, which reserves tens
of gigabytes of address space by design), so this is recorded rather than
fixed, and it is the second reason the disc problem above wants solving.

`gpu_ran` is the small part of that which IS fixed: an empty frame file
compared against a full one used to read as "the sandbox's GL run differs from
the native GL run", which sends the next reader hunting a divergence instead of
a corpse. The leg now says the run produced no frame at all, and prints the
last line of its stderr.

## A stored context id of zero is not a promise that nothing moved (2026-09-21)

Chimera issue #126, reported against PCSX2 on Maximo: Ghosts to Glory, fixed
there in 323e916. The code it lived in was copied between every bridged core,
so this one was checked for it. **It had it.**

`GLGSRender::chimera_check_gl_context` rebuilds the renderer's GL objects when
the context id stored beside them differs from the one the calls are landing
on. `m_chimera_gl_context` starts at 0 and is first written at the bottom of
that function, which the RSX thread first reaches in `do_local_task` - AFTER
`on_init_thread` has already run `chimera_gl_setup` and built every object the
renderer holds. So there is a window in every session in which the objects
exist and the stored id is still 0, and a state taken inside it carries the 0.
The guard `if (m_chimera_gl_context != 0)` read that as "this machine has never
held any GL objects". It does not mean that. It means "this state was taken
before the renderer looked, so it cannot vouch for what the driver is holding
now" - and what the driver is holding is whatever the frames after the snapshot
left there.

The greenzone's frame-0 anchor is such a state (the engine captures it right
after Init, `session.cpp`), and it is the one TAStudio loads when a movie is
replayed from the beginning, because it reaches a frame by loading the state
BEFORE it and emulating one frame forward. Frames 0 and 1 both load the anchor;
frame 2 is the first that does not. That is the fingerprint the reporter
described.

**Measured on flip.elf through chimera-run**, `--gpu --greenzone 4096
--rewind-loop N,1` under `CHIMERA_GL_TRACE=1 CHIMERA_GL_STATEAUDIT=1`, counting
the bridge crossings on the frame after the restore. Two packages from this
tree differing only in the fix:

| restore to | rebuild left alone | rebuild off (`CHIMERA_GL_KEEP_OBJECTS_ON_LOAD=1`) |
|---|---|---|
| frame 0, before | 3549 | 3549 |
| frame 0, after | **8752** | 8752 |
| frame 2, before | 5258 | 53 |
| frame 2, after | 5258 | 53 |

Read it this way: before the fix, restoring the anchor cost the same whether
the rebuild was allowed or not - 3549 calls, all of them flip.elf's own gcm
setup being replayed - so no rebuild happened. After the fix it costs 8752,
which is that same 3549 plus 5203, and 5203 is exactly what the rebuild costs
at frame 2 (5258 - 55). Frame 2 is untouched by the change, as it should be: it
always rebuilt.

**The fix is PCSX2's, so that every bridged core ends up one shape.** The
engine already tells every core when the machine's memory has been replaced;
this core now implements that optional export (`StateLoaded` in wbx-entry.cpp),
which sets a flag in `gl-shim.cpp` that the renderer reads and clears. The flag
is set AFTER the load, so the load cannot wipe it, and a fresh boot has had no
load and still does not rebuild. `run-wbx` calls the export at all three of its
load sites too, so the runner's loads are not quieter than the frontend's.

The two alternatives PCSX2 rejected were not re-litigated: recording the id
during `Init` puts it in the SEALED baseline where no state carries it as a
delta, which breaks the cross-session rebuild of issue #43; and a non-zero
"never seen" sentinel fails identically, because the anchor carries whatever
the initial value is.

**The leg is `gl:rebuild-at-zero`** (waterbox/tests/run-frontend.sh, which is
where the package and the chimera checkout are already to hand). It needs the
firmware and flip.elf and no disc, and it is placed AHEAD of the three disc
legs for that reason: those are the slowest and hungriest things in the script,
they exhausted a 30 GiB cap twice on this machine, and a run that dies at leg 3
used to take this one with it.

It was watched failing. Against the package built before the fix:
`FAIL: gl:rebuild-at-zero - restoring the frame-0 anchor cost 3549 calls
against 5258 restoring frame 2: the anchor's stored context id of zero was read
as nothing to rebuild (chimera issue 126)`. Against the fixed package: `PASS -
the anchor costs 8752 calls, frame 2 5258, and 53 with the rebuild turned off`.

**PCSX2's leg would have passed on the broken core here**, which is worth
recording as its own lesson. That leg asserts the frame after a restore crosses
the bridge thousands of times, because an idle frame of its test program
crosses it once; restoring rpcs3 to frame 0 crosses it 3549 times on a broken
core, from the program's own replayed setup. And the obvious A/B - the same
restore under `CHIMERA_GL_KEEP_OBJECTS_ON_LOAD` - is not a control at frame 0
either, because that switch only stops the engine MINTING a new id, and a
stored 0 differs from the live id however little it moved; it reads 8752
against 8752 on the FIXED core. A borrowed threshold is not a borrowed test.
So the leg asserts what was actually false before and true after: that frame
2's crossings really are a rebuild (the A/B is valid there - 5258 against 53),
and that restoring the anchor costs at least as much as restoring frame 2,
which it cannot if the anchor skipped the rebuild.

**What this does NOT establish.** The corrupt picture was never reproduced
here; nothing was run on the reporter's hardware and no NVIDIA driver has been
near it. What is established is that the rpcs3 core had the same defect PCSX2
had, by the same measurement, and that the same fix removes it.
