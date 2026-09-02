; padtest: a PPU program that reads the controller through the firmware's
; libio (cellPad, HLE) and reports it on the TTY every frame:
;   frame <n> pad <status> <digital1> <digital2> lx ly rx ry
; Needs the firmware installed (liblv2 LLE resolves nothing here, but the
; import table is the real PS3 shape and rpcs3 links HLE functions into it).
;
; lv2 syscalls: 141 sys_timer_usleep, 22 sys_process_exit, 403 sys_tty_write.
; Imports (sys_io): cellPadInit, cellPadGetData, cellPadEnd.

.opd _start, main_entry

main_entry:
	li r31, 0                ; frame counter

	; cellPadInit(7)
	li r3, 7
	la r12, stub_cellPadInit
	bl call_import

frame_loop:
	; cellPadGetData(0, &pad_data)
	li r3, 0
	la r4, pad_data
	la r12, stub_cellPadGetData
	bl call_import
	la r30, pad_status
	stw r3, 0(r30)

	; line: "frame " decimal(frame) " pad " hex32(status) " " hex32(digital1<<16|digital2) " " hex32(lx<<24|ly<<16|rx<<8|ry) "\n"
	la r3, line
	la r4, frame_text
	li r5, 6
	bl copy
	mr r4, r31
	bl decimal
	la r4, pad_text
	li r5, 5
	bl copy
	la r4, pad_status
	lwz r4, 0(r4)
	bl hex32
	li r4, 32
	stb r4, 0(r3)
	addi r3, r3, 1
	la r4, pad_data
	lhz r5, 8(r4)            ; button[2] = DIGITAL1 (CellPadData: s32 len, u16 button[64])
	lhz r6, 10(r4)           ; button[3] = DIGITAL2
	sldi r5, r5, 16
	or r4, r5, r6
	bl hex32
	li r4, 32
	stb r4, 0(r3)
	addi r3, r3, 1
	la r4, pad_data
	lhz r5, 12(r4)           ; button[4..7] = RX RY LX LY (u16 each at 4 + 2*i)
	lhz r6, 14(r4)
	lhz r7, 16(r4)
	lhz r8, 18(r4)
	sldi r5, r5, 24
	sldi r6, r6, 16
	sldi r7, r7, 8
	or r4, r5, r6
	or r4, r4, r7
	or r4, r4, r8
	bl hex32
	li r4, 10
	stb r4, 0(r3)
	addi r3, r3, 1
	la r4, line
	subf r5, r4, r3

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

	; cellPadEnd(); sys_process_exit(0)
	la r12, stub_cellPadEnd
	bl call_import
	li r11, 22
	li r3, 0
	sc

; ---- the PS3 import call: r12 = the stub table entry (the loader wrote the
; function descriptor's address there), LR already holds our return ----
call_import:
	mflr r0
	std r0, 8(r1)            ; PS3 ABI: LR save slot
	stdu r1, -128(r1)
	lwz r12, 0(r12)          ; descriptor
	lwz r0, 0(r12)           ; code address
	lwz r2, 4(r12)           ; TOC
	mtctr r0
	bctrl
	addi r1, r1, 128
	ld r0, 8(r1)
	mtlr r0
	blr

; ---- helpers: r3 = output cursor (advanced) ----
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

hex32:
	li r6, 8
	sldi r4, r4, 32
	la r7, hexdigits
	li r10, 0
hex_loop:
	rotldi r4, r4, 4
	andi. r8, r4, 15
	lbzx r8, r7, r8
	stbx r8, r3, r10
	addi r10, r10, 1
	cmpw r10, r6
	blt hex_loop
	add r3, r3, r6
	blr

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
	la r9, powers
	sldi r7, r10, 2
	lwzx r5, r9, r7
	cmpw r10, r6
	blt dec_loop
	add r3, r3, r6
	blr

; ---- data ----
.align 8
pad_status:   .long 0, 0
written:      .long 0, 0
pad_data:     .space 132           ; CellPadData: s32 len + u16 button[64]
powers:       .long 10000, 1000, 100, 10, 1, 1
hexdigits:    .ascii "0123456789abcdef"
frame_text:   .ascii "frame "
pad_text:     .ascii " pad "
.align 8
line:         .space 128

; ---- the import table (rpcs3: ppu_prx_module_info, 44 bytes) ----
.align 8
modname_sys_io: .ascii "sys_io\0"
.align 4
nids:
	.nid "cellPadInit"
	.nid "cellPadGetData"
	.nid "cellPadEnd"
stubs:
stub_cellPadInit:    .long 0
stub_cellPadGetData: .long 0
stub_cellPadEnd:     .long 0
.align 4
libstub_start:
	.byte 0x2c, 0                     ; size, unk0
	.byte 0, 1                        ; version 1
	.byte 0, 9                        ; attributes
	.byte 0, 3                        ; num_func
	.byte 0, 0                        ; num_var
	.byte 0, 0                        ; num_tlsvar
	.byte 0, 0, 0, 0                  ; info_hash, info_tlshash, unk1[2]
	.long modname_sys_io
	.long nids
	.long stubs
	.long 0, 0                        ; vnids, vstubs
	.long 0, 0                        ; unk4, unk5
libstub_end:
.prxparam libstub_start, libstub_end
