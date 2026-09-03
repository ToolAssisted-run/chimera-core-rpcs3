#!/bin/bash
# The frontend half of the gate: load the RPCS3 package in Chimera (under
# Mono, on a private Xvfb display), boot lv2test.elf for a fixed number of frames
# with nothing pressed, and require the machine's memory to be byte-identical
# to the sandbox reference (which the core gate already holds equal to the
# native build). Then the same over a commercial disc, a machine-shaping
# setting, and the package's keybinds - the disc legs SKIP without content.
#
# Usage: ./run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=200
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/rpcs3.chimeraCore"
runwbx="$wb/bin/run-wbx"
wbx="$wb/bin/core.wbx"
sys="$root/extern/rpcs3"
game="$root/tests/roms/lv2test.elf"
disc=""
[ -f "$emu_exe" ] || { echo "Chimera not built: $emu_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$runwbx" ] || { echo "run-wbx not built (../build-core.sh)" >&2; exit 1; }

ok=0
failed=0
skipped=0
report() {
	printf "%-28s %-9s %s\n" "$1" "$2" "$3"
	case "$2" in
		PASS) ok=$((ok + 1)) ;;
		SKIP) skipped=$((skipped + 1)) ;;
		*) failed=$((failed + 1)) ;;
	esac
}
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

work="$here/work"
mkdir -p "$work"

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

SLICE=1048576

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="$4" romarg="$5"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
		echo "bytes=$SLICE"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "$romarg" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# the sandbox reference (the core gate holds run-wbx == the native build)
reference_ram() {
	local tag="$1" game="$2" settings="${3:-}"
	# run-wbx carries its own libminiboxhost via rpath; the chimera
	# LD_LIBRARY_PATH exported above would shadow it
	env -u LD_LIBRARY_PATH timeout 900 "$runwbx" "$wbx" \
		--frames "$frames" --report "$frames" --ram-out "$work/ref.$tag.ram.full" \
		"$game" > "$work/ref.$tag.log" 2>&1 || return 1
	head -c "$SLICE" "$work/ref.$tag.ram.full" > "$work/ref.$tag.ram.bin"
}

settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2" "${3:-{\}}"; }

# --- 1. lv2test through the frontend: RAM == the sandbox reference ---------
if ! reference_ram "base" "$game"; then
	report "game:frontend" FAIL "reference runner error (see tests/work/ref.base.log)"
elif ! run_frontend "base" "$config" "$frames" "$work/base.png" "$game"; then
	report "game:frontend" FAIL "no OK meta (see tests/work/base.log)"
elif ! cmp -s "$work/ref.base.ram.bin" "$work/base.ram.bin"; then
	report "game:frontend" FAIL "main memory differs from the sandbox reference"
elif [ "$(sed -n 's/^ramsize=//p' "$work/base.meta.txt")" != "268369920" ]; then
	report "game:frontend" FAIL "main memory is $(sed -n 's/^ramsize=//p' "$work/base.meta.txt") bytes, want 268369920"
else
	report "game:frontend" PASS "$frames frames of lv2test, main memory identical to the sandbox reference"
fi

