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
# Run from anywhere; artifacts land in waterbox/work/gate.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$here/work/gate"
rom="$root/tests/roms/lv2test.elf"
frames="${FRAMES:-600}"
native="$here/obj-native/run-native"
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
wbx="$here/bin/run-wbx"
core="$here/bin/core.wbx"
if [ ! -x "$wbx" ] || [ ! -f "$core" ]; then
	skip "sandbox legs: build-core.sh has not produced bin/core.wbx + bin/run-wbx"
	# ---- firmware:lle (needs the user's PS3UPDAT.PUP) -----------------------
pup="$root/tests/roms-local/PS3UPDAT.PUP"
if [ ! -f "$pup" ]; then
	skip "firmware:lle - no tests/roms-local/PS3UPDAT.PUP (would prove: the PUP installs in the box and liblv2 runs LLE)"
else
	"$native" --work "$work/fw" --firmware "$pup" --frames "$((frames / 3))" --report 50 --tty-out "$work/tty-fw-native.txt" "$rom" 2>"$work/fw-native.err" | grep '^frame\|^booted' > "$work/fw-native.txt"
	"$wbx" "$core" --firmware "$pup" --frames "$((frames / 3))" --report 50 --tty-out "$work/tty-fw-wbx.txt" "$rom" 2>"$work/fw-wbx.err" | grep '^frame\|^booted' > "$work/fw-wbx.txt"
	if ! grep -q "firmware 4" "$work/fw-native.txt"; then
		failed "firmware:lle - the firmware did not install natively ($(tail -1 "$work/fw-native.err"))"
	elif grep '^frame' "$work/fw-native.txt" > "$work/fw-native-frames.txt" && grep '^frame' "$work/fw-wbx.txt" > "$work/fw-wbx-frames.txt" && cmp -s "$work/fw-native-frames.txt" "$work/fw-wbx-frames.txt" && cmp -s "$work/tty-fw-native.txt" "$work/tty-fw-wbx.txt"; then
		pass "firmware:lle - $(grep '^booted' "$work/fw-native.txt" | sed 's/booted; //'), liblv2 LLE, native == sandbox at $((frames / 3)) frames"
	else
		failed "firmware:lle - sandbox differs from native with firmware (diff $work/fw-native.txt $work/fw-wbx.txt)"
	fi
fi

# ---- input:press (needs the firmware too: cellPad is libio) --------------
padrom="$root/tests/roms/padtest.elf"
[ -f "$padrom" ] || python3 "$root/tests/asm/ppc.py" "$root/tests/asm/padtest.s" "$padrom" >/dev/null
if [ ! -f "$pup" ]; then
	skip "input:press - no firmware (would prove: a scripted press reaches cellPadGetData on the right frames)"
else
	"$native" --work "$work/pad" --firmware "$pup" --frames 40 --report 40 --press 10:5:10 --tty-out "$work/pad-native.txt" "$padrom" 2>"$work/pad-native.err" | grep '^frame' > "$work/pad-native-frame.txt"
	"$wbx" "$core" --firmware "$pup" --frames 40 --report 40 --press 10:5:10 --tty-out "$work/pad-wbx.txt" "$padrom" 2>"$work/pad-wbx.err" | grep '^frame' > "$work/pad-wbx-frame.txt"
	pressed="$(grep -c ' 00000040 ' "$work/pad-native.txt")"
	first="$(grep -n ' 00000040 ' "$work/pad-native.txt" | head -1 | cut -d: -f1)"
	if [ "$pressed" = 5 ] && [ "$first" = 11 ] && grep -q ' lag 0 ' "$work/pad-native-frame.txt"; then
		if cmp -s "$work/pad-native.txt" "$work/pad-wbx.txt" && cmp -s "$work/pad-native-frame.txt" "$work/pad-wbx-frame.txt"; then
			pass "input:press - Cross seen on exactly the 5 pressed frames (first at line $first), lag 0, native == sandbox"
		else
			failed "input:press - the sandbox's pad trace differs from native (diff $work/pad-native.txt $work/pad-wbx.txt)"
		fi
	else
		failed "input:press - expected 5 pressed lines starting at line 11 with lag 0, got $pressed from line ${first:-none} ($(tail -1 "$work/pad-native-frame.txt"))"
	fi
	# a program that never polls the pad is lag on every frame
	if grep -q ' lag 200 ' "$work/fw-native-frames.txt" 2>/dev/null || grep -q " lag $((frames / 3)) " "$work/fw-native.txt"; then
		pass "input:lag - a program that never polls counts every frame as lag"
	else
		failed "input:lag - lv2test should be all lag ($(tail -1 "$work/fw-native.txt"))"
	fi
