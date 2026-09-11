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

- **Open: Bejeweled 3 boots but stops at its launcher.** The TTY says "PopCap
  Launcher App" and then nothing: main memory does not change over 600 frames,
  every frame is a lag frame, and the machine is idle rather than spinning. The
  disc carries one EBOOT and several games' assets, so this is a compilation
  launcher waiting for something it is not getting. Identical under the SPU
  interpreter, so it is not the recompiler.

- **Open: a sandboxed machine's log cannot be read.** `CHIMERA_LOG_TRACE` and
  `CHIMERA_SPU_TRACE` are `getenv`, and a sandboxed guest is handed no
  environment at all, so both do nothing in the box - they only work in
  `run-native`. The log file itself lives in the memory filesystem, which
  nothing outside can read. Chasing the launcher above needs one of the two
  fixed: an export that takes the channel list, or a way to pull
  /cache/RPCS3.log out of memfs (`--log-out`, beside `--tty-out`).

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
