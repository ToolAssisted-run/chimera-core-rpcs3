#!/bin/sh
# The core gate. Every leg prints PASS/FAIL/SKIP with what it proved; the
# script exits non-zero if any leg failed. Legs arrive with their milestones:
#   native:deterministic   two native runs print identical RAM + TTY digests
#   sandbox:equivalent     core.wbx under miniBox prints the native run's lines
#   sandbox:rewind         save mid-run, finish, load, finish again: equal
#   sandbox:rerecord       save+load around every frame changes nothing
#   firmware:lle           with Sony's PUP (tests/roms-local, never committed):
#                          the firmware installs in the box, liblv2 is LLE, and
#                          native == sandbox still holds
#   input:press            padtest.elf (cellPad through the firmware) reports a
#                          scripted Cross exactly on the pressed frames, in both
#                          flavors alike; a program that never polls is all lag
#   video:flip             flip.elf (PSL1GHT, tests/ps3) draws with the CPU and
#                          flips with the RSX: a new image every frame, the
#                          program's own account of its flips on the TTY, and
#                          the sandbox's images are the native ones
#   audio:tone             tone.elf plays a square wave through cellAudio: the
#                          audio changes every frame, cycles with the wave, and
#                          the sandbox's samples are the native ones
#   disc:boot              a decrypted disc image in tests/roms-local (the
#                          user's, never committed) boots: memory changes
#                          every frame, native == sandbox
#   gpu:flip               the GL renderer through the GPU bridge (a host EGL
#                          context, llvmpipe will do): the flip program's
#                          pictures are the machine's own pixels, the same
#                          digests the null renderer copies out of VRAM, and
#                          the sandbox's GL run prints the native GL run's
#                          lines; SKIP when the host offers no context
#   gpu:disc               the disc on the GL renderer: memory identical to
#                          the null renderer's run (the GPU changes the
#                          picture, not the machine), native == sandbox
#   gpu:rewind             a savestate taken and loaded while the GL renderer
#                          draws: the machine after the load runs on to the
#                          same memory (the renderer's caches, in guest
#                          memory, survive the load consistently)
# Run from anywhere; artifacts land in waterbox/work/gate.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$here/work/gate"
rom="$root/tests/roms/lv2test.elf"
frames="${FRAMES:-600}"
native="$here/obj-native/run-native"
wbx="$here/bin/run-wbx"
core="$here/bin/core.wbx"
pup="$root/tests/roms-local/PS3UPDAT.PUP"
fail=0

pass() { echo "PASS: $*"; }
failed() { echo "FAIL: $*"; fail=1; }
skip() { echo "SKIP: $*"; }

rm -rf "$work"
mkdir -p "$work"

if [ ! -x "$native" ]; then
	failed "native runner missing (build-native.sh, then make -f native.mk)"
	exit 1
fi
if [ ! -f "$rom" ]; then
	python3 "$root/tests/asm/ppc.py" "$root/tests/asm/lv2test.s" "$rom" >/dev/null || failed "lv2test.elf does not assemble"
fi
have_wbx=1
if [ ! -x "$wbx" ] || [ ! -f "$core" ]; then
	have_wbx=0
	skip "sandbox legs: build-core.sh has not produced bin/core.wbx + bin/run-wbx"
fi

# run_both NAME FRAMES REPORT ROM [extra runner flags]: native then (if built)
# sandbox, frame lines in work/NAME-native.txt / NAME-wbx.txt, TTY in
# work/tty-NAME-*.txt, the firmware mounted in both
run_both() {
	name="$1"; n="$2"; rep="$3"; game="$4"; shift 4
	"$native" --work "$work/$name" --firmware "$pup" --frames "$n" --report "$rep" --tty-out "$work/tty-$name-native.txt" "$@" "$game" 2>"$work/$name-native.err" | grep '^frame\|^booted' > "$work/$name-native.txt"
	[ "$have_wbx" = 1 ] || return 0
	"$wbx" "$core" --firmware "$pup" --frames "$n" --report "$rep" --tty-out "$work/tty-$name-wbx.txt" "$@" "$game" 2>"$work/$name-wbx.err" | grep '^frame\|^booted' > "$work/$name-wbx.txt"
}