fi

exit $fail
fi

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

# ---- firmware:lle (needs the user's PS3UPDAT.PUP) -----------------------
pup="$root/tests/roms-local/PS3UPDAT.PUP"
if [ ! -f "$pup" ]; then
	skip "firmware:lle - no tests/roms-local/PS3UPDAT.PUP (would prove: the PUP installs in the box and liblv2 runs LLE)"
else
	"$native" --work "$work/fw" --firmware "$pup" --frames "$((frames / 3))" --report 50 --tty-out "$work/tty-fw-native.txt" "$rom" 2>"$work/fw-native.err" | grep '^frame\|^booted' > "$work/fw-native.txt"
	"$wbx" "$core" --firmware "$pup" --frames "$((frames / 3))" --report 50 --tty-out "$work/tty-fw-wbx.txt" "$rom" 2>"$work/fw-wbx.err" | grep '^frame\|^booted' > "$work/fw-wbx.txt"
	if ! grep -q "firmware 4" "$work/fw-native.txt"; then
		failed "firmware:lle - the firmware did not install natively ($(tail -1 "$work/fw-native.err"))"
	elif grep '^frame' "$work/fw-native.txt" > "$work/fw-native-frames.txt" && grep '^frame' "$work/fw-wbx.txt" > "$work/fw-wbx-frames.txt" && cmp -s "$work/fw-native-frames.txt" "$work/fw-wbx-frames.txt" && cmp -s "$work/tty-fw-native.txt" "$work/tty-fw-wbx.txt"; then
		pass "firmware:lle - $(grep '^booted' "$work/fw-native.txt" | sed 's/booted; //'), liblv2 LLE, native == sandbox at $((frames / 3)) frames"
	else
		failed "firmware:lle - sandbox differs from native with firmware (diff $work/fw-native.txt $work/fw-wbx.txt)"
	fi
fi

# ---- input:press (needs the firmware too: cellPad is libio) --------------
padrom="$root/tests/roms/padtest.elf"
[ -f "$padrom" ] || python3 "$root/tests/asm/ppc.py" "$root/tests/asm/padtest.s" "$padrom" >/dev/null
if [ ! -f "$pup" ]; then
	skip "input:press - no firmware (would prove: a scripted press reaches cellPadGetData on the right frames)"
else
	"$native" --work "$work/pad" --firmware "$pup" --frames 40 --report 40 --press 10:5:10 --tty-out "$work/pad-native.txt" "$padrom" 2>"$work/pad-native.err" | grep '^frame' > "$work/pad-native-frame.txt"
	"$wbx" "$core" --firmware "$pup" --frames 40 --report 40 --press 10:5:10 --tty-out "$work/pad-wbx.txt" "$padrom" 2>"$work/pad-wbx.err" | grep '^frame' > "$work/pad-wbx-frame.txt"
	pressed="$(grep -c ' 00000040 ' "$work/pad-native.txt")"
	first="$(grep -n ' 00000040 ' "$work/pad-native.txt" | head -1 | cut -d: -f1)"
	if [ "$pressed" = 5 ] && [ "$first" = 11 ] && grep -q ' lag 0 ' "$work/pad-native-frame.txt"; then
		if cmp -s "$work/pad-native.txt" "$work/pad-wbx.txt" && cmp -s "$work/pad-native-frame.txt" "$work/pad-wbx-frame.txt"; then
			pass "input:press - Cross seen on exactly the 5 pressed frames (first at line $first), lag 0, native == sandbox"
		else
			failed "input:press - the sandbox's pad trace differs from native (diff $work/pad-native.txt $work/pad-wbx.txt)"
		fi
	else
		failed "input:press - expected 5 pressed lines starting at line 11 with lag 0, got $pressed from line ${first:-none} ($(tail -1 "$work/pad-native-frame.txt"))"
	fi
	# a program that never polls the pad is lag on every frame
	if grep -q ' lag 200 ' "$work/fw-native-frames.txt" 2>/dev/null || grep -q " lag $((frames / 3)) " "$work/fw-native.txt"; then
		pass "input:lag - a program that never polls counts every frame as lag"
	else
		failed "input:lag - lv2test should be all lag ($(tail -1 "$work/fw-native.txt"))"
	fi
fi

exit $fail
