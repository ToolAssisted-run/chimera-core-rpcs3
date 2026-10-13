#!/usr/bin/env python3
"""Does the Vulkan bridge answer every command RPCS3 asks a device for by name?

    check-vk-names.py <VKProcTable.h> <vk-bridge-guest.c> <offered-extensions.txt>

RPCS3 keeps a table of commands that belong to extensions (VKProcTable.h) and
asks the device for each by name. Whether it then CALLS one depends on whether
the device says it has the extension - not on whether the name was answered.
So an extension the bridge offers has to answer every name RPCS3 asks for, or
RPCS3 calls address 0 (Tekken 6, chimera issue 268: the bridge offered
VK_KHR_synchronization2 and had no vkCmdSetEvent2KHR).

The table says which extension each command belongs to in a comment above it
("// KHR_synchronization2"). Exit 0 when every command of an offered extension
is answered, 1 when one is not, 2 when a file cannot be read or the table has
changed shape."""
import re
import sys


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    try:
        table = open(sys.argv[1], encoding="utf-8").read()
        guest = open(sys.argv[2], encoding="ascii").read()
        offered = set(open(sys.argv[3], encoding="ascii").read().split())
    except OSError as e:
        print(f"{e.filename}: {e.strerror}")
        return 2
    answered = set(re.findall(r'^\t\{"(vk[A-Za-z0-9]+)", ', guest, re.M))
    asked = []          # (extension, command)
    extension = None
    for line in table.split("\n"):
        m = re.match(r"//\s*((?:KHR|EXT)_[a-z0-9_]+)\s*$", line)
        if m:
            extension = "VK_" + m.group(1)
            continue
        m = re.match(r"VK_FUNC\((vk[A-Za-z0-9]+)\);", line)
        if m:
            if extension is None:
                print(f"{m.group(1)} has no extension named above it")
                return 2
            asked.append((extension, m.group(1)))
    if not asked or not answered or not offered:
        print("nothing to check: %d commands asked for, %d answered, %d extensions offered"
              % (len(asked), len(answered), len(offered)))
        return 2
    missing = [(e, c) for e, c in asked if e in offered and c not in answered]
    if missing:
        print("offered but not answered: " + ", ".join("%s (%s)" % (c, e) for e, c in missing))
        return 1
    of_offered = [c for e, c in asked if e in offered]
    print("%d of the %d commands RPCS3 asks for by name belong to an extension the bridge offers, and all are answered"
          % (len(of_offered), len(asked)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
