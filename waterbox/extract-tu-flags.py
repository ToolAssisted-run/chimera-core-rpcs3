#!/usr/bin/env python3
"""Print the compile flags (defines + includes + std) CMake used for one TU,
so the driver compiles with EXACTLY the flags the libraries were built with.
Usage: extract-tu-flags.py <builddir>/compile_commands.json <file-suffix>
"""
import json
import shlex
import sys

cc = json.load(open(sys.argv[1]))
for e in cc:
    if e["file"].endswith(sys.argv[2]):
        args = shlex.split(e["command"])
        keep = []
        i = 1
        while i < len(args):
            a = args[i]
            if a in ("-o", "-c"):
                i += 2 if a == "-o" else 1
                continue
            if a.startswith(("-D", "-I", "-isystem", "-std=")):
                if a == "-isystem":
                    keep += [a, args[i + 1]]
                    i += 2
                    continue
                keep.append(a)
            i += 1
        print(" ".join(shlex.quote(k) for k in keep))
        sys.exit(0)
print("TU not found", file=sys.stderr)
sys.exit(1)
