#!/usr/bin/env python3
# Asserts that every kind of game file a retail disc comes as - an image
# (.iso) or a disc folder packed into one archive (.zip, .7z) - asks for the
# PS3 system software (PS3UPDAT.PUP) in waterbox.config "requiredWhen", which
# the window and the new-project wizard go by, so the user is asked for the
# file before the game starts.
#
# A disc packed as a .zip once did not ask. The project had no system
# software, the game started on a machine with none of the system libraries,
# and Sonic Unleashed stopped on the first frame reading address 0xfffffff0
# (Chimera issue 634). The core itself still starts a .zip or .7z without the
# file, as homebrew may come packed that way (owner, 2026-11-22); only a .iso
# is refused by the core (wbx-entry.cpp).
#
# Every kind asked for must also be a game file the package accepts
# ("extensions").
#
# With a second argument, Chimera's engine library (libchimera.so), the check
# also asks the engine itself - the code the window runs - which firmware a
# project of each kind of game file needs, which is the answer the user meets.
#
# Usage: check-firmware-required.py <waterbox.config> [<libchimera>]
import ctypes
import json
import sys

config = json.load(open(sys.argv[1]))

entry = [f for f in config["firmware"] if f["id"] == "PS3UPDAT.PUP"]
if len(entry) != 1:
    sys.exit("waterbox.config does not declare PS3UPDAT.PUP exactly once")
declared = {
    c["extension"].lower()
    for c in entry[0]["requiredWhen"]["any"]
    if c.get("slot") == "game" and "extension" in c
}
accepted = {e.lstrip(".").lower() for e in config["extensions"]}

problems = []
for needed in ("iso", "zip", "7z"):
    if needed not in declared:
        problems.append(f".{needed} is not in requiredWhen: the window would not ask for the system software")
for kind in sorted(declared - accepted):
    problems.append(f".{kind} asks for the system software but is not among the package's extensions")
engine = ""
if len(sys.argv) > 2:
    lib = ctypes.CDLL(sys.argv[2])
    lib.ce_firmware_evaluate.restype = ctypes.c_void_p
    lib.ce_firmware_evaluate.argtypes = [ctypes.c_char_p, ctypes.c_uint64] * 3 + [ctypes.POINTER(ctypes.c_uint64)]
    decl = json.dumps(config["firmware"]).encode()

    def needs(game):
        slots = json.dumps({"game": [game]}).encode()
        n = ctypes.c_uint64()
        p = lib.ce_firmware_evaluate(decl, len(decl), slots, len(slots), b"{}", 2, ctypes.byref(n))
        return json.loads(ctypes.string_at(p, n.value))

    for game, want in (("Some Game (Media Maker 2).zip", True), ("Some Game.7z", True),
                       ("Some Game.ISO", True), ("homebrew.elf", False)):
        if bool(needs(game)) != want:
            problems.append(f"the engine says a project of '{game}' {'does not need' if want else 'needs'} the system software")
    engine = "; the engine asks for it for a .zip, a .7z and an .iso, and for nothing for an .elf"
if problems:
    sys.exit("; ".join(problems))
print(f"a game file of kind {', '.join('.' + e for e in sorted(declared))} asks for the system software" + engine)
