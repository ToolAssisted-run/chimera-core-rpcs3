; threadstart: a thread that was just started does not run before the thread
; that started it has gone on (chimera#332). On a PS3 a new thread takes
; microseconds to start, and a game may finish the object the thread uses
; after starting it: Oblivion starts its task manager's thread from inside
; the manager's constructor, and the thread aborted when it ran first.
;
; The main thread starts 984 threads, one after another. After each start it
; works on for about 3000 instructions (about a microsecond on a PS3) and
; then marks that thread as made. Each thread looks at its mark first thing
; and counts itself as early when the mark is not there yet. The main thread's
; turns end every so often wherever it happens to be, about a third of the
; time inside those 3000 instructions; the work between two starts changes
; from one start to the next, so the turns do not end in step with the
; starts. The threads are started 41 at a time and joined before the next 41
; (lv2 keeps at most 100 PPU threads, and runs two at once). When everything
; has run, one line goes to the TTY:
;   threadstart: started 00984 early 00000000
; and the program waits a frame at a time for ever. A create, start or join
; that is refused says so instead: threadstart: refused <error> at <index>.
;
; lv2 syscalls used: 22 sys_process_exit, 41 sys_ppu_thread_exit,
; 44 sys_ppu_thread_join, 52 sys_ppu_thread_create, 53 sys_ppu_thread_start,
; 141 sys_timer_usleep, 403 sys_tty_write.

.opd _start, main_entry
.opd worker_opd, worker_entry

main_entry:
	li r31, 0                ; threads started
	li r27, 0                ; 0..12, how much the work after a mark varies

batch:
	li r25, 0                ; index within the batch

start_loop:
	; r24 = &ids[r25]
	la r24, ids
	sldi r23, r25, 3
	add r24, r24, r23

	; sys_ppu_thread_create(&ids[r25], &worker_param, arg=index, unk=0, prio=3000, stack=0x4000, flags=JOINABLE, &worker_name)
	li r11, 52
	mr r3, r24
	la r4, worker_param
	mr r5, r31
	li r6, 0
	li r7, 3000
	li r8, 0x4000
	li r9, 1
	la r10, worker_name
	sc
	cmpwi r3, 0
	bne refused

	; sys_ppu_thread_start(ids[r25])
	li r11, 53
	ld r3, 0(r24)
	sc
	cmpwi r3, 0
	bne refused

	; the starter's own work before the thread is made: 1000 turns of 3
	li r29, 1000
made_wait:
	addi r29, r29, -1
	cmpwi r29, 0
	bgt made_wait

	; made[index] = 1
	la r4, made
	li r5, 1
	stbx r5, r4, r31

	; other work before the next start: 1800 + 111 * (0..12) turns of 3
	li r29, 1800
	li r26, 0
vary:
	cmpw r26, r27
	bge vary_done
	addi r29, r29, 111
	addi r26, r26, 1
	b vary
vary_done:
	addi r27, r27, 1
	cmpwi r27, 13
	blt pad_loop
	li r27, 0
pad_loop:
	addi r29, r29, -1
	cmpwi r29, 0
	bgt pad_loop

	addi r31, r31, 1
	addi r25, r25, 1
	cmpwi r25, 41
	blt start_loop

	; sys_ppu_thread_join(ids[i], &exit_status) for the batch
	li r25, 0
join_loop:
	li r11, 44
	la r3, ids
	sldi r23, r25, 3
	add r3, r3, r23
	ld r3, 0(r3)
	la r4, exit_status
	sc
	cmpwi r3, 0
	bne refused
	addi r25, r25, 1
	cmpwi r25, 41
	blt join_loop

	cmpwi r31, 984
	blt batch

	; let the last threads run: sys_timer_usleep(100000)
	li r11, 141
	la r3, settle
	lwz r3, 0(r3)
	sc

	; "threadstart: started " + decimal(started) + " early " + hex32(early) + "\n"
	la r3, line
	la r4, started_text
	li r5, 21
	bl copy
	mr r4, r31
	bl decimal
	la r4, early_text
	li r5, 7
	bl copy
	la r4, early
	lwz r4, 0(r4)
	bl hex32
	li r4, 10
	stb r4, 0(r3)
	addi r3, r3, 1
	la r4, line
	subf r5, r4, r3          ; r5 = length

	; sys_tty_write(0, line, len, &written)
	li r11, 403
	li r3, 0
	la r6, written
	sc

idle:
	; sys_timer_usleep(16666), for ever
	li r11, 141
	li r3, 16666
	sc
	b idle

refused:
	; a create or start was refused: "threadstart: refused " + hex32(error) + " at " + decimal(index)
	mr r20, r3
	la r3, line
	la r4, refused_text
	li r5, 21
	bl copy
	mr r4, r20
	bl hex32
	la r4, at_text
	li r5, 4
	bl copy
	mr r4, r31
	bl decimal
	li r4, 10
	stb r4, 0(r3)
	addi r3, r3, 1
	la r4, line
	subf r5, r4, r3
	li r11, 403
	li r3, 0
	la r6, written
	sc

fatal:
	; sys_process_exit(1)
	li r11, 22
	li r3, 1
	sc

; a started thread (r3 = its index): early when its mark is not there yet
worker_entry:
	la r4, made
	lbzx r5, r4, r3
	cmpwi r5, 0
	bne worker_done
	la r6, early
	lwz r7, 0(r6)
	addi r7, r7, 1
	stw r7, 0(r6)
worker_done:
	; sys_ppu_thread_exit(0)
	li r11, 41
	li r3, 0
	sc

; ---- helpers: r3 = output cursor (advanced), r4 = value / source ----

; copy r5 bytes from r4 to r3
copy:
	li r6, 0
copy_loop:
	cmpw r6, r5
	bge copy_done
	lbzx r7, r4, r6
	stbx r7, r3, r6
	addi r6, r6, 1
	b copy_loop
copy_done:
	add r3, r3, r5
	blr

; 8 hex digits of a 32-bit r4, most significant first
hex32:
	li r6, 8
	sldi r4, r4, 32
	la r7, hexdigits
	li r10, 0
hex_loop:
	rotldi r4, r4, 4         ; the top nibble becomes the low nibble
	andi. r8, r4, 15
	lbzx r8, r7, r8
	stbx r8, r3, r10
	addi r10, r10, 1
	cmpw r10, r6
	blt hex_loop
	add r3, r3, r6
	blr

; decimal of r4 (0..99999) as 5 digits
decimal:
	li r6, 5
	li r10, 0
	li r5, 10000
dec_loop:
	li r8, 0
dec_div:
	cmpw r4, r5
	blt dec_digit
	subf r4, r5, r4
	addi r8, r8, 1
	b dec_div
dec_digit:
	addi r8, r8, 48
	stbx r8, r3, r10
	addi r10, r10, 1
	; r5 = r5 / 10 by table
	la r9, powers
	sldi r7, r10, 2
	lwzx r5, r9, r7
	cmpw r10, r6
	blt dec_loop
	add r3, r3, r6
	blr

.align 8
exit_status:  .quad 0
ids:          .space 328
early:        .long 0, 0
written:      .long 0, 0
settle:       .long 100000, 0
worker_param: .long worker_opd, 0
worker_name:  .ascii "threadstart worker\0"
.align 4
powers:       .long 10000, 1000, 100, 10, 1, 1
hexdigits:    .ascii "0123456789abcdef"
started_text: .ascii "threadstart: started "
early_text:   .ascii " early "
refused_text: .ascii "threadstart: refused "
at_text:      .ascii " at "
.align 8
made:         .space 1024
line:         .space 128
