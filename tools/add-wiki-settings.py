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

MOVIE = " It is part of the machine, so a movie needs the same value."
HOST = (" It changes what your graphics card draws, not what the console "
    "calculates. Without a renderer on a graphics card it does nothing.")
PACE = (" This core fixes it so that a movie gives the same result every time, "
    "and the default is that fixed value. Chimera decides when each frame "
    "runs, so changing it is an experiment, and a movie needs the same "
    "value.")

SETTINGS = [
    ("zcullAccuracy", "ZCULL accuracy", "enum", ["Precise", "Approximate", "Relaxed"], "Precise",
     "How exactly the counts of visible pixels (the ZCULL statistics of the "
     "RSX, the PS3's graphics chip) are reported back to the game. 'Precise'"
     " counts every sample and waits for the graphics card. 'Approximate' "
     "and 'Relaxed' answer sooner with a less exact count. Some games need "
     "that to run at all, and some draw wrongly with it." + MOVIE),
    ("resolutionScaleThreshold", "Resolution scale threshold", "int", (1, 1024), 16,
     "Render targets (the surfaces a game draws into) smaller than this many"
     " pixels on each side are never scaled by 'Resolution scale'. The RPCS3"
     " wiki writes the value in the form '320x320'. Here it is just the one "
     "number." + HOST),
    ("frameLimit", "Framelimit", "enum",
     ["Off", "30", "50", "60", "120", "Display", "Auto", "PS3 Native", "Infinite"], "PS3 Native",
     "How RPCS3 times flips, the moments a finished picture is shown. 'PS3 Native' holds a flip that "
     "waits for vertical sync until the console's next vertical blank, as the hardware does. A rate "
     "(60, Auto and so on) holds a flip that comes early until its turn in the same way, and the RSX "
     "(the graphics chip) keeps reading commands in the meantime. RPCS3 itself pauses the RSX thread "
     "there. In this core that pause is the machine's own time, and it stopped Mortal Kombat "
     "(chimera#125) and made Rayman Legends run out of memory (chimera#165). 'Off' and 'Infinite' "
     "flip immediately." + MOVIE),
    ("sleepTimersAccuracy", "Sleep timers accuracy", "enum", ["As Host", "Usleep Only", "All Timers"], "As Host",
     "How precisely the console's sleep and timer calls are followed. The "
     "finer choices wake threads exactly on time and are slower." + MOVIE),
    ("vblankRate", "Vblank rate", "int", (1, 6000), 60,
     "How many vertical blank (vblank) events the console sees each second. "
     "Games that time themselves by vblank run faster at a higher rate, and "
     "some run correctly only at 120." + PACE),
    ("vblankNtscFixup", "VBlank NTSC Fixup", "bool", None, False,
     "Makes the vblank rate 59.94 instead of 60, as on an NTSC console." + PACE),
    ("multithreadedRsx", "Multithreaded RSX", "bool", None, False,
     "Moves part of the work of the RSX (the graphics chip) to another "
     "thread. That thread is scheduled like every other thread in the "
     "machine, and the order in which the two finish can differ." + PACE),
    ("disableVertexCache", "Disable vertex cache", "bool", None, False,
     "Stops RPCS3 from reusing vertex data it has already sent to the "
     "graphics card. It is for games that change that data in place."
     + MOVIE),
    ("strictRenderingMode", "Strict rendering mode", "bool", None, False,
     "Follows the rules of the RSX (the graphics chip) exactly where RPCS3 "
     "normally takes faster shortcuts. It is slower, and it is correct for "
     "games that the shortcuts break." + MOVIE),
    ("firmwareLibraries", "Firmware libraries", "string", None, "",
     "System libraries to run from the PS3 system software (LLE) instead of "
     "RPCS3's own versions (HLE), separated by commas, for example "
     "'libvdec.sprx'. The RPCS3 wiki names a library when RPCS3's version of"
     " it is not good enough for a game. libvdec.sprx is the video decoder. "
     "This needs the system software. Without it there is nothing to run the"
     " libraries from, and the setting is ignored." + MOVIE),
    ("accurateRsxReservationAccess", "Accurate RSX reservation access", "bool", None, False,
     "Makes the RSX (the graphics chip) respect the Cell processor's memory "
     "reservations exactly when it reads and writes shared memory." + MOVIE),
    ("spuBlockSize", "SPU block size", "enum", ["Safe", "Mega", "Giga"], "Safe",
     "How much SPU code the recompiler compiles as one unit. Larger blocks "
     "are faster. They also change the points where the recompiler counts "
     "time (once per function and loop), so the machine's clock runs "
     "slightly differently."
     + MOVIE),
    ("spuXfloatAccuracy", "SPU xfloat accuracy", "enum", ["Accurate", "Approximate", "Relaxed", "Inaccurate"],
     "Approximate",
     "How exactly the SPU processors' non-standard floating point is "
     "reproduced. Choose 'Accurate' for games whose physics or animation go "
     "wrong." + MOVIE),
    ("handleRsxMemoryTiling", "Handle RSX memory tiling", "bool", None, False,
     "Emulates the RSX's tiled memory layout when the Cell processor reads "
     "or writes a tiled region. It is for games that draw from or into such "
     "a region directly." + MOVIE),
    ("forceCpuBlit", "Force CPU blit emulation", "bool", None, False,
     "Does the RSX's image copies on the processor, into the console's "
     "memory, instead of on the graphics card. It is for games that read the"
     " result back." + MOVIE),
    ("antiAliasing", "Anti-aliasing", "enum", ["Disabled", "Auto"], "Auto",
     "Whether the render targets a game asks to be multisampled really are. "
     "'Disabled' draws them without multisampling, which some games need to "
     "show anything at all." + HOST),
    ("writeDepthBuffer", "Write depth buffer", "bool", None, False,
     "Writes finished depth buffers back into the console's memory, where a "
     "game that reads them expects them. It is the depth version of 'Write "
     "color buffers to memory'." + MOVIE),
    ("readDepthBuffer", "Read depth buffer", "bool", None, False,
     "Before drawing into a depth buffer, loads what the console's memory "
     "already holds for it."
     + MOVIE),
    ("resolutionScale", "Resolution scale", "int", (25, 800), 100,
     "Draws at this percentage of the game's own resolution. The picture "
     "Chimera receives changes size with it." + HOST),
    ("driverWakeUpDelay", "Driver wake-up delay", "int", (0, 16667), 0,
     "How many microseconds the RSX thread waits before it wakes up to "
     "process new commands. It is for games that depend on the exact timing "
     "of their own command stream." + MOVIE),
    ("shaderQuality", "Shader quality", "enum", ["Auto", "Low", "High", "Ultra"], "High",
     "The floating point precision used by the shaders on your graphics "
     "card." + HOST),
    ("allowHostGpuLabels", "Allow Host GPU Labels", "bool", None, False,
     "Lets your graphics card signal the RSX's semaphores directly, instead "
     "of through RPCS3's own synchronisation." + MOVIE),
    ("disableZcullQueries", "Disable ZCull occlusion queries", "bool", None, False,
     "Answers every occlusion query (the question whether something is "
     "visible) with yes, without asking the graphics card." + MOVIE),
    ("emulateSpecialDepthComparison", "Emulate Special Depth Comparison", "bool", None, False,
     "Reproduces the RSX's depth comparison where it differs from the "
     "graphics card's. It is for shadows and decals that flicker or "
     "disappear." + HOST),
    ("shaderMode", "Shader mode", "enum",
     ["Legacy Recompiler (single-threaded)", "Async Recompiler (multi-threaded)",
      "Async Recompiler with Shader Interpreter", "Shader Interpreter only"],
     "Legacy Recompiler (single-threaded)",
     "How the shaders for your graphics card are made. This core compiles "
     "each shader on the RSX thread before drawing with it. The asynchronous"
     " modes draw without a shader until one is ready, so what is drawn "
     "depends on how fast your computer compiles." + HOST + PACE),
    ("preferredSpuThreads", "Preferred SPU threads", "int", (0, 6), 0,
     "How many SPU threads may run heavy work at the same time. 0 leaves it "
     "to RPCS3." + MOVIE),
    ("accurateSpuDma", "Accurate SPU DMA", "bool", None, False,
     "Makes every SPU memory transfer (DMA) exactly as indivisible as it is "
     "on the hardware." + MOVIE),
    ("defaultResolution", "Default resolution", "enum",
     ["1920x1080", "1920x1080i", "1280x720", "720x480", "720x480i", "720x576", "720x576i",
      "1600x1080", "1440x1080", "1280x1080", "960x1080"], "1280x720",
     "The video mode the console reports to the game as the mode of its "
     "display. A game chooses its own rendering resolution from it, so it "
     "changes what the game does and not only what is shown." + MOVIE),
    ("maxSpursThreads", "Maximum SPURS threads", "int", (1, 6), 6,
     "The largest number of SPU threads a SPURS task group may run at once." + MOVIE),
    ("rsxFifoAccuracy", "RSX FIFO accuracy", "enum", ["Fast", "Atomic", "Ordered & Atomic", "PS3"], "Atomic",
     "How the RSX reads the command stream that the Cell processor writes. "
     "The stricter modes see each command exactly as and when the console's "
     "chip would." + MOVIE),
    ("delayOddMfcCommands", "Delay each odd MFC Command", "bool", None, False,
     "Delays every second SPU memory transfer (DMA) command so that the "
     "commands finish out of order, which some games depend on. RPCS3 "
     "chooses the order from the processor's time-stamp counter." + MOVIE),
    ("disableSpuGetllarSpinOptimization", "Disable SPU GETLLAR Spin Optimization", "bool", None, False,
     "Stops RPCS3 from taking a shortcut for SPUs that wait in a loop on a "
     "memory reservation." + MOVIE),
    ("debugConsoleMode", "Debug console mode", "bool", None, False,
     "Makes the console a debug unit, with the larger memory that model has,"
     " for games that need it." + MOVIE),
    ("accuratePpu128Reservations", "Accurate PPU 128 reservations", "int", (-1, 14), 0,
     "How long a loop on a 128-byte PPU memory reservation may be and still "
     "be made exact. 0 means never and -1 means always (the wiki's 'Always "
     "Enabled')." + MOVIE),
    ("ppuThreads", "PPU thread count", "int", (1, 8), 2,
     "How many PPU threads may run at once. The console has two hardware "
     "threads." + PACE),
    ("lodBiasOffset", "LOD bias offset", "float", (-32, 32), 0.0,
     "A number added to every texture's level-of-detail bias. Negative is "
     "sharper and positive is blurrier." + HOST),
    ("spuLoopDetection", "Enable SPU loop detection", "bool", None, False,
     "Detects SPUs that wait in a loop and lets another thread run." + PACE),
    ("anisotropicFilter", "Anisotropic filter", "enum", ["Auto", "2x", "4x", "8x", "16x"], "Auto",
     "Overrides the game's anisotropic filtering. 'Auto' keeps the game's "
     "own." + HOST),
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