# --- 2. a disc through the frontend -----------------------------------------
# A decrypted disc image in tests/roms-local (the user's, never committed)
# boots through Chimera with the firmware resolved the way the Firmware
# window stores it (keyed by core name and declaration id), and its main
# memory after a fixed number of frames is the sandbox reference's.
for f in "$root"/tests/roms-local/*.iso; do
	[ -f "$f" ] && { disc="$f"; break; }
done
pup="$root/tests/roms-local/PS3UPDAT.PUP"
dframes=120
if [ -z "$disc" ] || [ ! -f "$pup" ]; then
	report "disc:frontend" SKIP "needs a decrypted .iso and PS3UPDAT.PUP in tests/roms-local"
else
	firmware_json="$(python3 -c "import json,sys; print(json.dumps({'PS3UPDAT.PUP': sys.argv[1]}))" "$pup")"
	settings_config "$work/config.disc.ini" '{}' "$firmware_json"
	if ! env -u LD_LIBRARY_PATH timeout 900 "$runwbx" "$wbx" --firmware "$pup" \
		--frames "$dframes" --report "$dframes" --ram-out "$work/ref.disc.ram.full" \
		"$disc" > "$work/ref.disc.log" 2>&1; then
		report "disc:frontend" FAIL "reference runner error (see tests/work/ref.disc.log)"
	elif ! head -c "$SLICE" "$work/ref.disc.ram.full" > "$work/ref.disc.ram.bin"; then
		report "disc:frontend" FAIL "no reference memory"
	elif ! run_frontend "disc" "$work/config.disc.ini" "$dframes" "$work/disc.png" "$disc"; then
		report "disc:frontend" FAIL "no OK meta (see tests/work/disc.log)"
	elif ! cmp -s "$work/ref.disc.ram.bin" "$work/disc.ram.bin"; then
		report "disc:frontend" FAIL "main memory differs from the sandbox reference"
	else
		report "disc:frontend" PASS "$dframes frames of $(basename "$disc" | cut -c1-30), main memory identical to the sandbox reference"
	fi
fi

# --- 3. the disc on the GL renderer through the frontend --------------------
# The frontend offers its own GL context (the GPU bridge) when the renderer
# setting ends in -hw. The machine must not notice: main memory after the same
# frames equals the sandbox reference (which draws nothing: run-wbx is offered
# no context). The screenshot is the proof that the picture came from the GPU.
if [ -z "$disc" ] || [ ! -f "$pup" ]; then
	report "gpu:frontend" SKIP "needs the disc and the firmware"
else
	settings_config "$work/config.gpu.ini" '{}' "$firmware_json"
	# Far enough in that the game has drawn: this disc's first picture is at
	# frame 200 and nothing before it is anything but black, so a shorter run
	# proves only that a file was written.
	gframes=300
	if ! run_frontend "gpu" "$work/config.gpu.ini" "$gframes" "$work/gpu.png" "$disc"; then
		report "gpu:frontend" FAIL "no OK meta (see tests/work/gpu.log)"
	elif ! env -u LD_LIBRARY_PATH timeout 900 "$runwbx" "$wbx" --firmware "$pup" \
		--frames "$gframes" --report "$gframes" --ram-out "$work/ref.gpu.ram.full" \
		"$disc" > "$work/ref.gpu.log" 2>&1; then
		report "gpu:frontend" FAIL "reference runner error (see tests/work/ref.gpu.log)"
	elif ! head -c "$SLICE" "$work/ref.gpu.ram.full" > "$work/ref.gpu.ram.bin"; then
		report "gpu:frontend" FAIL "no reference memory"
	elif ! cmp -s "$work/ref.gpu.ram.bin" "$work/gpu.ram.bin"; then
		report "gpu:frontend" FAIL "main memory differs from the sandbox reference"
	elif ! grep -q "renderer opengl-hw through the GPU bridge" "$work/gpu.log"; then
		report "gpu:frontend" FAIL "the core did not get a GPU bridge from the frontend ($(grep -a 'chimera rpcs3: renderer' "$work/gpu.log" | head -1))"
	elif [ ! -s "$work/gpu.png" ]; then
		report "gpu:frontend" FAIL "no screenshot"
	elif ! lit="$(python3 "$here/lit-pixels.py" "$work/gpu.png" 10000)"; then
		report "gpu:frontend" FAIL "the screenshot is (nearly) black: $lit - the picture never reached the frontend"
	else
		report "gpu:frontend" PASS "$gframes frames on the GL renderer, main memory identical to the sandbox reference, $lit"
	fi
fi

# --- 3b. precompile sessions through the frontend ---------------------------
# The frontend runs itself twice as precompile sessions (what its orchestrator
# does before a first boot), each compiling its share into the compile cache
# under the Compile Cache path; a run on the LLVM recompiler afterwards
# fetches every object and compiles none.
cache_root="$chimera_root/build/CoreCache"
if [ ! -f "$pup" ]; then
	report "precompile:frontend" SKIP "needs the firmware"
else
	settings_config "$work/config.llvm.ini" '{"ppu_decoder": "llvm"}' "$firmware_json"
	rm -rf "$cache_root"
	( cd "$chimera_root" && timeout 900 mono "$emu_exe" --headless "--config=$work/config.llvm.ini" "--core=$package" "--precompile=0/2/game" "$game" ) > "$work/precompile-0.log" 2>&1 &
	pre0=$!
	( cd "$chimera_root" && timeout 900 mono "$emu_exe" --headless "--config=$work/config.llvm.ini" "--core=$package" "--precompile=1/2/game" "$game" ) > "$work/precompile-1.log" 2>&1 &
	pre1=$!
	# by pid, not a bare "wait": this script may be running an Xvfb of its own
	# in the background, and a bare wait never returns while it lives
	wait "$pre0" "$pre1"
	stored="$(sed -n 's/^precompile session [0-9]* of [0-9]*: \([0-9]*\) objects stored.*/\1/p' "$work/precompile-0.log" "$work/precompile-1.log" | awk '{s+=$1} END {print s+0}')"
	nfiles="$(find "$cache_root" -name '*.obj.gz' 2>/dev/null | wc -l)"
	if ! run_frontend "warm" "$work/config.llvm.ini" 100 "" "$game"; then
		report "precompile:frontend" FAIL "the warm run gave no OK meta (see tests/work/warm.log)"
	else
		fetched="$(sed -n 's/^chimera cache: \([0-9]*\) stored, \([0-9]*\) fetched.*/\2/p' "$work/warm.log" | tail -1)"
		wstored="$(sed -n 's/^chimera cache: \([0-9]*\) stored, \([0-9]*\) fetched.*/\1/p' "$work/warm.log" | tail -1)"
		# The warm run may still compile: a sweep sees the modules that are
		# there to be seen, and a program loads more as it runs (the GL
		# renderer alone pulls in libgcm_sys). What must never happen is
		# recompiling what the sessions already did - and the core gate's
		# cache:warm leg holds that a run which compiles and a run which
		# fetches are the same machine.
		if [ "$stored" -ge 2 ] && [ "$nfiles" = "$stored" ] && [ "${fetched:-0}" = "$stored" ]; then
			report "precompile:frontend" PASS "two sessions stored $stored objects under CoreCache/, the warm run fetched all $fetched and compiled ${wstored:-0} more (modules the program loaded at runtime)"
		else
			report "precompile:frontend" FAIL "sessions stored $stored ($nfiles files), warm run stored ${wstored:-?} fetched ${fetched:-?} ($(grep -a 'rror\|xception' "$work/precompile-0.log" | head -1 | cut -c1-100))"
		fi
	fi
fi

# --- 4. the package's bindings became the frontend's defaults ---------------
if out="$(python3 "$here/check-keybinds.py" "$config" "$wb/default_keybinds.json" "PlayStation 3 Controller" 2>&1)"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
