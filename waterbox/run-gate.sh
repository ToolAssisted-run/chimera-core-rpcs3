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
#   input:port2            padtest2.elf reads the SECOND pad: with --ports
#                          1100000 (the port2 setting) a Cross pressed on it
#                          shows on exactly the pressed frames, and with only
#                          port 1 plugged in it never shows
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
#   disc:install           what a game's own data install costs the machine:
#                          a file copied off the disc onto the console's hard
#                          disk, 64 KiB at a time the way an installer does,
#                          costs the machine nothing (it is held as a
#                          reference to the disc) and reads back byte for byte;
#                          the sandbox decides the same and runs to the same
#                          memory (same_memory, not same_both - see issue #120)
#   disc:install:encrypted the same off a Redump image, whose data regions the
#                          ISO layer decrypts on the way past - with the
#                          negative control in the leg: read as it lies rather
#                          than as the console reads it, every byte is kept
#   disc:install:state     and the reference survives a savestate loaded in
#                          ANOTHER PROCESS, which is what opening the project
#                          tomorrow is: the installed file still reads back as
#                          the disc's own bytes
#   pkg:boot               a digital-only title's .pkg in tests/roms-local IS
#                          the game: it installs onto the console's hard disk
#                          and the machine boots and runs what the install
#                          produced, with no disc anywhere
#   pkg:content            a package that is not a game on its own put in the
#                          Game slot is refused by name before the machine
#                          starts: content FOR a game (a DLC, a licence), or
#                          an UPDATE, which installs a bootable EBOOT.BIN
#                          exactly as its game does and is still part of one
#   pkg:licence            a purchased title (REQUIRE_LICENSE) boots with its
#                          .rap in the Licence slot, and without it says which
#                          licence is missing
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
#   gpu:context            the same across two PROCESSES, which is what opening
#                          a project tomorrow is: the GL objects the state
#                          names belong to a context that is gone, so the
#                          renderer notices and builds them again - and the
#                          picture after the load is the picture before the
#                          save, rather than no picture at all
#   (each gpu leg also fails when the bridge's dispatcher had no CASE for an
#    opcode the guest sent it, however well the pictures then compared - see
#    bridge_answered, and gl-host.c's default arm, for why)
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

npass=0
nfail=0
nskip=0
pass() { echo "PASS: $*"; npass=$((npass + 1)); }
failed() { echo "FAIL: $*"; nfail=$((nfail + 1)); fail=1; }
skip() { echo "SKIP: $*"; nskip=$((nskip + 1)); }

# Every exit goes through this. A skipped leg has to be COUNTED or a run that
# did four legs of twenty-five reads exactly like a run that did all of them:
# the same green tick, the same silence. (chimera docs/gates.md, mode G.)
summary() {
	echo "---"
	echo "gate: $npass passed, $nfail failed, $nskip skipped"
	exit $fail
}

# One gate at a time. Two runs at once each began by deleting the other's work
# directory, and on 2026-09-21 that produced a pkg:boot FAIL for a package that
# had booted: the second run's rm -rf took the first run's files out from under
# it while it was reading them. A second run now waits for the first to finish.
mkdir -p "$here/work"
exec 9>"$here/work/gate.lock"
if ! flock -n 9; then
	echo "gate: another run holds $here/work/gate.lock - waiting for it" >&2
	flock 9
