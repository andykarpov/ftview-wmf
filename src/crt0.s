
	.module crt0
	.globl	_main
	.globl	_main_dispatch
	.globl	_far_entry
	.globl	_wc_video_bank
	.globl	_far_gate
	.globl	_ret_sp

	.area	_HEADER (ABS)
	.org 	0x8000
	jp _main
	; Вход для вызова из видеобанка (far_call). Адрес одинаков в обоих
	; банках; в видеобанке far_entry — пустая функция, её никто не вызывает.
	jp _far_entry

; В обоих банках эти короткие шлюзы находятся по одним адресам. API 79
; переключает #8000, а следующая инструкция уже берётся из нового банка.
; Адреса main_dispatch и far_entry у каждого банка свои; общий входной кадр
; лежит в #BFE0..#BFF1.
	.area _BANKSWITCH (ABS)
	.org 0xBF80
_wc_video_bank:
	ld sp, (_ret_sp)
	; Логические адреса кадра совпадают, физические страницы различаются.
	; Переносим все 18 байтов через стек ядра, который API 79 не переключает.
	ld hl, #0xBFE0
	ld b, #9
00001$:
	ld e, (hl)
	inc hl
	ld d, (hl)
	inc hl
	push de
	djnz 00001$
	ld a, #9
	ex af, af'
	ld a, #79		; API 79 MNG8_PL (десятичный номер)
	call 0x6006
	ld hl, #0xBFF1
	ld b, #9
00002$:
	pop de
	ld (hl), d
	dec hl
	ld (hl), e
	dec hl
	djnz 00002$
	jp _main_dispatch

; far_entry of another bank: A — its page (0 images, 9 video, 10 I/O), DE —
; the parameter block in the window #C000 (the same in all banks), C — the
; page to come back to (far_call of each bank sets it). The result is only in
; the block: the switch back spoils A and DE. IX / IY are kept.
_far_gate:
	push ix
	push iy
	push bc
	push de
	ex af, af'
	ld a, #79		; API 79 MNG8_PL (decimal)
	call 0x6006
	pop hl
	push hl
	call 0x8003
	pop hl
	pop bc
	ld a, c
	ex af, af'
	ld a, #79		; API 79 MNG8_PL (decimal)
	call 0x6006
	pop iy
	pop ix
	ret

	;; Ordering of segments for the linker.
	.area	_HOME
	.area	_CODE
	.area	_INITIALIZER
	.area   _GSINIT
	.area   _GSFINAL

	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area   _BSS
	.area   _HEAP

	.area   _GSINIT
	.area   _GSFINAL

