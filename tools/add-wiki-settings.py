#!/usr/bin/env python3
"""One-off: declares, in waterbox/waterbox.config, every setting the RPCS3 wiki
recommends per game that this core can run with. Kept so the declarations can
be re-derived; the config is the record.

Each display name is the wiki's own name for the setting, so a recommendation
on a wiki page and the row in the settings grid read the same. The options of
an enum are rpcs3's own spellings (the wiki's too), which the driver hands to
rpcs3's own parser. Every default is what the core ran with before the setting
existed, so no existing project's machine moves.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, "..", "waterbox", "waterbox.config")

MOVIE = " It is part of the machine: a movie made with it replays only with it."
HOST = (" It changes what the host GPU draws, not what the console computes; with no"
        " GPU renderer it does nothing.")
PACE = (" This core pins it for determinism, and the default is that pin: the frontend"
        " drives the frames, so a change here is an experiment, and one a movie must keep.")

SETTINGS = [
    ("zcullAccuracy", "ZCULL accuracy", "enum", ["Precise", "Approximate", "Relaxed"], "Precise",
     "How exactly the RSX's occlusion counts (ZCULL statistics) are reported back to the game. 'Precise' "
     "counts every sample and waits for the GPU; 'Approximate' and 'Relaxed' answer sooner with a less "
     "exact count, which some games need to run at all and some misdraw with." + MOVIE),
    ("resolutionScaleThreshold", "Resolution scale threshold", "int", (1, 1024), 16,
     "Render targets smaller than this many pixels on each side are never scaled by 'Resolution scale'. "
     "The wiki writes it as '320x320'; here it is the one number." + HOST),
    ("frameLimit", "Framelimit", "enum",
     ["Off", "30", "50", "60", "120", "Display", "Auto", "PS3 Native", "Infinite"], "PS3 Native",
     "How rpcs3 paces flips. 'PS3 Native' holds a vsync flip until the console's next vblank, as the "
     "hardware does; the others make the RSX thread sleep toward a rate, and in this core that sleep is "
     "the machine's own time, which starved Mortal Kombat's command stream (chimera#125)." + PACE),
    ("sleepTimersAccuracy", "Sleep timers accuracy", "enum", ["As Host", "Usleep Only", "All Timers"], "As Host",
     "How precisely the console's sleep and timer calls are honoured. The finer settings wake threads "
     "exactly on time at a cost in speed." + MOVIE),
    ("vblankRate", "Vblank rate", "int", (1, 6000), 60,
     "How many vblank events a second the console sees. Games that pace themselves by vblank run faster "
     "at a higher rate, and some run correctly only at 120." + PACE),
    ("vblankNtscFixup", "VBlank NTSC Fixup", "bool", None, False,
     "Makes the vblank rate 59.94 instead of 60, as an NTSC console has it." + PACE),
    ("multithreadedRsx", "Multithreaded RSX", "bool", None, False,
     "Offloads part of the RSX's work to another thread. That thread is scheduled like every other in "
     "the machine, and the order in which the two finish can differ." + PACE),
    ("disableVertexCache", "Disable vertex cache", "bool", None, False,
     "Stops rpcs3 from reusing vertex data it already uploaded, for games that change it in place."
     + MOVIE),
    ("strictRenderingMode", "Strict rendering mode", "bool", None, False,
     "Follows the RSX's rules to the letter where rpcs3 normally takes faster shortcuts: slower, and "
     "correct for games the shortcuts break." + MOVIE),
    ("firmwareLibraries", "Firmware libraries", "string", None, "",
     "System libraries to run from the firmware (LLE) instead of rpcs3's own implementation (HLE), "
     "separated by commas, e.g. 'libvdec.sprx'. The wiki names one when rpcs3's version of it is not "
     "good enough for a game - libvdec.sprx is the video decoder. Needs the system software; without "
     "it there is nothing to run them from and this is ignored." + MOVIE),
    ("accurateRsxReservationAccess", "Accurate RSX reservation access", "bool", None, False,
     "Makes the RSX honour the Cell's memory reservations exactly when it reads and writes shared "
     "memory." + MOVIE),
    ("spuBlockSize", "SPU block size", "enum", ["Safe", "Mega", "Giga"], "Safe",
     "How much SPU code the recompiler compiles as one unit. Larger blocks are faster and change where "
     "the recompiler charges the clock (per function and loop), so the machine keeps different time."
     + MOVIE),
    ("spuXfloatAccuracy", "SPU xfloat accuracy", "enum", ["Accurate", "Approximate", "Relaxed", "Inaccurate"],
     "Approximate",
     "How exactly the SPUs' non-IEEE floating point is reproduced. Games whose physics or animation go "
     "wrong want 'Accurate'." + MOVIE),
    ("handleRsxMemoryTiling", "Handle RSX memory tiling", "bool", None, False,
     "Emulates the RSX's tiled memory layout when the Cell reads or writes a tiled region, for games "
     "that draw from or into one directly." + MOVIE),
    ("forceCpuBlit", "Force CPU blit emulation", "bool", None, False,
     "Performs the RSX's image copies on the CPU into the console's memory instead of on the GPU, for "
     "games that read the result back." + MOVIE),
    ("antiAliasing", "Anti-aliasing", "enum", ["Disabled", "Auto"], "Auto",
     "Whether render targets the game asks to be multisampled are. 'Disabled' draws them without, which "
     "some games need to show anything at all." + HOST),
    ("writeDepthBuffer", "Write depth buffer", "bool", None, False,
     "Writes finished depth buffers back into the console's memory, where a game that reads them expects "
     "them. The depth counterpart of 'Write color buffers to memory'." + MOVIE),
    ("readDepthBuffer", "Read depth buffer", "bool", None, False,
     "Loads a depth buffer's existing contents out of the console's memory before drawing into it."
     + MOVIE),
    ("resolutionScale", "Resolution scale", "int", (25, 800), 100,
     "Draws at this percentage of the game's own resolution. The picture the frontend receives changes "
     "size with it." + HOST),
    ("driverWakeUpDelay", "Driver wake-up delay", "int", (0, 16667), 0,
     "Microseconds the RSX thread waits before it wakes to process new commands, for games that race "
     "their own command stream." + MOVIE),
    ("shaderQuality", "Shader quality", "enum", ["Auto", "Low", "High", "Ultra"], "High",
     "The floating point precision the host's shaders use." + HOST),
    ("allowHostGpuLabels", "Allow Host GPU Labels", "bool", None, False,
     "Lets the host GPU signal the RSX's semaphores directly instead of through rpcs3's own "
     "synchronisation." + MOVIE),
    ("disableZcullQueries", "Disable ZCull occlusion queries", "bool", None, False,
     "Answers every occlusion query as visible without asking the GPU." + MOVIE),
    ("emulateSpecialDepthComparison", "Emulate Special Depth Comparison", "bool", None, False,
     "Reproduces the RSX's depth comparison where it differs from the host GPU's, for shadows and "
     "decals that flicker or vanish." + HOST),
    ("shaderMode", "Shader mode", "enum",
     ["Legacy Recompiler (single-threaded)", "Async Recompiler (multi-threaded)",
      "Async Recompiler with Shader Interpreter", "Shader Interpreter only"],
     "Legacy Recompiler (single-threaded)",
     "How the host's shaders are made. This core compiles each one on the RSX thread before it draws "
     "with it; the asynchronous modes draw without a shader until one is ready, so what is drawn "
     "depends on how fast the host compiles." + HOST + PACE),
    ("preferredSpuThreads", "Preferred SPU threads", "int", (0, 6), 0,
     "How many SPU threads may run heavy work at the same time; 0 leaves it to rpcs3." + MOVIE),
    ("accurateSpuDma", "Accurate SPU DMA", "bool", None, False,
     "Makes every SPU DMA transfer exactly as atomic as the hardware's." + MOVIE),
    ("defaultResolution", "Default resolution", "enum",
     ["1920x1080", "1920x1080i", "1280x720", "720x480", "720x480i", "720x576", "720x576i",
      "1600x1080", "1440x1080", "1280x1080", "960x1080"], "1280x720",
     "The video mode the console reports to the game as its display's. A game picks its own rendering "
     "resolution from it, so it changes what the game does, not only what is shown." + MOVIE),
    ("maxSpursThreads", "Maximum SPURS threads", "int", (1, 6), 6,
     "The most SPU threads a SPURS task group may run at once." + MOVIE),
    ("rsxFifoAccuracy", "RSX FIFO accuracy", "enum", ["Fast", "Atomic", "Ordered & Atomic", "PS3"], "Atomic",
     "How the RSX reads the command stream the Cell writes. The stricter modes see each command exactly "
     "as and when the console's would." + MOVIE),
    ("delayOddMfcCommands", "Delay each odd MFC Command", "bool", None, False,
     "Holds back every other SPU DMA command so that they complete out of order, which some games "
     "depend on. rpcs3 picks the order from the CPU's time-stamp counter." + MOVIE),
    ("disableSpuGetllarSpinOptimization", "Disable SPU GETLLAR Spin Optimization", "bool", None, False,
     "Stops rpcs3 from shortcutting SPUs that spin on a reservation." + MOVIE),
    ("debugConsoleMode", "Debug console mode", "bool", None, False,
     "Makes the console a debug unit, with that model's larger memory, for games that need it." + MOVIE),
    ("accuratePpu128Reservations", "Accurate PPU 128 reservations", "int", (-1, 14), 0,
     "How long a 128-byte PPU reservation loop may be and still be made exact: 0 never, -1 always (the "
     "wiki's 'Always Enabled')." + MOVIE),
    ("ppuThreads", "PPU thread count", "int", (1, 8), 2,
     "How many PPU threads may run at once. The console has two hardware threads." + PACE),
    ("lodBiasOffset", "LOD bias offset", "float", (-32, 32), 0.0,
     "Added to every texture's level-of-detail bias: negative is sharper, positive blurrier." + HOST),
    ("spuLoopDetection", "Enable SPU loop detection", "bool", None, False,
     "Detects SPUs waiting in a loop and yields their thread." + PACE),
    ("anisotropicFilter", "Anisotropic filter", "enum", ["Auto", "2x", "4x", "8x", "16x"], "Auto",
     "Overrides the game's anisotropic filtering. 'Auto' keeps the game's own." + HOST),
]


def main():
    with open(CONFIG, encoding="utf-8") as f:
        cfg = json.load(f)
    have = {s["name"] for s in cfg["settings"]}
    at = next(i for i, s in enumerate(cfg["settings"]) if s["name"] == "readColorBuffers") + 1
    added = []
    for name, display, kind, extra, default, text in SETTINGS:
        if name in have:
            continue
        s = {"name": name, "display": display, "type": kind}
        if kind == "enum":
            s["options"] = extra
        elif kind in ("int", "float") and extra:
            s["min"], s["max"] = extra
        s["default"] = default
        s["description"] = text
        added.append(s)
    cfg["settings"][at:at] = added
    with open(CONFIG, "w", encoding="utf-8") as f:
        f.write(json.dumps(cfg, indent=2) + "\n")
    print(f"{len(added)} settings declared")


if __name__ == "__main__":
    main()