fi

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
# sandbox, frame lines in work/NAME-native.txt / NAME-wbx.txt, the whole of
# stdout in work/NAME-*-out.txt, TTY in work/tty-NAME-*.txt, firmware in both
run_both() {
	name="$1"; n="$2"; rep="$3"; game="$4"; shift 4
	"$native" --work "$work/$name" --firmware "$pup" --frames "$n" --report "$rep" --tty-out "$work/tty-$name-native.txt" "$@" "$game" > "$work/$name-native-out.txt" 2>"$work/$name-native.err"
	grep '^frame\|^booted' "$work/$name-native-out.txt" > "$work/$name-native.txt"
	[ "$have_wbx" = 1 ] || return 0
	"$wbx" "$core" --firmware "$pup" --frames "$n" --report "$rep" --tty-out "$work/tty-$name-wbx.txt" "$@" "$game" > "$work/$name-wbx-out.txt" 2>"$work/$name-wbx.err"
	grep '^frame\|^booted' "$work/$name-wbx-out.txt" > "$work/$name-wbx.txt"
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
# there is no sandbox, so a native-only leg still passes on its own merits -
# and then it must say vs_sandbox rather than claim the equivalence)
same_both() {
	[ "$have_wbx" = 1 ] || return 0
	grep '^frame' "$work/$1-native.txt" > "$work/$1-native-frames.txt"
	grep '^frame' "$work/$1-wbx.txt" > "$work/$1-wbx-frames.txt"
	[ -s "$work/$1-native-frames.txt" ] && cmp -s "$work/$1-native-frames.txt" "$work/$1-wbx-frames.txt" && cmp -s "$work/tty-$1-native.txt" "$work/tty-$1-wbx.txt"
}

# same_memory NAME: the weaker claim, for a leg run on somebody's GAME. The
# sandbox computed the native run's MEMORY and TTY, but not necessarily its
# frame lines: a real game can still differ in the timing fields (issue #120 -
# Resident Evil 5 ends five frames on 1083339 us natively and 1083353 in the
# box, with every memory, TTY, video and audio digest identical). A leg about
# what the memory filesystem decided must not fail for that, and must not claim
# the equality it did not check either - so it says this instead.
same_memory() {
	[ "$have_wbx" = 1 ] || return 0
	for fl in native wbx; do
		grep '^frame' "$work/$1-$fl.txt" | sed 's/.* ram \([0-9a-f]*\) .*/\1/' > "$work/$1-$fl-ram.txt"
	done
	[ -s "$work/$1-native-ram.txt" ] && cmp -s "$work/$1-native-ram.txt" "$work/$1-wbx-ram.txt" && cmp -s "$work/tty-$1-native.txt" "$work/tty-$1-wbx.txt"
}

# bridge_answered FILE...: did the GPU bridge have a case for every opcode the
# guest sent it? The dispatcher's default arm logs and returns 0, and 0 is a
# perfectly plausible answer to nearly every question the bridge carries - so a
# guest that was answered and a guest that was shrugged at look the same, and
# two flavours that were both shrugged at compare EQUAL.
#
# That is not a worry, it is a measurement. GL_OP_CONTEXT_ID (chimera issue
# #43) had no case in gl-host.c for as long as the opcode existed. gpu:flip
# passed green while a single 60-frame sandbox run printed "opcode 4 has no
# case" 365 times, and gpu:context - the leg that exists for exactly that
# opcode - could not have passed at all. Absent was indistinguishable from
# working (~/chimera/docs/gates.md, mode C).
#
# So no gpu leg may go green over that line. Every one of them runs this
# first, on both flavours' stderr, and the message names the opcodes.
bridge_gap=""
bridge_answered() {
	bridge_gap=""
	for f in "$@"; do
		[ -f "$f" ] || continue
		grep -q 'has no case' "$f" || continue
		bridge_gap="the GPU bridge had no case for $(grep -o 'opcode [0-9]*' "$f" | sort -u | tr '\n' ',' | sed 's/,$//; s/,/, /g') and answered 0 ($(basename "$f"))"
		return 1
	done
	return 0
}

# gpu_ran NAME: did both flavours actually produce frame lines? A run that was
# killed, hung or crashed leaves an EMPTY frame file, and an empty file
# compared against a full one reads as "the two flavours DIFFER" - which sends
# the next reader hunting a divergence instead of a corpse. Measured on
# 2026-09-21: the sandbox GL run on bejeweled3.iso never completes frame 1 (it
# faults guest pages in at ~370 MiB/s from about 95 s in until it is OOM-killed,
# while the same disc runs 20 frames natively on GL in three seconds and 20
# frames in the sandbox on the NULL renderer in seconds), and the leg said
# "differs". Absent must not wear the costume of failed
# (~/chimera/docs/gates.md, mode C).
gpu_absent=""
gpu_ran() {
	gpu_absent=""
	for fl in native wbx; do
		[ "$fl" = wbx ] && [ "$have_wbx" != 1 ] && continue
		if ! grep -q '^frame' "$work/$1-$fl.txt" 2>/dev/null; then
			gpu_absent="the $fl GL run produced no frame at all - killed, hung or refused to start ($(grep -v 'Failed to open /proc/self/status' "$work/$1-$fl.err" 2>/dev/null | tail -1))"
			return 1
		fi
	done
	return 0
}

# vs_sandbox: what a leg that passed same_both is entitled to SAY. With a
# sandboxed core built, the two flavors' lines were compared and matched. With
# no core.wbx there is nothing to compare, and a leg that still printed
# "native == sandbox" asserted an equivalence nobody computed - absent wearing
# the costume of verified (~/chimera/docs/gates.md, mode C).
vs_sandbox() {
	if [ "$have_wbx" = 1 ]; then
		echo "native == sandbox"
	else
		echo "native only, no sandboxed core built"
	fi
}

# vs_memory: the same courtesy for same_memory - with no sandboxed core there
# is no second flavour and the leg must not pretend there was one.
vs_memory() {
	if [ "$have_wbx" = 1 ]; then
		echo ", and the sandbox made the same decision and ran to the same memory"
	else
		echo " (native only, no sandboxed core built)"
	fi
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
	# EVERY leg below this line, one SKIP each, by the name it would have
	# printed. Without the firmware none of them says anything at all, and a
	# leg that says nothing is a leg nobody can ask "when did that last
	# actually run?" about - which is how a whole gate goes unnoticed.
	skip "firmware:lle - no tests/roms-local/PS3UPDAT.PUP (would prove: the PUP installs in the box and liblv2 runs LLE)"
	for leg in input:press input:port2 input:lag video:flip audio:tone \
		disc:boot disc:install disc:install:encrypted disc:install:state \
		pkg:boot pkg:content pkg:licence ppu:llvm \
		cache:objects cache:warm cache:precompile \
		spu:interpreter spu:asmjit spu:agree rsx:drain spu:barrier \
		gpu:flip gpu:disc gpu:rewind gpu:context; do
		skip "$leg - the same firmware"
	done
	summary
fi

# ---- firmware:lle -------------------------------------------------------
run_both fw "$((frames / 3))" 50 "$rom"
if ! grep -q "firmware 4" "$work/fw-native.txt"; then
	failed "firmware:lle - the firmware did not install natively ($(tail -1 "$work/fw-native.err"))"
elif same_both fw; then
	pass "firmware:lle - $(grep '^booted' "$work/fw-native.txt" | sed 's/booted; //'), liblv2 LLE, $(vs_sandbox) at $((frames / 3)) frames"
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
		pass "input:press - Cross seen on exactly the 5 pressed frames (first at line $first), lag 0, $(vs_sandbox)"
	else
		failed "input:press - the sandbox's pad trace differs from native (diff $work/tty-pad-native.txt $work/tty-pad-wbx.txt)"
	fi
else
	failed "input:press - expected 5 pressed lines starting at line 11 with lag 0, got $pressed from line ${first:-none} ($(tail -1 "$work/pad-native.txt"))"
fi
# ---- input:port2 (the port settings plug a second pad in) ---------------
pad2rom="$root/tests/roms/padtest2.elf"
[ -f "$pad2rom" ] || python3 "$root/tests/asm/ppc.py" "$root/tests/asm/padtest2.s" "$pad2rom" >/dev/null
# Cross on port 2 is wire index 17 + 10
run_both pad2 40 40 "$pad2rom" --ports 1100000 --press 10:5:27
pressed2="$(grep -c ' 00000040 ' "$work/tty-pad2-native.txt")"
first2="$(grep -n ' 00000040 ' "$work/tty-pad2-native.txt" | head -1 | cut -d: -f1)"
run_both pad2off 40 40 "$pad2rom" --ports 1000000 --press 10:5:27
pressedoff="$(grep -c ' 00000040 ' "$work/tty-pad2off-native.txt")"
if [ "$pressed2" = 5 ] && [ "$first2" = 11 ] && [ "$pressedoff" = 0 ]; then
	if same_both pad2 && same_both pad2off; then
		pass "input:port2 - Cross on a plugged-in second pad seen on exactly the 5 pressed frames, and on an empty port never; $(vs_sandbox)"
	else
		failed "input:port2 - the sandbox's second-pad trace differs from native (diff $work/tty-pad2-native.txt $work/tty-pad2-wbx.txt)"
	fi
else
	failed "input:port2 - expected 5 pressed lines from line 11 with port 2 plugged in and 0 without, got $pressed2 from line ${first2:-none} and $pressedoff"
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
			pass "video:flip - 12 reports, 12 different 1280x720 images, $flips flips reported by the program, $(vs_sandbox)"
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
			pass "audio:tone - 6 reports, $sounds different 800-sample blocks (the wave's period), $blocks block reports, $(vs_sandbox)"
		else
			failed "audio:tone - the sandbox's audio differs from native (diff $work/tone-native.txt $work/tone-wbx.txt)"
		fi
	else
		failed "audio:tone - expected 3+ different audio blocks and 8+ block reports, got $sounds and $blocks ($(tail -1 "$work/tone-native.err"))"
	fi
fi

# ---- disc:boot -----------------------------------------------------------
# The discs the user put in tests/roms-local, sorted by whether they can be
# read without a key: an image with a .dkey beside it is a Redump dump, whose
# data regions are encrypted, and booting one as though it were in the clear
# gets "Invalid file or folder" rather than a disc.
plaindisc=""
encdisc=""
for f in "$root"/tests/roms-local/*.iso; do
	[ -f "$f" ] || continue
	if [ -f "${f%.iso}.dkey" ] || [ -f "${f%.iso}.key" ]; then
		[ -z "$encdisc" ] && encdisc="$f"
	else
		[ -z "$plaindisc" ] && plaindisc="$f"
	fi
done
disc="$plaindisc"
if [ -z "$disc" ]; then
	skip "disc:boot - no decrypted .iso in tests/roms-local (would prove: a disc mounts, its modules link, memory changes every frame, native == sandbox)"
else
	run_both disc 120 20 "$disc"
	rams="$(distinct "$work/disc-native.txt" ram 1)"
	if [ "$rams" = 6 ]; then
		if same_both disc; then
			pass "disc:boot - $(basename "$disc" | cut -c1-40): 120 frames, memory different at every report, $(vs_sandbox)"
		else
			failed "disc:boot - the sandbox differs from native on the disc (diff $work/disc-native.txt $work/disc-wbx.txt)"
		fi
	else
		failed "disc:boot - expected 6 different memory digests, got $rams ($(tail -1 "$work/disc-native.err"))"
	fi
fi

# ---- disc:install --------------------------------------------------------
# What a game's own data install costs the machine. A PS3 game that installs
# itself writes gigabytes onto /dev_hdd0, which is the machine's memory and so
# is every savestate; a file whose bytes are provably a stretch of the disc is
# held as a reference to the disc instead and costs nothing (memfs.cpp, "not
# carrying the disc twice"). This drives the loop an installer runs - read a
# disc file, write it to the hard disk, 64 KiB at a time - and then reads the
# result back and compares it with the disc, because a reference that answered
# with the wrong bytes would be a corruption nothing else here would catch.
#
# It is a STAND-IN and does not run a game's installer: what it does not cover
# is a game that transforms what it installs (unpacks an archive, say), for
# which no reference can stand and every byte is machine state. PLAN.md records
# what the real thing measured.
#
# Its own negative control is the second run: with the image read as it lies
# rather than as the console reads it, an encrypted disc's bytes match nothing.
# On a disc in the clear the two runs are the same run, so the control there is
# the leg for the encrypted one.
# copy_field FILE N: the Nth number on the runner's "disc copy" line, which the
# runner prints the moment the copy is done and nothing else has run since. The
# figures cannot be read at the END of a run: the emulator appends to its own
# log in the memory filesystem, and that growth would read as an install's cost.
copy_field() { sed -n 's/^disc copy [^:]*: \([0-9-]*\) bytes, \([0-9]*\) held, \([0-9]*\) kept, \([0-9]*\) decrypted.*/\'"$2"'/p' "$1"; }
disc_install_leg() {
	legname="$1"; image="$2"; shift 2
	run_both "$legname" 5 5 "$image" --disc-copy PS3_GAME/USRDIR/EBOOT.BIN "$@"
	copied="$(copy_field "$work/$legname-native-out.txt" 1)"
	held="$(copy_field "$work/$legname-native-out.txt" 2)"
	kept="$(copy_field "$work/$legname-native-out.txt" 3)"
	run_both "$legname-raw" 5 5 "$image" --disc-copy PS3_GAME/USRDIR/EBOOT.BIN --disc-copy-raw "$@"
	rawheld="$(copy_field "$work/$legname-raw-native-out.txt" 2)"
	rawkept="$(copy_field "$work/$legname-raw-native-out.txt" 3)"
	# and the sandbox reached the same verdict, which same_both cannot see: it
	# compares frame lines and the TTY, and this decision is in neither
	samecopy=1
	if [ "$have_wbx" = 1 ]; then
		grep '^disc copy' "$work/$legname-native-out.txt" > "$work/$legname-copy-native.txt"
		grep '^disc copy' "$work/$legname-wbx-out.txt" > "$work/$legname-copy-wbx.txt"
		[ -s "$work/$legname-copy-native.txt" ] && cmp -s "$work/$legname-copy-native.txt" "$work/$legname-copy-wbx.txt" || samecopy=0
	fi
}
if [ -z "$plaindisc" ]; then
	skip "disc:install - no decrypted .iso in tests/roms-local (would prove: a file the game copies off the disc onto the hard disk costs the machine nothing)"
else
	disc_install_leg dinst "$plaindisc"
	if [ -z "$copied" ] || [ "$copied" -le 0 ]; then
		failed "disc:install - the copy off $(basename "$plaindisc" | cut -c1-30) did not happen or did not read back as the disc's own bytes (probe said ${copied:-nothing}; $(tail -1 "$work/dinst-native.err"))"
	elif [ "$held" = "$copied" ] && [ "$kept" = 0 ]; then
		if same_memory dinst && [ "$samecopy" = 1 ]; then
			pass "disc:install - $(basename "$plaindisc" | cut -c1-30): $copied bytes copied off the disc onto the hard disk, read back byte for byte, and the machine carries none of them$(vs_memory)"
		else
			failed "disc:install - the sandbox decided differently or ran to different memory (diff $work/dinst-native.txt $work/dinst-wbx.txt, and $work/dinst-copy-native.txt)"
		fi
	else
		failed "disc:install - $copied bytes copied, but only $held held as the disc's and $kept kept in the machine"
	fi
fi
if [ -z "$encdisc" ]; then
	skip "disc:install:encrypted - no .iso with a .dkey beside it in tests/roms-local (would prove: a Redump image, whose data the ISO layer decrypts on the way past, costs the machine nothing either - and would be the negative control for the leg above)"
else
	disc_install_leg dinste "$encdisc" --dkey "${encdisc%.iso}.dkey"
	if ! grep -q "data regions are encrypted" "$work/dinste/RPCS3.log" 2>/dev/null; then
		failed "disc:install:encrypted - $(basename "$encdisc" | cut -c1-30) is not an encrypted image, or its key was refused: nothing to control against"
	elif [ -z "$copied" ] || [ "$copied" -le 0 ]; then
		failed "disc:install:encrypted - the copy did not happen or did not read back as the disc's own bytes (probe said ${copied:-nothing}; $(tail -1 "$work/dinste-native.err"))"
	elif [ "$rawheld" != 0 ] || [ "$rawkept" != "$copied" ]; then
		failed "disc:install:encrypted - the control did not fail: read as it lies the image should hold none of the $copied bytes, and it held $rawheld ($rawkept kept)"
	elif [ "$held" = "$copied" ] && [ "$kept" = 0 ]; then
		if same_memory dinste && same_memory dinste-raw && [ "$samecopy" = 1 ]; then
			pass "disc:install:encrypted - $(basename "$encdisc" | cut -c1-30): $copied bytes copied off a Redump image and read back byte for byte, the machine carrying none of them; read as it lies instead it carries all $rawkept$(vs_memory)"
		else
			failed "disc:install:encrypted - the sandbox decided differently or ran to different memory (diff $work/dinste-native.txt $work/dinste-wbx.txt, and $work/dinste-copy-native.txt)"
		fi
	else
		failed "disc:install:encrypted - $copied bytes copied, but only $held held as the disc's and $kept kept in the machine"
	fi
fi

# ---- disc:install:state --------------------------------------------------
# What a reference to the disc is worth to a TAS: it has to survive a savestate
# into ANOTHER PROCESS, which is what opening the project tomorrow is. The state
# carries the reference (it is guest memory like everything else here) but not
# the host's open handle on the image, nor anything derived from it. So: copy in
# one process, save at the last frame, and in a second process load that state
# and read the installed file back - against the disc, byte for byte.
if [ "$have_wbx" != 1 ]; then
	skip "disc:install:state - no sandboxed core (this question needs two processes)"
elif [ -z "$encdisc" ]; then
	skip "disc:install:state - no .iso with a .dkey beside it in tests/roms-local (would prove: a file held as a reference to an encrypted disc still reads back as the disc's own bytes after a savestate is loaded in another process)"
else
	st="$work/dinst-state.bin"
	"$wbx" "$core" --firmware "$pup" --dkey "${encdisc%.iso}.dkey" --frames 30 --report 30 \
		--disc-copy PS3_GAME/USRDIR/EBOOT.BIN --save-state "$st" "$encdisc" > "$work/dinsts1.txt" 2>"$work/dinsts1.err"
	"$wbx" "$core" --firmware "$pup" --dkey "${encdisc%.iso}.dkey" --frames 5 --report 5 \
		--state "$st" --disc-copy PS3_GAME/USRDIR/EBOOT.BIN --disc-verify "$encdisc" > "$work/dinsts2.txt" 2>"$work/dinsts2.err"
	wrote="$(copy_field "$work/dinsts1.txt" 1)"
	readback="$(copy_field "$work/dinsts2.txt" 1)"
	if [ -z "$wrote" ] || [ "$wrote" -le 0 ]; then
		failed "disc:install:state - the first process did not make the copy (said ${wrote:-nothing}; $(tail -1 "$work/dinsts1.err"))"
	elif [ ! -s "$st" ]; then
		failed "disc:install:state - no state was written ($(tail -1 "$work/dinsts1.err"))"
	elif [ "$readback" = "$wrote" ]; then
		pass "disc:install:state - $wrote bytes held as the disc's, saved in a $(wc -c < "$st") byte state, and read back byte for byte out of a state loaded in another process"
	else
		failed "disc:install:state - after the state was loaded in another process the installed file read back as ${readback:-nothing}, not the $wrote bytes written ($(tail -1 "$work/dinsts2.err"))"
	fi
fi

# ---- pkg:boot, pkg:content and pkg:licence -------------------------------
# A digital-only title was never pressed on a disc: its .pkg IS the game. Put
# in the Game slot it installs onto the console's hard disk before the machine
# starts, exactly as the console's own XMB installs it, and the machine boots
# the EBOOT.BIN the install produced - no disc and no disc key anywhere.
#
# Every .pkg in tests/roms-local (the user's, never committed) is tried in
# turn, with every .rap there handed over as a licence, and each one lands in
# one of the legs:
#   pkg:boot     a GAME package installs and the machine boots and runs what
#                it installed (whether the two flavors agree is reported, not
#                required - see the note at the leg)
#   pkg:content  a package that is not a game on its own is refused BY NAME
#                before the machine starts: content FOR a game (a DLC, a
#                licence, a theme), or an UPDATE, which says CATEGORY=HG and
#                installs a bootable EBOOT.BIN exactly as its game does and is
#                still only part of one
#   pkg:licence  a package that says REQUIRE_LICENSE boots with its .rap and
#                is refused, by a sentence naming the licence, without it
# Telling those apart is half of what this feature is, so they are legs.
raparg=""
for f in "$root"/tests/roms-local/*.rap; do
	[ -f "$f" ] && raparg="$raparg --rap $f"
done
pkggame=""
pkgcontent=""
pkglicensed=""
# smallest first, and stop as soon as every leg has its package: booting a
# package means installing it and running a real game, so the cheapest one
# that answers a question is the one to answer it with
find "$root/tests/roms-local" -maxdepth 1 -name '*.pkg' -printf '%s\t%p\n' 2>/dev/null | sort -n | cut -f2- > "$work/pkglist.txt"
while IFS= read -r f; do
	[ -f "$f" ] || continue
	[ -n "$pkggame" ] && [ -n "$pkgcontent" ] && [ -n "$pkglicensed" ] && break
	rm -rf "$work/pkgscan"
	# shellcheck disable=SC2086
	"$native" --work "$work/pkgscan" --firmware "$pup" $raparg --frames 120 --report 20 --tty-out "$work/tty-pkgscan.txt" "$f" 2>"$work/pkgscan.err" | grep '^frame\|^booted' > "$work/pkgscan.txt"
	if grep -q 'is not a game: installing it produced no USRDIR\|is a PATCH for a disc game\|is an UPDATE, not a game' "$work/pkgscan.err"; then
		if [ -z "$pkgcontent" ]; then
			pkgcontent="$f"
			cp "$work/pkgscan.err" "$work/pkgcontent.err"
		fi
	elif [ -s "$work/pkgscan.txt" ]; then
		if [ -z "$pkggame" ]; then
			pkggame="$f"
			cp "$work/pkgscan.txt" "$work/pkg-native.txt"
			cp "$work/tty-pkgscan.txt" "$work/tty-pkg-native.txt"
			cp "$work/pkgscan.err" "$work/pkg-native.err"
		fi
		# a package whose header says REQUIRE_LICENSE is the one worth asking
		# the licence question of; the others boot with or without a .rap
		if [ -z "$pkglicensed" ] && grep -aq 'Package Flags = .*REQUIRE_LICENSE' "$work/pkgscan/RPCS3.log" 2>/dev/null; then
			pkglicensed="$f"
		fi
	fi
done < "$work/pkglist.txt"
if [ -z "$pkggame" ]; then
	skip "pkg:boot - no digital-only game .pkg in tests/roms-local (would prove: a package installs onto the console's hard disk and the machine boots the title it installed, native == sandbox)"
else
	if [ "$have_wbx" = 1 ]; then
		# shellcheck disable=SC2086
		"$wbx" "$core" --firmware "$pup" $raparg --frames 120 --report 20 --tty-out "$work/tty-pkg-wbx.txt" "$pkggame" 2>"$work/pkg-wbx.err" | grep '^frame\|^booted' > "$work/pkg-wbx.txt"
	fi
	rams="$(distinct "$work/pkg-native.txt" ram 1)"
	# Whether the two flavors agree is REPORTED here and not required, and the
	# reason is measured rather than assumed: on titles that drive the SPUs the
	# two flavors' VIRTUAL clocks drift apart by microseconds, and a game that
	# measures elapsed time stores the difference (chimera#120). echochrome is
	# one - 35 bytes of frame-time floats out of 256 MiB, and it does not grow
	# with the run - and so are Saint Seiya and Arcana Heart 3, none of which
	# came from a package. Bejeweled 3, GTA San Andreas, Oblivion, Ultra Street
	# Fighter IV and Prince of Persia are byte-identical in both flavors.
	# Requiring it here would pin a core-wide matter on this feature. What this
	# leg is for is the package: that it installs onto the console's hard disk
	# and that the machine boots and RUNS what it installed. See docs/PLAN.md,
	# the .pkg entry.
	if [ "$rams" = 6 ]; then
		if same_both pkg; then
			agree="$(vs_sandbox)"
		else
			agree="the flavors' virtual clocks drift (chimera#120, not a package matter: see docs/PLAN.md)"
		fi
		pass "pkg:boot - $(basename "$pkggame" | cut -c1-40): the package installed and the machine booted what it installed, 120 frames, memory different at every report, $agree"
	else
		failed "pkg:boot - expected 6 different memory digests, got $rams ($(tail -1 "$work/pkg-native.err"))"
	fi
fi
if [ -z "$pkgcontent" ]; then
	skip "pkg:content - no .pkg in tests/roms-local that is content or an update (would prove: a package that is not a game on its own is refused by name, not installed into a console with part of a title on it)"
else
	pass "pkg:content - $(basename "$pkgcontent" | cut -c1-40): put in the Game slot it is refused - $(sed -n 's/^init failed: that .pkg \(is[^.,:]*\).*/a package that \1/p' "$work/pkgcontent.err" | head -1)"
fi
# The same licensed game again with no licence at all. A title whose header
# says REQUIRE_LICENSE is NPDRM: without the account's .rap its executable
# does not decrypt, and rpcs3 says only "Failed to decrypt content", which
# names neither the cause nor the cure.
#
# The refusal has to be a REFUSAL: the runner says the sentence and returns
# 1. It used to say the sentence and then die - SIGABRT in a static
# destructor, exit 134 - and this leg passed on the sentence alone
# (chimera#123; ~/chimera/docs/gates.md, mode B: nobody had watched it fail).
# So the exit code is part of the claim now, in both flavours, and once more
# as a precompile session, which is the shape the report came in: the
# frontend's precompile step opens the package on its own and is refused.
refused_cleanly() {
	# $1 name, $2 expected exit code, $3 what ran
	rc="$(cat "$work/$1.rc")"
	if ! grep -q 'licensed content and the project carries no licence' "$work/$1.err"; then
		if grep -q '^booted' "$work/$1-out.txt"; then
			failed "pkg:licence - $3 says REQUIRE_LICENSE but booted with no licence at all"
		else
			failed "pkg:licence - $3 failed without saying a licence was missing ($(tail -1 "$work/$1.err"))"
		fi
		return 1
	fi
	if [ "$rc" != "$2" ]; then
		failed "pkg:licence - $3 said which licence is missing and then exited $rc instead of $2 ($(grep -a 'fatal\|Aborted\|Segmentation' "$work/$1.err" | tail -1))"
		return 1
	fi
	return 0
}
if [ -z "$pkglicensed" ]; then
	skip "pkg:licence - no purchased .pkg (REQUIRE_LICENSE) with its .rap in tests/roms-local (would prove: such a title boots with its licence and names the missing licence without it, and returns rather than dying)"
else
	lic="$(basename "$pkglicensed" | cut -c1-40)"
	"$native" --work "$work/pkg-nolic" --firmware "$pup" --frames 1 --report 1 "$pkglicensed" > "$work/pkg-nolic-out.txt" 2>"$work/pkg-nolic.err"; echo $? > "$work/pkg-nolic.rc"
	rm -rf "$work/pkg-nolic-cache"
	"$native" --work "$work/pkg-nolic-pre" --firmware "$pup" --cache "$work/pkg-nolic-cache" --precompile 0/1 "$pkglicensed" > "$work/pkg-nolic-pre-out.txt" 2>"$work/pkg-nolic-pre.err"; echo $? > "$work/pkg-nolic-pre.rc"
	if [ "$have_wbx" = 1 ]; then
		"$wbx" "$core" --firmware "$pup" --frames 1 --report 1 "$pkglicensed" > "$work/pkg-nolic-wbx-out.txt" 2>"$work/pkg-nolic-wbx.err"; echo $? > "$work/pkg-nolic-wbx.rc"
	fi
	if refused_cleanly pkg-nolic 1 "$lic natively" && refused_cleanly pkg-nolic-pre 1 "$lic natively, as a precompile session," \
		&& { [ "$have_wbx" != 1 ] || refused_cleanly pkg-nolic-wbx 1 "$lic in the sandbox"; }; then
		pass "pkg:licence - $lic: boots with its .rap in the Licence slot, and without it says which licence is missing and returns 1 - as a run and as a precompile session, $(vs_sandbox)"
	fi
fi

# ---- ppu:llvm --------------------------------------------------------------
# lv2test on the LLVM PPU recompiler: liblv2 and the program compile inside
# each flavor when they load, and the machine that runs afterwards is the
# same one in both
# (each flavor fills its own compile cache on the way: cache:objects compares them)
rm -rf "$work/cache-native" "$work/cache-wbx"
CHIMERA_PPU_DECODER=llvm "$native" --work "$work/ppu-llvm" --firmware "$pup" --cache "$work/cache-native" --frames 200 --report 50 --tty-out "$work/tty-ppu-llvm-native.txt" "$rom" 2>"$work/ppu-llvm-native.err" | grep '^frame\|^booted' > "$work/ppu-llvm-native.txt"
if [ "$have_wbx" = 1 ]; then
	"$wbx" "$core" --firmware "$pup" --settings '{"ppu_decoder":"llvm"}' --cache "$work/cache-wbx" --frames 200 --report 50 --tty-out "$work/tty-ppu-llvm-wbx.txt" "$rom" 2>"$work/ppu-llvm-wbx.err" | grep '^frame\|^booted' > "$work/ppu-llvm-wbx.txt"
fi
if ! grep -q '^frame   200' "$work/ppu-llvm-native.txt"; then
	failed "ppu:llvm - the native run did not reach frame 200 ($(tail -1 "$work/ppu-llvm-native.err"))"
elif ! same_both ppu-llvm; then
	failed "ppu:llvm - the sandbox differs from native (diff $work/ppu-llvm-native.txt $work/ppu-llvm-wbx.txt)"
else
	pass "ppu:llvm - 200 frames of lv2test on the LLVM recompiler, $(wc -c < "$work/tty-ppu-llvm-native.txt") TTY bytes, $(vs_sandbox)"
fi

# ---- cache:objects and cache:warm ---------------------------------------
# The compiled objects each flavor stored are byte-identical (the same
# compiler, the same inputs: what lets a host-compiled object stand in for
# the guest's), and a second sandbox run loads them all and compiles nothing
if [ "$have_wbx" = 1 ]; then
	(cd "$work/cache-native" 2>/dev/null && find . -type f -exec sha1sum {} \; | sort) > "$work/cache-native.sha"
	(cd "$work/cache-wbx" 2>/dev/null && find . -type f -exec sha1sum {} \; | sort) > "$work/cache-wbx.sha"
	nobj="$(wc -l < "$work/cache-native.sha")"
	if [ "$nobj" -lt 3 ]; then
		failed "cache:objects - expected 3+ compiled objects stored natively, got $nobj ($(grep 'compile cache' "$work/ppu-llvm-native.err"))"
	elif cmp -s "$work/cache-native.sha" "$work/cache-wbx.sha"; then
		pass "cache:objects - $nobj compiled objects, byte-identical between the native and the sandbox compiler"
	else
		failed "cache:objects - the flavors' objects differ (diff $work/cache-native.sha $work/cache-wbx.sha)"
	fi
	"$wbx" "$core" --firmware "$pup" --settings '{"ppu_decoder":"llvm"}' --cache "$work/cache-wbx" --frames 200 --report 50 --tty-out "$work/tty-cache-warm.txt" "$rom" 2>"$work/cache-warm.err" | grep '^frame' > "$work/cache-warm.txt"
	warm="$(sed -n 's/^compile cache: \([0-9]*\) stored, \([0-9]*\) fetched.*/\1 stored \2 fetched/p' "$work/cache-warm.err")"
	if [ "$warm" = "0 stored $nobj fetched" ] && cmp -s "$work/cache-warm.txt" "$work/ppu-llvm-wbx-frames.txt" && cmp -s "$work/tty-cache-warm.txt" "$work/tty-ppu-llvm-wbx.txt"; then
		pass "cache:warm - the second sandbox run fetched all $nobj objects, compiled nothing, and ran the same machine"
	else
		failed "cache:warm - $warm (want 0 stored $nobj fetched) or the machine differed ($(tail -1 "$work/cache-warm.err"))"
	fi
fi

# ---- cache:precompile ----------------------------------------------------
# Two precompile sessions side by side, each taking its share of the parts
# (game scope: the executable and what it loads at boot), store between them
# exactly the objects a run compiles for those modules, byte-identical to the
# native compiler's; a run afterwards fetches them
if [ "$have_wbx" = 1 ]; then
	rm -rf "$work/cache-pre"
	"$wbx" "$core" --firmware "$pup" --settings '{"ppu_decoder":"llvm"}' --cache "$work/cache-pre" --precompile 0/2/game "$rom" > "$work/pre-0.txt" 2>&1 &
	"$wbx" "$core" --firmware "$pup" --settings '{"ppu_decoder":"llvm"}' --cache "$work/cache-pre" --precompile 1/2/game "$rom" > "$work/pre-1.txt" 2>&1 &
	wait
	stored0="$(sed -n 's/^compile cache: \([0-9]*\) stored.*/\1/p' "$work/pre-0.txt")"
	stored1="$(sed -n 's/^compile cache: \([0-9]*\) stored.*/\1/p' "$work/pre-1.txt")"
	(cd "$work/cache-pre" 2>/dev/null && find . -type f -exec sha1sum {} \; | sort) > "$work/cache-pre.sha"
	npre="$(wc -l < "$work/cache-pre.sha")"
	# every precompiled object is one the native run compiled, byte for byte
	missing="$(comm -23 "$work/cache-pre.sha" "$work/cache-native.sha" | wc -l)"
	"$wbx" "$core" --firmware "$pup" --settings '{"ppu_decoder":"llvm"}' --cache "$work/cache-pre" --frames 50 --report 50 "$rom" 2>"$work/pre-warm.err" | grep -c '^frame' > /dev/null
	fetched="$(sed -n 's/^compile cache: [0-9]* stored, \([0-9]*\) fetched.*/\1/p' "$work/pre-warm.err")"
	if [ "$npre" -ge 2 ] && [ "$((stored0 + stored1))" = "$npre" ] && [ "$stored0" -ge 1 ] && [ "$stored1" -ge 1 ] && [ "$missing" = 0 ] && [ "${fetched:-0}" = "$npre" ]; then
		pass "cache:precompile - two sessions stored $stored0 + $stored1 objects of the boot set, all byte-identical to native's, and a run fetched all $npre"
	else
		failed "cache:precompile - stored $stored0 + $stored1 (want $npre, both >= 1), $missing not native's, run fetched ${fetched:-0} ($(grep -a 'fatal\|vsched\|unhandled' "$work/pre-0.txt" "$work/pre-1.txt" | head -1 | cut -c1-120))"
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
			pass "spu:$dec - $answers SPU answers in 120 frames, $(vs_sandbox)"
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

# ---- rsx:drain -----------------------------------------------------------
# The RSX must never be what the game waits for. The core used to pin rpcs3's
# frame limiter to "auto", and handle_emu_flip serves that limiter by putting
# the RSX thread to sleep until its next flip slot - a sleep taken in the
# MACHINE's time here, on the thread that is also the FIFO puller. A game that
# queues a flip every vblank while its render thread waits on a fence got:
# sleep out the frame, flip, run one pushbuffer command, repeat (chimera issue
# #125: Mortal Kombat's get moved 18,124 bytes in 1500 frames, twelve bytes a
# frame, while 21,860 bytes sat unread; Injustice drained its buffer but
# presented eight frames in 1500).
#
# The rule is the game's PRESENT RATE: gcm label 255, the back-end label an
# Unreal Engine 3 game writes once per present, must have advanced at least
# once per four frames of the run (15 a second at 60). A starved RSX cannot
# meet it, whatever the buffer looks like: Mortal Kombat starved left its
# buffer 87% unread with the label at 37 in 1500 frames, and Injustice starved
# drained its buffer with the label at 8.
#
# It used to be two facts - the buffer fully read (put == get), and the label
# above one per hundred frames - which described a game at REST by frame 1500.
# Both reproducers were at rest, and for the wrong reason: stuck in UE3's SPU
# garbage-collection barrier (#125, closed by 274287a), which a drained buffer
# and a label of 39 passed. Once the barrier closed the game kept drawing, a
# drawing game is mid-buffer at any given frame (17,204 bytes in flight, label
# 7807), and the old leg failed the healthy machine. put and get are still
# reported, for whoever reads the result.
#
# The subject is whatever .iso the user put in tests/roms-local/rsx-drain/ (a
# symlink will do; both reproducers are UE3 games). Native only: the question
# is the RSX's, not the sandbox's.
drainiso=""
for f in "$root"/tests/roms-local/rsx-drain/*.iso; do
	[ -f "$f" ] && { drainiso="$f"; break; }
done
if [ -z "$drainiso" ]; then
	skip "rsx:drain - no .iso in tests/roms-local/rsx-drain/ (would prove: on a game whose render thread fences every present, the RSX reads everything the game submits and the fence label keeps moving - no frame limiter sleeps the FIFO puller)"
else
	drainkey=""
	[ -f "${drainiso%.iso}.dkey" ] && drainkey="--dkey ${drainiso%.iso}.dkey"
	# shellcheck disable=SC2086
	CHIMERA_DEBUG_THREADS=1 CHIMERA_PEEK=40300ff0:1 "$native" --work "$work/drain" --firmware "$pup" $drainkey --frames 1500 --report 500 --tty-out "$work/tty-drain-native.txt" "$drainiso" 2>"$work/drain-native.err" | grep '^frame\|^booted' > "$work/drain-native.txt"
	rsxline="$(grep 'RSX put=' "$work/drain-native.err" | head -1)"
	put="$(printf '%s' "$rsxline" | sed -n 's/.*put=\([0-9a-f]*\).*/\1/p')"
	get="$(printf '%s' "$rsxline" | sed -n 's/.*get=\([0-9a-f]*\).*/\1/p')"
	label="$(sed -n 's/^  40300ff0: \([0-9a-f]*\)$/\1/p' "$work/drain-native.err" | head -1)"
	labeln="$((0x${label:-0}))"
	if ! grep -q '^frame  1500' "$work/drain-native.txt"; then
		failed "rsx:drain - the run did not reach frame 1500 ($(tail -1 "$work/drain-native.err"))"
	elif [ -z "$put" ] || [ -z "$label" ]; then
		failed "rsx:drain - no RSX report or no label read at the end of the run (put='$put' label='$label')"
	elif [ "$labeln" -lt 375 ]; then
		failed "rsx:drain - $(basename "$drainiso"): its per-present label 255 moved only to $labeln in 1500 frames, less than once per four frames - the RSX or the game is stalled (put $put, get $get)"
	else
		pass "rsx:drain - $(basename "$drainiso"): its per-present label 255 stands at $labeln after 1500 frames (at least one per four frames required); put $put, get $get; native only"
	fi
fi

# ---- spu:barrier ---------------------------------------------------------
# A lock-free barrier needs two CPUs to be observed at once, and vsched runs
# one at a time for eighty thousand instructions.
#
# Unreal Engine 3's SPU garbage collector decrements a participant count and
# re-increments it about TEN instructions later. The PPU sleeps while
# decremented, waiting to be the last participant in. On hardware the other
# CPUs are running while it looks; here nobody is ever caught idle, the
# barrier never closes, and the game stops dead in its map load
# (chimera#125: Injustice and Mortal Kombat, both UE3).
#
# The fix gives way after a conditional store to a reservation slot whose
# previous writer was a different CPU. This leg asks whether the barrier
# closes, by the same fence label rsx:drain reads: stalled it stands at 39
# after 750 frames, and the game never gets past the load.
#
# NEGATIVE CONTROL, and it costs an environment variable rather than a second
# build: CHIMERA_SPU_YIELD=0 turns the give-way off in this very binary and
# the label returns to 39. Run by hand when the leg or the fix changes:
#   CHIMERA_SPU_YIELD=0 ... run-native --frames 750 ... inj.iso
# Measured 2026-09-21 on Injustice: off 39 (90 s), on 4735 (410 s). The extra
# time is the collector actually running; a game that never reaches this
# barrier pays nothing (Resident Evil 5, 300 frames: 125 s off, 124 s on).
if [ -z "$drainiso" ]; then
	skip "spu:barrier - no .iso in tests/roms-local/rsx-drain/ (would prove: a UE3 game's SPU garbage collector gets past its participant barrier, which needs one CPU to observe another's idle window)"
else
	barkey=""
	[ -f "${drainiso%.iso}.dkey" ] && barkey="--dkey ${drainiso%.iso}.dkey"
	# shellcheck disable=SC2086
	CHIMERA_PEEK=40300ff0:1 "$native" --work "$work/barrier" --firmware "$pup" $barkey --frames 750 --report 750 "$drainiso" 2>"$work/barrier.err" | grep '^frame' > "$work/barrier.txt"
	blabel="$(sed -n 's/^  40300ff0: \([0-9a-f]*\)$/\1/p' "$work/barrier.err" | head -1)"
	blabeln="$((0x${blabel:-0}))"
	if ! grep -q '^frame   750' "$work/barrier.txt"; then
		failed "spu:barrier - the run did not reach frame 750 ($(tail -1 "$work/barrier.err"))"
	elif [ -z "$blabel" ]; then
		failed "spu:barrier - no fence label read at the end of the run"
	elif [ "$blabeln" -le 1000 ]; then
		failed "spu:barrier - $(basename "$drainiso"): the fence label stands at $blabeln after 750 frames, which is where it sits when the SPU garbage collector's barrier never closes (39). The give-way after a contended conditional store is not reaching the machine"
	else
		pass "spu:barrier - $(basename "$drainiso"): the fence label stands at $blabeln after 750 frames, so the SPU garbage collector's participant barrier closes (it stalls at 39 with CHIMERA_SPU_YIELD=0); native only"
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
		if ! bridge_answered "$work/gflip-native.err" "$work/gflip-wbx.err"; then
			failed "gpu:flip - $bridge_gap"
		elif ! gpu_ran gflip; then
			failed "gpu:flip - $gpu_absent"
		elif [ -s "$work/gflip-got.txt" ] && cmp -s "$work/gflip-want.txt" "$work/gflip-got.txt"; then
			if same_both gflip; then
				pass "gpu:flip - $(grep 'gpu bridge: [0-9]' "$work/gflip-native.err" | head -1 | sed 's/gpu bridge: //' | cut -c1-60): 6 pictures identical to the machine's own pixels, $(vs_sandbox)"
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
	elif [ "${GATE_SKIP_GPU_DISC:-0}" != 0 ]; then
		# The sandbox GL run on the disc this machine holds never completes
		# frame 1: it faults guest pages in until the cgroup's OOM killer takes
		# run-wbx AND the gate's own shell with it, so the summary never prints
		# and every leg after this one is silently absent (docs/PLAN.md, the
		# GL_OP_CONTEXT_ID entry; ~/chimera/docs/gates.md, modes A and G).
		# Until that disc problem is solved a run that needs its summary asks
		# for a NAMED skip of the three legs rather than a cap nobody survives.
		skip "gpu:disc - GATE_SKIP_GPU_DISC set: the sandbox GL run on $(basename "$disc") is OOM-killed and takes the gate with it (docs/PLAN.md, 2026-09-21)"
		skip "gpu:rewind - the same run"
		skip "gpu:context - the same run"
	else
		run_both_gpu gdisc 120 20 "$disc"
		grep '^frame' "$work/disc-native.txt" | awk '{print $2, $4}' > "$work/gdisc-want.txt"
		grep '^frame' "$work/gdisc-native.txt" | awk '{print $2, $4}' > "$work/gdisc-got.txt"
		faults_native="$(sed -n 's/^page faults served by the renderer: //p' "$work/gdisc-native.err")"
		faults_wbx="$(sed -n 's/^page faults served by the renderer: //p' "$work/gdisc-wbx.err")"
		if ! bridge_answered "$work/gdisc-native.err" "$work/gdisc-wbx.err"; then
			failed "gpu:disc - $bridge_gap"
		elif ! gpu_ran gdisc; then
			failed "gpu:disc - $gpu_absent"
		elif [ -s "$work/gdisc-got.txt" ] && cmp -s "$work/gdisc-want.txt" "$work/gdisc-got.txt"; then
			if ! same_both gdisc; then
				failed "gpu:disc - the sandbox's GL run differs from the native GL run (diff $work/gdisc-native.txt $work/gdisc-wbx.txt)"
			elif [ -z "$faults_native" ] || [ "$have_wbx" = 1 ] && [ "$faults_native" != "$faults_wbx" ]; then
				failed "gpu:disc - the renderer's page watch served ${faults_native:-0} faults natively and ${faults_wbx:-0} in the sandbox (both must, equally)"
			else
				pass "gpu:disc - 120 frames on the GL renderer, memory identical to the null renderer's run, ${faults_native} page faults served by the renderer in each flavor, $(vs_sandbox)"
			fi
		else
			failed "gpu:disc - the GL run's memory differs from the null renderer's (diff $work/gdisc-want.txt $work/gdisc-got.txt; $(tail -1 "$work/gdisc-native.err"))"
		fi
		if [ "$have_wbx" = 1 ]; then
			if CHIMERA_GPU=1 "$wbx" "$core" --firmware "$pup" --settings '{"renderer":"opengl-hw"}' --frames 120 --rewind "$disc" > "$work/grewind.txt" 2>"$work/grewind.err"; then
				if ! bridge_answered "$work/grewind.err"; then
					failed "gpu:rewind - $bridge_gap"
				else
					pass "gpu:rewind - $(grep '^rewind' "$work/grewind.txt")"
				fi
			else
				failed "gpu:rewind - $(grep '^rewind' "$work/grewind.txt" || tail -1 "$work/grewind.err")"
			fi
		fi

		# The question rewind cannot ask, because it needs two PROCESSES: a
		# state saved by one session, loaded by another. The renderer's GL
		# objects are names a context handed out and they live in guest memory,
		# so they come back naming nothing; the driver refuses every call using
		# one and tells the guest nothing, and what used to happen is a machine
		# that runs and draws NOTHING (a black screen, then a crash - the
		# chimera issue this leg exists for). The renderer notices the context
		# is not its own and builds its objects again, so the picture after the
		# load is the picture before the save.
		if [ "$have_wbx" = 1 ]; then
			st="$work/context.state"
			rm -f "$st"
			if CHIMERA_GPU=1 "$wbx" "$core" --firmware "$pup" --settings '{"renderer":"opengl-hw"}' --frames 120 --report 120 --save-state "$st" "$disc" > "$work/gctx1.txt" 2>"$work/gctx1.err" \
				&& CHIMERA_GPU=1 "$wbx" "$core" --firmware "$pup" --settings '{"renderer":"opengl-hw"}' --frames 20 --report 20 --state "$st" "$disc" > "$work/gctx2.txt" 2>"$work/gctx2.err"; then
				before="$(grep '^frame' "$work/gctx1.txt" | tail -1 | sed 's/.*vid \([0-9x]*\) \([0-9a-f]*\).*/\1 \2/')"
				after="$(grep '^frame' "$work/gctx2.txt" | tail -1 | sed 's/.*vid \([0-9x]*\) \([0-9a-f]*\).*/\1 \2/')"
				blank="$(printf '%s' "$after" | grep -c '1920x1080' || true)"
				if ! bridge_answered "$work/gctx1.err" "$work/gctx2.err"; then
					failed "gpu:context - $bridge_gap"
				elif [ "$after" = "$before" ] && [ "$blank" = 0 ]; then
					pass "gpu:context - a state loaded in another process draws again ($after)"
				else
					failed "gpu:context - the picture after the load is '$after', the one before the save was '$before'"
				fi
			else
				failed "gpu:context - $(tail -1 "$work/gctx2.err" || tail -1 "$work/gctx1.err")"
			fi
			rm -f "$st"
		fi
	fi
fi

summary