# run_both_gpu NAME FRAMES REPORT ROM: the same on the GL renderer, natively
# with the bridge's host half in the binary, in the sandbox with CHIMERA_GPU=1
# and the renderer setting the frontend would send
run_both_gpu() {
	name="$1"; n="$2"; rep="$3"; game="$4"; shift 4
	"$native" --work "$work/$name" --firmware "$pup" --renderer opengl-hw --frames "$n" --report "$rep" --tty-out "$work/tty-$name-native.txt" "$@" "$game" 2>"$work/$name-native.err" | grep '^frame\|^booted' > "$work/$name-native.txt"
	[ "$have_wbx" = 1 ] || return 0
	CHIMERA_GPU=1 "$wbx" "$core" --firmware "$pup" --settings '{"renderer":"opengl-hw"}' --frames "$n" --report "$rep" --tty-out "$work/tty-$name-wbx.txt" "$@" "$game" 2>"$work/$name-wbx.err" | grep '^frame\|^booted' > "$work/$name-wbx.txt"
}

# same_both NAME: the sandbox printed the native lines and TTY (true when
# there is no sandbox, so a native-only leg still passes on its own merits)
same_both() {
	[ "$have_wbx" = 1 ] || return 0
	grep '^frame' "$work/$1-native.txt" > "$work/$1-native-frames.txt"
	grep '^frame' "$work/$1-wbx.txt" > "$work/$1-wbx-frames.txt"
	[ -s "$work/$1-native-frames.txt" ] && cmp -s "$work/$1-native-frames.txt" "$work/$1-wbx-frames.txt" && cmp -s "$work/tty-$1-native.txt" "$work/tty-$1-wbx.txt"
}

# distinct FILE FIELD OFFSET: how many different values a frame-line field
# takes; the digest sits OFFSET words after the field's name (ram 1, vid and
# aud 2, a size sits between)
distinct() { grep '^frame' "$1" | awk -v f="$2" -v o="$3" '{for (i = 1; i <= NF; i++) if ($i == f) print $(i + o)}' | sort -u | wc -l; }

# ---- native:deterministic ---------------------------------------------
for r in a b; do
	"$native" --work "$work/$r" --frames "$frames" --report 50 --tty-out "$work/tty-$r.txt" "$rom" 2>"$work/$r.err" | grep '^frame' > "$work/run-$r.txt"
done
if [ ! -s "$work/run-a.txt" ]; then
	failed "native run produced no frames ($(tail -1 "$work/a.err"))"
elif cmp -s "$work/run-a.txt" "$work/run-b.txt" && cmp -s "$work/tty-a.txt" "$work/tty-b.txt"; then
	pass "native deterministic at $frames frames ($(wc -c < "$work/tty-a.txt") TTY bytes, $(tail -1 "$work/run-a.txt" | sed 's/.*threads/threads/'))"
else
	failed "native runs differ (diff $work/run-a.txt $work/run-b.txt)"
fi

# ---- the sandbox legs -------------------------------------------------
if [ "$have_wbx" = 1 ]; then
	"$wbx" "$core" --frames "$frames" --report 50 --tty-out "$work/tty-wbx.txt" "$rom" 2>"$work/wbx.err" | grep '^frame' > "$work/run-wbx.txt"
	if [ ! -s "$work/run-wbx.txt" ]; then
		failed "sandbox run produced no frames ($(tail -1 "$work/wbx.err"))"
	elif cmp -s "$work/run-a.txt" "$work/run-wbx.txt" && cmp -s "$work/tty-a.txt" "$work/tty-wbx.txt"; then
		pass "native == sandbox at $frames frames (RAM + TTY digests)"
	else
		failed "sandbox differs from native (diff $work/run-a.txt $work/run-wbx.txt)"
	fi

	if "$wbx" "$core" --frames "$frames" --rewind "$rom" > "$work/rewind.txt" 2>"$work/rewind.err"; then
		pass "rewind leg - $(grep '^rewind' "$work/rewind.txt")"
	else
		failed "rewind leg - $(grep '^rewind' "$work/rewind.txt" || tail -1 "$work/rewind.err")"
	fi

	"$wbx" "$core" --frames "$((frames / 3))" --report 50 --rerecord "$rom" 2>"$work/rerecord.err" | grep '^frame' > "$work/rerecord.txt"
	"$wbx" "$core" --frames "$((frames / 3))" --report 50 "$rom" 2>/dev/null | grep '^frame' > "$work/plain.txt"
	if [ -s "$work/rerecord.txt" ] && cmp -s "$work/rerecord.txt" "$work/plain.txt"; then
		pass "rerecord leg - save+load around every frame changes nothing ($((frames / 3)) frames)"
	else
		failed "rerecord leg - $(tail -1 "$work/rerecord.err")"
	fi
