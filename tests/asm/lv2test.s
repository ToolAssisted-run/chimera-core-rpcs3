; lv2test: a PPU program that needs no firmware. It exercises the lv2
; scheduler the way a game does - two PPU threads, timers, memory - and
; leaves a trace in main memory and on the TTY that the gate hashes.
;
; Every frame (16666 us of machine time) the main thread advances an LCG,
; fills a page of allocated memory with it, and prints one line:
;   frame <n> state <16 hex> worker <8 hex>
; The worker thread wakes every millisecond and counts.
;
; lv2 syscalls used: 22 sys_process_exit, 41 sys_ppu_thread_exit,
; 53 sys_ppu_thread_start, 52 sys_ppu_thread_create, 141 sys_timer_usleep,
; 348 sys_memory_allocate, 403 sys_tty_write.

.opd _start, main_entry
.opd worker_opd, worker_entry

main_entry:
	; r31 = frame counter
	li r31, 0

	; sys_memory_allocate(0x10000, SYS_MEMORY_PAGE_SIZE_64K = 0x200, &page_addr)
	li r11, 348
	li r3, 1
	sldi r3, r3, 16
	li r4, 0x200
	la r5, page_addr
	sc
	cmpwi r3, 0
	bne fatal
	la r5, page_addr
	lwz r30, 0(r5)          ; r30 = allocated page (32-bit address)

	; sys_ppu_thread_create(&worker_id, &worker_param, arg=0, unk=0, prio=1000, stack=0x4000, flags=JOINABLE, &worker_name)
	li r11, 52
	la r3, worker_id
	la r4, worker_param
	li r5, 0
	li r6, 0
	li r7, 1000
	li r8, 0x4000
	li r9, 1
	la r10, worker_name
	sc
	cmpwi r3, 0
	bne fatal

	; sys_ppu_thread_start(worker_id)
	li r11, 53
	la r3, worker_id
	ld r3, 0(r3)
	sc
	cmpwi r3, 0
	bne fatal

frame_loop:
	; state = state * 6364136223846793005 + 1442695040888963407
	la r29, state
	ld r28, 0(r29)
	li64 r27, 0x5851F42D4C957F2D
	mulld r28, r28, r27
	li64 r27, 0x14057B7EF767814F
	add r28, r28, r27
	std r28, 0(r29)

	; fill 64 KiB of the allocated page with state ^ (offset * 0x9E3779B97F4A7C15)
	li r26, 0
	li64 r25, 0x9E3779B97F4A7C15
	mr r24, r28
fill_loop:
	add r23, r30, r26
	std r24, 0(r23)
	add r24, r24, r25
	addi r26, r26, 8
	li r23, 1
	sldi r23, r23, 16
	cmpd r26, r23
	blt fill_loop

	; text: "frame " + decimal(r31) + " state " + hex64(state) + " worker " + hex32(worker_count) + "\n"
	la r3, line
	la r4, frame_text
	li r5, 6
	bl copy
	mr r4, r31
	bl decimal
	la r4, state_text
	li r5, 7
	bl copy
	la r4, state
	ld r4, 0(r4)
	bl hex64
	la r4, worker_text
	li r5, 8
	bl copy
	la r4, worker_count
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

	; sys_timer_usleep(16666)
	li r11, 141
	li r3, 16666
	sc

	addi r31, r31, 1
	cmpwi r31, 30000
	blt frame_loop

	; sys_process_exit(0)
	li r11, 22
	li r3, 0
	sc

fatal:
	; a syscall failed: sys_process_exit(1)
	li r11, 22
	li r3, 1
	sc

; worker: count once per millisecond forever
worker_entry:
	la r31, worker_count
worker_loop:
	lwz r3, 0(r31)
	addi r3, r3, 1
	stw r3, 0(r31)
	li r11, 141
	li r3, 1000
	sc
	b worker_loop

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

; 16 hex digits of r4 (hex64) or 8 of a 32-bit r4 (hex32), most significant first
hex64:
	li r6, 16
	b hex_common
hex32:
	li r6, 8
	sldi r4, r4, 32
hex_common:
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
state:        .quad 0x243F6A8885A308D3
page_addr:    .long 0, 0
worker_id:    .quad 0
worker_count: .long 0, 0
written:      .long 0, 0
worker_param: .long worker_opd, 0
worker_name:  .ascii "lv2test worker\0"
.align 4
powers:       .long 10000, 1000, 100, 10, 1, 1
hexdigits:    .ascii "0123456789abcdef"
frame_text:   .ascii "frame "
state_text:   .ascii " state "
worker_text:  .ascii " worker "
.align 8
line:         .space 128
