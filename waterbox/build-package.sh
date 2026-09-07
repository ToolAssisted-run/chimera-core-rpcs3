#!/bin/sh
# Builds the RPCS3 waterbox core package and installs it into a chimera
# checkout as build/Cores/rpcs3.chimeraCore.
#
# A package is core.wbx (fixed name) + waterbox.config + default_keybinds.json
# + file_slots.json + the licences manifest. The firmware (PS3UPDAT.PUP) is
# never inside it: the user's own, resolved by the frontend's firmware channel.
# Needs build/ffmpeg-guest (build-ffmpeg.sh guest) and a miniBox at spec v2.
#
# Usage: ./build-package.sh [-m <miniBox dir>] [-r <chimera root>]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-}"
chimera_root=""
while getopts "m:r:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		r) chimera_root="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass -r <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"
[ -n "$mb" ] || mb="$chimera_root/extern/chimera-common-minibox"

# Every step below builds against the miniBox guest toolchain, and each one used
# to find it by guessing at $HOME. That works on the machine the guess was
# written on and nowhere else - on a CI runner $HOME is not the checkout, so
# gcc was handed a specs file that does not exist and ffmpeg's configure failed
# with its complaint on stdout, which the bundle sends to /dev/null. Say where
# it is, once.
MINIBOX_SYSROOT="$mb/build/meson-cpp/guest-sysroot"
export MINIBOX_SYSROOT MINIBOX_DIR="$mb"

[ -f "$MINIBOX_SYSROOT/lib/musl-gcc.specs" ] || {
	echo "miniBox guest toolchain missing at $MINIBOX_SYSROOT" >&2
	echo "build it with: meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true && ninja -C <miniBox>/build/meson-cpp" >&2
	exit 1; }

# the guest: cmake archives + the musl ffmpeg + the adapter, linked by build-core.sh
[ -d "$root/build/ffmpeg-guest/lib" ] || sh "$here/build-ffmpeg.sh" guest

# The PPU and SPU recompilers are LLVM, so the guest needs one. Nothing else
# builds it: a machine that has never built this core has no LLVM at all, and
# the cmake configure below fails on the spot with "Can't find LLVM libraries".
# Only the table generator is taken from the native flavor - the rest of that
# flavor is for the native reference, which a package does not use.
if [ ! -d "$root/build/llvm-guest/lib/cmake/llvm" ]; then
	sh "$here/build-llvm.sh" tblgen
	sh "$here/build-llvm.sh" guest
fi

[ -d "$root/build/guest" ] || sh "$here/build-guest.sh"
MINIBOX_DIR="$mb" sh "$here/build-core.sh" -m "$mb"

staging="$root/build/package-staging"
rm -rf "$staging"
mkdir -p "$staging"
cp "$here/bin/core.wbx" "$staging/core.wbx"
cp "$here/waterbox.config" "$staging/waterbox.config"
cp "$here/default_keybinds.json" "$staging/default_keybinds.json"
cp "$here/file_slots.json" "$staging/file_slots.json"

# No assets: a PS3 core ships no data files of its own; the firmware is the
# user's and arrives through the firmware channel.

# the terms travel with the binary: this package may be downloaded on its
# own, and the emulator inside it is somebody else's work under somebody
# else's licence (see waterbox/package-licenses.json)
python3 "$mb/source/guest/package-licenses.py" "$root" "$staging"

# ---- version (see chimera docs: commit-as-version, stamped by CD) ----
core_version="${CORE_VERSION:-}"
if [ -z "$core_version" ]; then
	if commit="$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null)"; then
		git -C "$root" diff --quiet HEAD 2>/dev/null || commit="$commit-dirty"
		core_version="$commit+local"
	else
		core_version="unversioned+local"
	fi
fi
python3 - "$staging/waterbox.config" "$core_version" <<'PYVER'
import json, sys
path, version = sys.argv[1], sys.argv[2]
with open(path) as f:
    cfg = json.load(f)
cfg["version"] = version
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PYVER

# ---- provenance: what built this exact package (inputs only) ----
gccver="$(gcc -dumpfullversion)"
musl_version="$(cat "$mb/extern/musl/VERSION" 2>/dev/null || echo unknown)"
binutils_version="$(ld --version | head -1 | grep -o '[0-9][0-9.]*$' || echo unknown)"
os_id="$(. /etc/os-release 2>/dev/null && printf '%s %s' "${ID:-unknown}" "${VERSION_ID:-}" || echo unknown)"
guest_kit="$(git -C "$mb" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
rpcs3_pin="$(git -C "$root/extern/rpcs3" describe --tags --always 2>/dev/null || echo unknown)"
python3 - <<PYPROV
import json, subprocess
def git(*args, default="unknown"):
    try:
        return subprocess.run(["git", "-C", "$root", *args], capture_output=True,
                              text=True, check=True).stdout.strip()
    except Exception:
        return default
json.dump({
    "version": "$core_version",
    "source": {"commit": git("rev-parse", "HEAD"),
               "origin": git("config", "--get", "remote.origin.url", default=""),
               "dirty": "-dirty" in "$core_version"},
    "toolchain": {"compiler": "gcc $gccver", "binutils": "$binutils_version",
                  "target": "x86_64-linux-musl", "musl": "$musl_version"},
    "guestKit": {"name": "miniBox", "commit": "$guest_kit"},
    "upstream": {"name": "RPCS3", "pin": "$rpcs3_pin"},
    "builtOn": "$os_id",
}, open("$staging/build.json", "w"), indent=2, sort_keys=True)
PYPROV

cores_dir="$chimera_root/build/Cores"
mkdir -p "$cores_dir"
zip_path="$cores_dir/rpcs3.chimeraCore"
rm -f "$zip_path"
# deterministic packaging: sorted entries, fixed timestamp/permissions, pinned
# compression - the package's SHA1 is the core's identity (movies cite it)
python3 - "$staging" "$zip_path" <<'PYEOF'
import hashlib, os, sys, tempfile, zipfile

staging, zip_path = sys.argv[1], sys.argv[2]
FIXED_DATE = (1980, 1, 1, 0, 0, 0)

def write_package(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, dirs, files in os.walk(staging):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(root, name)
                info = zipfile.ZipInfo(os.path.relpath(full, staging), date_time=FIXED_DATE)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o644 << 16
                with open(full, "rb") as f:
                    z.writestr(info, f.read())

write_package(zip_path)
with tempfile.NamedTemporaryFile(suffix=".zip") as tmp:
    write_package(tmp.name)
    again = hashlib.sha1(open(tmp.name, "rb").read()).hexdigest()
first = hashlib.sha1(open(zip_path, "rb").read()).hexdigest()
if first != again:
    sys.exit(f"packaging is not deterministic: {first} then {again}")
print(f"package sha1 {first}")
PYEOF

for cache in "$chimera_root"/build/CoreCache/rpcs3-*; do
	[ -d "$cache" ] && rm -rf "$cache" || true
done
echo "packaged -> $zip_path"