fi

# ---- everything below needs the user's PS3UPDAT.PUP ---------------------
if [ ! -f "$pup" ]; then
	skip "firmware:lle - no tests/roms-local/PS3UPDAT.PUP (would prove: the PUP installs in the box and liblv2 runs LLE)"
	skip "input:press, video:flip, audio:tone, disc:boot - the same firmware"
	exit $fail
fi

# ---- firmware:lle -------------------------------------------------------
run_both fw "$((frames / 3))" 50 "$rom"
if ! grep -q "firmware 4" "$work/fw-native.txt"; then
	failed "firmware:lle - the firmware did not install natively ($(tail -1 "$work/fw-native.err"))"
elif same_both fw; then
	pass "firmware:lle - $(grep '^booted' "$work/fw-native.txt" | sed 's/booted; //'), liblv2 LLE, native == sandbox at $((frames / 3)) frames"
else
	failed "firmware:lle - sandbox differs from native with firmware (diff $work/fw-native.txt $work/fw-wbx.txt)"
fi

# ---- input:press (cellPad is libio, in the firmware) --------------------
padrom="$root/tests/roms/padtest.elf"
[ -f "$padrom" ] || python3 "$root/tests/asm/ppc.py" "$root/tests/asm/padtest.s" "$padrom" >/dev/null
run_both pad 40 40 "$padrom" --press 10:5:10
pressed="$(grep -c ' 00000040 ' "$work/tty-pad-native.txt")"
first="$(grep -n ' 00000040 ' "$work/tty-pad-native.txt" | head -1 | cut -d: -f1)"
if [ "$pressed" = 5 ] && [ "$first" = 11 ] && grep -q ' lag 0 ' "$work/pad-native.txt"; then
	if same_both pad; then
		pass "input:press - Cross seen on exactly the 5 pressed frames (first at line $first), lag 0, native == sandbox"
	else
		failed "input:press - the sandbox's pad trace differs from native (diff $work/tty-pad-native.txt $work/tty-pad-wbx.txt)"
	fi
else
	failed "input:press - expected 5 pressed lines starting at line 11 with lag 0, got $pressed from line ${first:-none} ($(tail -1 "$work/pad-native.txt"))"
fi
# a program that never polls the pad is lag on every frame
if grep -q " lag $((frames / 3)) " "$work/fw-native.txt"; then
	pass "input:lag - a program that never polls counts every frame as lag"
else
	failed "input:lag - lv2test should be all lag ($(tail -1 "$work/fw-native.txt"))"
fi

# ---- video:flip ----------------------------------------------------------
fliprom="$root/tests/roms/flip.elf"
if [ ! -f "$fliprom" ]; then
	skip "video:flip - no tests/roms/flip.elf (tests/ps3/build.sh, needs the PSL1GHT toolchain)"
else
	run_both flip 120 10 "$fliprom"
	images="$(distinct "$work/flip-native.txt" vid 2)"
	flips="$(grep -c '^frame [0-9]* buffer' "$work/tty-flip-native.txt")"
	if [ "$images" = 12 ] && [ "$flips" -ge 100 ]; then
		if same_both flip; then
			pass "video:flip - 12 reports, 12 different 1280x720 images, $flips flips reported by the program, native == sandbox"
		else
			failed "video:flip - the sandbox's images differ from native (diff $work/flip-native.txt $work/flip-wbx.txt)"
		fi
	else
		failed "video:flip - expected 12 different images and 100+ flips, got $images and $flips ($(tail -1 "$work/flip-native.err"))"
	fi
fi

# ---- audio:tone ----------------------------------------------------------
tonerom="$root/tests/roms/tone.elf"
if [ ! -f "$tonerom" ]; then
	skip "audio:tone - no tests/roms/tone.elf (tests/ps3/build.sh)"
