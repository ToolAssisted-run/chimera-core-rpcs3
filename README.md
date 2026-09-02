# chimera-core-rpcs3

The Sony PlayStation 3 as a
[Chimera](https://github.com/ToolAssisted-run/chimera) waterbox core, built from
[RPCS3](https://rpcs3.net). The machine runs deterministically inside the miniBox
sandbox: a virtual-time scheduler owns every one of the emulator's threads,
savestates are arena snapshots, and the picture comes from the bridged OpenGL
renderer while the machine's memory never depends on the GPU.

- `extern/rpcs3` - upstream, pinned, unmodified
- `patches/` - the local patch series (numbered, applied by `apply-patches.sh`;
  each a build option or a hook, never a deletion)
- `waterbox/` - the adapter, the scheduler, the build and the gate
- `docs/PLAN.md` - milestones and the decisions behind them

Upstream is GPL-2.0-only; the glue in this repository is MIT. Firmware
(`PS3UPDAT.PUP`, from Sony) and discs are supplied by the user and live outside
the repository; nothing under `tests/roms-local/` (gitignored) is ever committed.
