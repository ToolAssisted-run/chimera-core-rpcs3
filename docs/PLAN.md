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
