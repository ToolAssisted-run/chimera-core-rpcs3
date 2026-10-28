#!/bin/bash
# The frontend half of the gate: load the RPCS3 package in Chimera's window
# (run headless: no display is needed), boot lv2test.elf for a fixed number of frames
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

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/rpcs3.chimeraCore"
runwbx="$wb/bin/run-wbx"
wbx="$wb/bin/core.wbx"
sys="$root/extern/rpcs3"
game="$root/tests/roms/lv2test.elf"
disc=""
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
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

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini" \
	CHIMERA_QT_COMPILE_CACHE="$work/compile-cache"

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
	local project
	project="$(game_project "$tag" "$cfg" "$package" "$romarg")" || { cp "$work/projects/$tag.make.log" "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# the sandbox reference (the core gate holds run-wbx == the native build)
reference_ram() {
	local tag="$1" game="$2" settings="${3:-}"
	# run-wbx carries its own libminiboxhost via rpath; the chimera
	# LD_LIBRARY_PATH exported above would shadow it
	env -u LD_LIBRARY_PATH timeout 900 "$runwbx" "$wbx" ${settings:+--settings "$settings"} \
		--frames "$frames" --report "$frames" --ram-out "$work/ref.$tag.ram.full" \
		"$game" > "$work/ref.$tag.log" 2>&1 || return 1
	head -c "$SLICE" "$work/ref.$tag.ram.full" > "$work/ref.$tag.ram.bin"
}

# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings (and the firmware)
# settings_config was given for the config it runs with.
declare -A settings_of firmware_of
settings_config() { python3 "$here/settings-config.py" "$1" "$2" ${3:+"$3"}; settings_of["$1"]="$2"; firmware_of["$1"]="${3:-}"; }
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/projects" settings="${settings_of[$2]:-}" firmware="${firmware_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=()
	[ -n "$firmware" ] && mapfile -t fw < <(python3 -c 'import json,sys
for k, v in json.loads(sys.argv[1]).items(): print("--firmware"); print(k + "=" + v)' "$firmware")
	# one copy of each game, every run's project beside it
	local copy="$dir/$(basename "$game")"
	mkdir -p "$dir"
	[ -f "$copy" ] && [ "$(stat -c %s "$copy")" = "$(stat -c %s "$game")" ] || cp "$game" "$copy"
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" "$copy" \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/$1.make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

# --- 1. lv2test through the frontend: RAM == the sandbox reference ---------
# The renderer is named in every run here. The package's default is Vulkan,
# which a computer without a Vulkan driver - a CI runner - cannot give, and
# these runs are about the frontend and the machine, not about that.
# The reference runs the interpreter (run-wbx's own fallback) and the null
# renderer, this run GL. On LLVM, the package's default since chimera#316,
# one counter of lv2test's ends one apart from the reference's (the two
# decoders keep slightly different time), so this run names the
# interpreter. The legs below run the package's default against the same
# reference and their slices of memory agree; precompile:frontend holds that
# the default is LLVM.
settings_config "$work/settings.base.json" '{"renderer": "opengl-hw", "ppu_decoder": "interpreter"}'
if ! reference_ram "base" "$game"; then
	report "game:frontend" FAIL "reference runner error (see tests/work/ref.base.log)"
elif ! run_frontend "base" "$work/settings.base.json" "$frames" "$work/base.png" "$game"; then
	report "game:frontend" FAIL "no OK meta (see tests/work/base.log)"
elif ! cmp -s "$work/ref.base.ram.bin" "$work/base.ram.bin"; then
	report "game:frontend" FAIL "main memory differs from the sandbox reference"
elif [ "$(sed -n 's/^ramsize=//p' "$work/base.meta.txt")" != "268369920" ]; then
	report "game:frontend" FAIL "main memory is $(sed -n 's/^ramsize=//p' "$work/base.meta.txt") bytes, want 268369920"
else
	report "game:frontend" PASS "$frames frames of lv2test, main memory identical to the sandbox reference"
fi

# What content there is, found once: the disc the disc legs want, and the
# firmware that every leg below this line wants.
for f in "$root"/tests/roms-local/*.iso; do
	[ -f "$f" ] && { disc="$f"; break; }
done
pup="$root/tests/roms-local/PS3UPDAT.PUP"

# --- 1b. every restore rebuilds the renderer, the frame-0 anchor included ---
#
# Placed before the three disc legs because it needs NO disc: they are the
# slowest and hungriest things in this script, and a leg that can run without
# them should not be behind them (a run that dies at leg 3 used to take this
# one with it).
#
# On the GPU bridge the renderer's GL objects live in the driver and a savestate
# carries only their NAMES, so the engine mints a fresh context id on every load
# and this core rebuilds when the id it stored beside those objects no longer
# matches (GLGSRender::chimera_check_gl_context, patch 0021).
#
# One state used to slip through. m_chimera_gl_context starts at 0 and is first
# written the first time the RSX thread reaches do_local_task, which is AFTER
# on_init_thread has built every GL object the renderer holds - so a state taken
# in between carries a 0 while the objects exist, and the guard read that 0 as
# "this machine has never held any GL objects". The greenzone's frame-0 anchor
# is such a state, and it is the one TAStudio loads when a movie is replayed
# from the beginning. Reported on PCSX2 as chimera issue #126: corrupt picture
# from frame 0 or 1, clean from frame 2, which is exactly where TAStudio stops
# reaching for the anchor.
#
# WHAT IT MEASURES. How many calls cross the bridge on the frame after a
# restore. The rebuild is 5203 of them here; an ordinary frame of this program
# is about 55.
#
# WHY NOT PCSX2'S TEST. That leg asserts the frame after a restore crosses the
# bridge thousands of times, because an idle frame of its program crosses it
# once. The same test passes on a BROKEN rpcs3: restoring to frame 0 replays
# flip.elf's own gcm setup, 3549 calls with or without a rebuild. And the A/B
# PCSX2 could have used - the same restore under
# CHIMERA_GL_KEEP_OBJECTS_ON_LOAD - is not a control at frame 0 either, because
# that switch only stops the engine MINTING a new id, and a stored 0 differs
# from the live id however little it moved. Measured, both traps:
#
#   restore to      before the fix        after the fix
#   frame 0         3549 / 3549 (off)     8752 / 8752 (off)
#   frame 2           53 (off) / 5258       53 (off) / 5258
#
# So the leg asserts the two things that WERE false before the fix and are true
# after it: frame 2's crossings really are a rebuild (5258 against 53 with the
# rebuild off), and restoring the frame-0 anchor costs at least as much as
# restoring frame 2 - which it cannot if the anchor skipped the rebuild, since
# it does strictly more work when it does not (8752 = 3549 + 5203). Run against
# the package built before the fix it FAILS on the second: "3549 against 5258".
#
# WHAT THIS DOES NOT STAND IN FOR (chimera docs/gates.md, E): flip.elf is not a
# game and llvmpipe is not a driver. It proves the rebuild RUNS after every
# restore, not that a real game's picture is right on real hardware.
crun="$chimera_root/build/meson-linux/chimera-run"
flipelf="$root/tests/roms/flip.elf"
if [ ! -x "$crun" ] || [ ! -f "$pup" ] || [ ! -f "$flipelf" ]; then
	report "gl:rebuild-at-zero" SKIP "needs chimera-run ($crun), the firmware and tests/roms/flip.elf"
else
	gz="$work/glzero"
	rm -rf "$gz"; mkdir -p "$gz"
	printf '[Input]\nLogKey:#\n' > "$gz/none.txt"
	# a movie of its own: --rewind-loop needs frames to rewind through. A game
	# runs only in a project (Chimera, 2026-10-26): one of the program with no
	# input to record in, then one of the movie recorded
	( cd "$chimera_root" && env -u LD_LIBRARY_PATH "$crun" --new-project "$gz/none.chimeraProject" "$package" "$flipelf" \
		"$gz/none.txt" --settings '{"renderer":"opengl-hw"}' --firmware "PS3UPDAT.PUP=$pup" \
		&& env -u LD_LIBRARY_PATH timeout 900 "$crun" --project "$gz/none.chimeraProject" "$package" --files "$(dirname "$flipelf")" \
			--frames 40 --record "$gz/movie.txt" --firmware "PS3UPDAT.PUP=$pup" ) > "$gz/record.log" 2>&1
	( cd "$chimera_root" && env -u LD_LIBRARY_PATH "$crun" --new-project "$gz/movie.chimeraProject" "$package" "$flipelf" \
		"$gz/movie.txt" --settings '{"renderer":"opengl-hw"}' --firmware "PS3UPDAT.PUP=$pup" ) > "$gz/movie.make.log" 2>&1
	# the bridge crossings in the frame that follows a restore to $1, with the
	# rebuild left alone ($2 empty) or turned off ($2 = the keep-objects switch)
	restore_calls() {
		( cd "$chimera_root" && env -u LD_LIBRARY_PATH $2 CHIMERA_GL_TRACE=1 CHIMERA_GL_STATEAUDIT=1 \
			timeout 900 "$crun" --project "$gz/movie.chimeraProject" "$package" --files "$(dirname "$flipelf")" \
			--frames 40 --firmware "PS3UPDAT.PUP=$pup" \
			--gpu --greenzone 4096 --rewind-loop "$1",1 ) > "$gz/rewind.$1.$3.log" 2>&1
		awk '/ce-gl-audit\] restore/ { seen = 1; next }
		     seen && /^\[ce-gl\] frame/ { print $4; exit }' "$gz/rewind.$1.$3.log"
	}
	if [ ! -s "$gz/movie.txt" ]; then
		report "gl:rebuild-at-zero" FAIL "could not record a movie to rewind through (see tests/work/glzero/record.log)"
	else
		zero="$(restore_calls 0 "" on)"
		two="$(restore_calls 2 "" on)"
		two_off="$(restore_calls 2 CHIMERA_GL_KEEP_OBJECTS_ON_LOAD=1 off)"
		if grep -q "no context" "$gz/rewind.0.on.log"; then
			report "gl:rebuild-at-zero" SKIP "this machine offers the bridge no GL context: $(grep -a 'no context' "$gz/rewind.0.on.log" | head -1)"
		elif [ -z "$zero" ] || [ -z "$two" ] || [ -z "$two_off" ]; then
			report "gl:rebuild-at-zero" FAIL "a restore was not traced (see tests/work/glzero/rewind.*.log)"
		elif [ "$((two - two_off))" -lt 500 ]; then
			report "gl:rebuild-at-zero" FAIL "restoring frame 2 cost $two calls against $two_off with the rebuild off: the renderer was not rebuilt, so this leg cannot speak for the anchor either"
		elif [ "$zero" -lt "$two" ]; then
			report "gl:rebuild-at-zero" FAIL "restoring the frame-0 anchor cost $zero calls against $two restoring frame 2: the anchor's stored context id of zero was read as nothing to rebuild (chimera issue 126)"
		else
			report "gl:rebuild-at-zero" PASS "a restore rebuilds the renderer wherever it lands - the anchor costs $zero calls, frame 2 $two, and $two_off with the rebuild turned off"
		fi
	fi
fi

# --- 2. a disc through the frontend -----------------------------------------
# A decrypted disc image in tests/roms-local (the user's, never committed)
# boots through Chimera with the firmware resolved the way the Firmware
# window stores it (keyed by core name and declaration id), and its main
# memory after a fixed number of frames is the sandbox reference's.
dframes=120
if [ -z "$disc" ] || [ ! -f "$pup" ]; then
	report "disc:frontend" SKIP "needs a decrypted .iso and PS3UPDAT.PUP in tests/roms-local"
else
	firmware_json="$(python3 -c "import json,sys; print(json.dumps({'PS3UPDAT.PUP': sys.argv[1]}))" "$pup")"
	settings_config "$work/settings.disc.json" '{"renderer": "opengl-hw"}' "$firmware_json"
	if ! env -u LD_LIBRARY_PATH timeout 900 "$runwbx" "$wbx" --firmware "$pup" \
		--frames "$dframes" --report "$dframes" --ram-out "$work/ref.disc.ram.full" \
		"$disc" > "$work/ref.disc.log" 2>&1; then
		report "disc:frontend" FAIL "reference runner error (see tests/work/ref.disc.log)"
	elif ! head -c "$SLICE" "$work/ref.disc.ram.full" > "$work/ref.disc.ram.bin"; then
		report "disc:frontend" FAIL "no reference memory"
	elif ! run_frontend "disc" "$work/settings.disc.json" "$dframes" "$work/disc.png" "$disc"; then
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
	settings_config "$work/settings.gpu.json" '{"renderer": "opengl-hw"}' "$firmware_json"
	# Far enough in that the game has drawn: this disc's first picture is at
	# frame 200 and nothing before it is anything but black, so a shorter run
	# proves only that a file was written.
	gframes=300
	if ! run_frontend "gpu" "$work/settings.gpu.json" "$gframes" "$work/gpu.png" "$disc"; then
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
# Two compile sessions (chimera-run's --precompile: what the window's New
# Project runs before a first boot), each compiling its share of the game's
# code into the game's compile cache; a run on the LLVM recompiler afterwards
# fetches every object and compiles none.
#
# The cache is the test's own folder, not the user's: the user's is neither
# emptied by a test nor what makes this one pass (it did once, being warm
# already, when the sessions stored nothing).
# The window keeps a game's compile cache in a folder named by the hash of the
# game file, under the compile cache folder (CHIMERA_QT_COMPILE_CACHE, set at
# the top); the compile sessions are given that same folder, which is where
# the warm run looks.
cache_root="$work/compile-cache"
cache_dir="$cache_root/$(sha1sum "$game" | cut -d' ' -f1 | tr 'a-f' 'A-F')"
if [ ! -f "$pup" ]; then
	report "precompile:frontend" SKIP "needs the firmware"
else
	# no ppu_decoder: the warm run takes the package's default, which is the
	# LLVM recompiler (user-decided, 2026-10-19, chimera#316). Were it the
	# interpreter, the warm run would fetch nothing and this leg would fail.
	settings_config "$work/settings.llvm.json" '{"renderer": "opengl-hw"}' "$firmware_json"
	rm -rf "$cache_root"
	pre_project="$(game_project "precompile" "$work/settings.llvm.json" "$package" "$game")" || cp "$work/projects/precompile.make.log" "$work/precompile-0.log"
	# the sessions are chimera-run's: the compile sessions the window's New
	# Project runs, with the cache folder the warm run below will use
	( cd "$chimera_root" && timeout 900 "$crun" --project "$pre_project" "$package" --precompile "0/2/game" --cache-dir "$cache_dir" ) > "$work/precompile-0.log" 2>&1 &
	pre0=$!
	( cd "$chimera_root" && timeout 900 "$crun" --project "$pre_project" "$package" --precompile "1/2/game" --cache-dir "$cache_dir" ) > "$work/precompile-1.log" 2>&1 &
	pre1=$!
	wait "$pre0" "$pre1"
	stored="$(sed -n 's/^precompile session [0-9]* of [0-9]*: \([0-9]*\) objects stored.*/\1/p' "$work/precompile-0.log" "$work/precompile-1.log" | awk '{s+=$1} END {print s+0}')"
	nfiles="$(find "$cache_dir" -name '*.obj.gz' 2>/dev/null | wc -l)"
	if ! run_frontend "warm" "$work/settings.llvm.json" 100 "" "$game"; then
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
		#
		# One object is all there is to store: the program is a bare .elf,
		# the package asks for the system software only for a disc or a
		# package (requiredWhen), so the frontend hands none and no firmware
		# library is loaded to be compiled. The leg asked for two, from when
		# every boot was given the firmware. Two sessions each storing their
		# share is the core gate's cache:precompile, which gives it.
		if [ "$stored" -ge 1 ] && [ "$nfiles" = "$stored" ] && [ "${fetched:-0}" = "$stored" ]; then
			report "precompile:frontend" PASS "two sessions stored $stored objects in the compile cache, the warm run fetched all $fetched and compiled ${wstored:-0} more (modules the program loaded at runtime)"
		else
			report "precompile:frontend" FAIL "sessions stored $stored ($nfiles files), warm run stored ${wstored:-?} fetched ${fetched:-?} ($(grep -a 'rror\|xception' "$work/precompile-0.log" | head -1 | cut -c1-100))"
		fi
	fi
fi

# --- 3c. a PROJECT through the GUI proper ------------------------------------
# Everything above hands the frontend a config that already maps the firmware
# and drives a headless session. A user opens a PROJECT, in TAStudio, on a
# machine whose config may remember nothing: the project pins the firmware by
# hash and the Firmware folder must satisfy it. When that path failed it did
# so in silence - the disc booted with no system software, a machine that ran,
# polled nothing and drew nothing (2026-09-03, "black screen, every frame
# red") - so this leg opens a project with a FRESH config, seeks in TAStudio
# the way a user does, and requires the machine's memory to equal what the
# headless session produced at the same frame.
if [ -z "$disc" ] || [ ! -f "$pup" ]; then
	report "project:frontend" SKIP "needs the disc and the firmware"
else
	pframes=120
	python3 - "$package" "$disc" "$pup" "$work/gate.chimeraProject" <<'PYEOF'
import hashlib, json, sys, zipfile
def sha1(path, limit=None):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 24), b''):
            h.update(chunk)
    return h.hexdigest().upper()
package, disc, pup, out = sys.argv[1:5]
version = json.loads(zipfile.ZipFile(package).read('build.json'))['version']
import os
json.dump({
    'title': 'gate', 'description': '',
    'core': {'name': 'RPCS3', 'version': version, 'sha1': sha1(package)},
    'rerecords': 0,
    'files': [{'name': os.path.basename(disc), 'sha1': sha1(disc), 'slot': 'game'}],
    'settings': {}, 'firmware': [{'id': 'PS3UPDAT.PUP', 'sha1': sha1(pup)}],
    'markers': [], 'branches': [],
    'headers': {'MovieVersion': 'Chimera Tasproj v1.1', 'Platform': 'PS3'},
}, open(out, 'w'), indent=1)
PYEOF
	# the project resolves its disc beside itself; fresh settings remember no
	# firmware: it is found in the Firmware folder, which holds only the firmware
	ln -sf "$disc" "$work/$(basename "$disc")"
	mkdir -p "$work/firmware"
	cp -n "$pup" "$work/firmware/PS3UPDAT.PUP" 2>/dev/null
	echo '{}' > "$work/settings.project.json"
	job="$work/job.project.txt"
	printf 'frames=%s\nout=%s/project.ram.bin\nmeta=%s/project.meta.txt\nshot=%s/project.png\nbytes=%s\n' "$pframes" "$work" "$work" "$work" "$SLICE" > "$job"
	rm -f "$work/project.ram.bin" "$work/project.meta.txt" "$work/project.png"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" CHIMERA_FIRMWARE_DIR="$work/firmware" timeout 900 "$chimera_exe" --headless \
		"--config=$work/settings.project.json" "--project=$work/gate.chimeraProject" \
		"--lua=$here/tastudio-run.lua" ) > "$work/project.log" 2>&1 &
	gui=$!
	# a paused GUI and a dead machine both sit still; a modal dialog sits
	# still too, so a run whose clock stops is killed rather than waited for
	last=0; quiet=0
	while kill -0 $gui 2>/dev/null; do
		sleep 5
		pid=$(pgrep -f "^$chimera_exe --headless --config=$work/settings.project.json" | head -1)
		[ -z "$pid" ] && continue
		now=$(awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null || echo 0)
		if [ "$((now - last))" -ge 10 ]; then last=$now; quiet=0; else quiet=$((quiet + 5)); fi
		[ "$quiet" -ge 60 ] && { kill $pid; break; }
	done
	wait $gui 2>/dev/null
	if [ ! -f "$work/project.meta.txt" ] || ! grep -q "^status=OK" "$work/project.meta.txt"; then
		report "project:frontend" FAIL "the GUI run gave no OK meta - a dialog, a stall or a crash (see tests/work/project.log)"
	elif [ "$(sed -n 's/^lag=//p' "$work/project.meta.txt")" -ge "$pframes" ]; then
		report "project:frontend" FAIL "every frame was a lag frame: the machine never polled (no firmware?)"
	elif ! cmp -s "$work/ref.disc.ram.bin" "$work/project.ram.bin"; then
		report "project:frontend" FAIL "main memory differs from the sandbox reference after $pframes frames"
	else
		report "project:frontend" PASS "a project opened in TAStudio on a fresh config, seek to $pframes: lag $(sed -n 's/^lag=//p' "$work/project.meta.txt"), main memory identical to the sandbox reference"
	fi
fi

# --- 4. the bindings the package ships are the machine's when nobody changed them
# (the window keeps only the keys a person changed; fresh settings have none).
# The first run's project is the machine to ask.
printf 'out=%s/keys.controls.json\nmeta=%s/keys.meta.txt\n' "$work" "$work" > "$work/job.keys.txt"
rm -f "$work/keys.controls.json" "$work/keys.meta.txt"
( cd "$chimera_root" && MINIHAWK_JOB="$work/job.keys.txt" timeout 900 "$chimera_exe" --headless \
	"--config=$work/settings.base.json" "--project=$work/projects/base.chimeraProject" \
	"--lua=$here/dump-controls.lua" ) > "$work/keys.log" 2>&1
if [ ! -f "$work/keys.meta.txt" ] || ! grep -q "^status=OK" "$work/keys.meta.txt"; then
	report "keybinds" FAIL "the bindings were not written (see tests/work/keys.log)"
elif out="$(python3 "$here/check-keybinds.py" "$work/keys.controls.json" "$wb/default_keybinds.json" "PlayStation 3 Controller" 2>&1)"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
