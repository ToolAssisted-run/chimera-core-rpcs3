; rsrvtest: a reservation renewed while another CPU holds its line (chimera#151).
;
; Two PPU threads share one 128-byte line. The main thread increments a word
; of it with lwarx/stwcx.; the worker does nothing but store 0xAAAAAAAA into
; the word next to it - the store the interpreter's STW turns into
; vm::reservation_update (rpcs3's hack for Insomniac's engine), which waits
; for the line's unique lock.
;
; Run with accuratePpu128Reservations = -1, every stwcx. takes the line's
; unique lock and then, inside vm::writer_lock, waits for every other PPU to
; be seen waiting. On vsched that wait hands the machine over. The worker,
; whose slice ended on its store, runs that one store before anything else
; and meets the lock - and it must give way too, or the machine stops for
; good there (Guilty Gear Xrd stopped at its Stage 1 load this way).
;
; Every frame the main thread does 200 increments and prints one line:
;   frame <n> count <8 hex>
; and after 60 frames it exits. A machine that stops prints nothing more.
;
; lv2 syscalls used: 22 sys_process_exit, 52 sys_ppu_thread_create,
; 53 sys_ppu_thread_start, 141 sys_timer_usleep, 403 sys_tty_write.

.opd _start, main_entry
.opd worker_opd, worker_entry

main_entry:
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

	li r31, 0               ; r31 = frame
	li r29, 0               ; r29 = successful conditional stores
	la r28, cell
frame_loop:
	li r27, 0
inc_loop:
	lwarx r3, r0, r28
	addi r3, r3, 1
	stwcx. r3, r0, r28
	bne inc_loop            ; the reservation was lost: again
	addi r29, r29, 1
	addi r27, r27, 1
	cmpwi r27, 200
	blt inc_loop

	; text: "frame " + decimal(r31) + " count " + hex32(r29) + "\n"
	la r3, line
	la r4, frame_text
	li r5, 6
	bl copy
	mr r4, r31
	bl decimal
	la r4, count_text
	li r5, 7
	bl copy
	mr r4, r29
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
	cmpwi r31, 60
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

; worker: store 0xAAAAAAAA next to the counter, forever
worker_entry:
	la r6, cell
	lis r5, 0xAAAA
	ori r5, r5, 0xAAAA
worker_loop:
	stw r5, 4(r6)
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
worker_id:    .quad 0
written:      .long 0, 0
worker_param: .long worker_opd, 0
worker_name:  .ascii "rsrvtest worker\0"
.align 4
powers:       .long 10000, 1000, 100, 10, 1, 1
hexdigits:    .ascii "0123456789abcdef"
frame_text:   .ascii "frame "
count_text:   .ascii " count "
.align 8
line:         .space 128
; the shared line: the counter at +0, the worker's store at +4
.align 128
cell:         .space 128