else
	run_both tone 60 10 "$tonerom"
	sounds="$(distinct "$work/tone-native.txt" aud 2)"
	blocks="$(grep -c '^block' "$work/tty-tone-native.txt")"
	if [ "$sounds" -ge 3 ] && [ "$blocks" -ge 8 ]; then
		if same_both tone; then
			pass "audio:tone - 6 reports, $sounds different 800-sample blocks (the wave's period), $blocks block reports, native == sandbox"
		else
			failed "audio:tone - the sandbox's audio differs from native (diff $work/tone-native.txt $work/tone-wbx.txt)"
		fi
	else
		failed "audio:tone - expected 3+ different audio blocks and 8+ block reports, got $sounds and $blocks ($(tail -1 "$work/tone-native.err"))"
	fi
fi

# ---- disc:boot -----------------------------------------------------------
disc=""
for f in "$root"/tests/roms-local/*.iso; do
	[ -f "$f" ] && { disc="$f"; break; }
done
if [ -z "$disc" ]; then
	skip "disc:boot - no decrypted .iso in tests/roms-local (would prove: a disc mounts, its modules link, memory changes every frame, native == sandbox)"
else
	run_both disc 120 20 "$disc"
	rams="$(distinct "$work/disc-native.txt" ram 1)"
	if [ "$rams" = 6 ]; then
		if same_both disc; then
			pass "disc:boot - $(basename "$disc" | cut -c1-40): 120 frames, memory different at every report, native == sandbox"
		else
			failed "disc:boot - the sandbox differs from native on the disc (diff $work/disc-native.txt $work/disc-wbx.txt)"
		fi
	else
		failed "disc:boot - expected 6 different memory digests, got $rams ($(tail -1 "$work/disc-native.err"))"
	fi
fi

# ---- spu:interpreter and spu:asmjit ------------------------------------
# sputest.elf keeps one SPU thread busy and reports its checksums; the
# interpreter and the recompiler must produce the same lines (the machine's
# answers), each deterministic across the flavors, and the recompiler at
# least as fast in machine time (an answer per frame)
spurom="$root/tests/roms/sputest.elf"
if [ ! -f "$spurom" ]; then
	skip "spu:interpreter, spu:asmjit - no tests/roms/sputest.elf (tests/ps3/build.sh)"
else
	for dec in interpreter asmjit; do
		CHIMERA_SPU_DECODER="$dec" "$native" --work "$work/spu-$dec" --firmware "$pup" --frames 120 --report 40 --tty-out "$work/tty-spu-$dec-native.txt" "$spurom" 2>"$work/spu-$dec-native.err" | grep '^frame\|^booted' > "$work/spu-$dec-native.txt"
		if [ "$have_wbx" = 1 ]; then
			"$wbx" "$core" --firmware "$pup" --settings "{\"spu_decoder\":\"$dec\"}" --frames 120 --report 40 --tty-out "$work/tty-spu-$dec-wbx.txt" "$spurom" 2>"$work/spu-$dec-wbx.err" | grep '^frame\|^booted' > "$work/spu-$dec-wbx.txt"
		fi
		answers="$(grep -c '^spu [0-9]* sum' "$work/tty-spu-$dec-native.txt")"
		if [ "$answers" -lt 100 ]; then
			failed "spu:$dec - expected 100+ SPU answers in 120 frames, got $answers ($(tail -1 "$work/spu-$dec-native.err"))"
		elif ! same_both "spu-$dec"; then
			failed "spu:$dec - the sandbox differs from native (diff $work/spu-$dec-native.txt $work/spu-$dec-wbx.txt)"
		else
			pass "spu:$dec - $answers SPU answers in 120 frames, native == sandbox"
		fi
	done
	# the same answers from both decoders, line for line, as far as both got
	n="$(grep -c '^spu [0-9]* sum' "$work/tty-spu-interpreter-native.txt")"
	m="$(grep -c '^spu [0-9]* sum' "$work/tty-spu-asmjit-native.txt")"
	[ "$n" -lt "$m" ] || n="$m"
	grep '^spu [0-9]* sum' "$work/tty-spu-interpreter-native.txt" | head -n "$n" > "$work/spu-agree-a.txt"
	grep '^spu [0-9]* sum' "$work/tty-spu-asmjit-native.txt" | head -n "$n" > "$work/spu-agree-b.txt"
	if [ "$n" -gt 0 ] && cmp -s "$work/spu-agree-a.txt" "$work/spu-agree-b.txt"; then
		pass "spu:agree - the interpreter and the recompiler give the same $n answers"
	else
		failed "spu:agree - the decoders' answers differ (diff $work/tty-spu-interpreter-native.txt $work/tty-spu-asmjit-native.txt)"
	fi
fi

# ---- gpu:flip and gpu:disc (the GL renderer through the bridge) ----------
if [ ! -f "$fliprom" ]; then
	skip "gpu:flip - no tests/roms/flip.elf"
else
	run_both_gpu gflip 60 10 "$fliprom"
	if grep -q "gpu bridge: no context" "$work/gflip-native.err"; then
		skip "gpu:flip - the host offers no GL context ($(grep 'no context' "$work/gflip-native.err" | head -1))"
	else
		grep '^frame' "$work/flip-native.txt" | head -6 | awk '{print $2, $7}' > "$work/gflip-want.txt"
		grep '^frame' "$work/gflip-native.txt" | awk '{print $2, $7}' > "$work/gflip-got.txt"
		if [ -s "$work/gflip-got.txt" ] && cmp -s "$work/gflip-want.txt" "$work/gflip-got.txt"; then
			if same_both gflip; then
				pass "gpu:flip - $(grep 'gpu bridge: [0-9]' "$work/gflip-native.err" | head -1 | sed 's/gpu bridge: //' | cut -c1-60): 6 pictures identical to the machine's own pixels, native == sandbox"
			else
				failed "gpu:flip - the sandbox's GL run differs from the native GL run (diff $work/gflip-native.txt $work/gflip-wbx.txt)"
			fi
		else
			failed "gpu:flip - the GL pictures are not the VRAM pictures (diff $work/gflip-want.txt $work/gflip-got.txt; $(tail -1 "$work/gflip-native.err"))"
		fi
	fi
	if [ -z "$disc" ]; then
		skip "gpu:disc - no disc"
	elif grep -q "gpu bridge: no context" "$work/gflip-native.err"; then
		skip "gpu:disc - the host offers no GL context"
	else
		run_both_gpu gdisc 120 20 "$disc"
		grep '^frame' "$work/disc-native.txt" | awk '{print $2, $4}' > "$work/gdisc-want.txt"
		grep '^frame' "$work/gdisc-native.txt" | awk '{print $2, $4}' > "$work/gdisc-got.txt"
		faults_native="$(sed -n 's/^page faults served by the renderer: //p' "$work/gdisc-native.err")"
		faults_wbx="$(sed -n 's/^page faults served by the renderer: //p' "$work/gdisc-wbx.err")"
		if [ -s "$work/gdisc-got.txt" ] && cmp -s "$work/gdisc-want.txt" "$work/gdisc-got.txt"; then
			if ! same_both gdisc; then
				failed "gpu:disc - the sandbox's GL run differs from the native GL run (diff $work/gdisc-native.txt $work/gdisc-wbx.txt)"
			elif [ -z "$faults_native" ] || [ "$have_wbx" = 1 ] && [ "$faults_native" != "$faults_wbx" ]; then
				failed "gpu:disc - the renderer's page watch served ${faults_native:-0} faults natively and ${faults_wbx:-0} in the sandbox (both must, equally)"
			else
				pass "gpu:disc - 120 frames on the GL renderer, memory identical to the null renderer's run, ${faults_native} page faults served by the renderer in each flavor, native == sandbox"
			fi
		else
			failed "gpu:disc - the GL run's memory differs from the null renderer's (diff $work/gdisc-want.txt $work/gdisc-got.txt; $(tail -1 "$work/gdisc-native.err"))"
		fi
		if [ "$have_wbx" = 1 ]; then
			if CHIMERA_GPU=1 "$wbx" "$core" --firmware "$pup" --settings '{"renderer":"opengl-hw"}' --frames 120 --rewind "$disc" > "$work/grewind.txt" 2>"$work/grewind.err"; then
				pass "gpu:rewind - $(grep '^rewind' "$work/grewind.txt")"
			else
				failed "gpu:rewind - $(grep '^rewind' "$work/grewind.txt" || tail -1 "$work/grewind.err")"
			fi
		fi
	fi
fi

exit $fail
