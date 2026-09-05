.globl f133
.p2align 2
f133:
	stp x29, x30, [sp, #-16]!
	bl _cf_qbe_run
	ldp x29, x30, [sp], #16
	ret

.globl f143
.p2align 2
f143:
	mov x8, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0
	ret

.globl f144
.p2align 2
f144:
	mov x0, #0
	ret

.globl f503
.p2align 2
f503:
	mov x0, x0
	mov x8, #94
	svc #0
	ret

.globl cf_root_page_init
.p2align 2
cf_root_page_init:
	movn x15, #4095
	mov x0, #0
	mov x1, #0x100000000
	mov x2, #0
	mov x3, #0x4022
	mov x4, #-1
	mov x5, #0
	mov x8, #222
	svc #0
	cmp x0, x15
	b.hs cf_mmap_fail
	mov x12, x0
	mov x0, x12
	mov x1, #0x10000000
	mov x2, #3
	mov x8, #226
	svc #0
	cmp x0, x15
	b.hs cf_mmap_fail
	adrp x9, cf_page
	add x9, x9, :lo12:cf_page
	str x12, [x9]
	mov x1, #0x100000000
	add x11, x12, x1
	str x11, [x9, #8]
	mov x1, #0x10000000
	add x13, x12, x1
	str x13, [x9, #24]
	mov x0, #0
	mov x1, #0x40000000
	mov x2, #0
	mov x3, #0x4022
	mov x4, #-1
	mov x5, #0
	mov x8, #222
	svc #0
	cmp x0, x15
	b.hs cf_mmap_fail
	mov x12, x0
	mov x0, x12
	mov x1, #0x1000000
	mov x2, #3
	mov x8, #226
	svc #0
	cmp x0, x15
	b.hs cf_mmap_fail
	str x12, [x9, #40]
	mov x1, #0x40000000
	add x11, x12, x1
	str x11, [x9, #48]
	mov x1, #0x1000000
	add x13, x12, x1
	str x13, [x9, #64]
	ret

.globl cf_root_page_grow
.p2align 2
cf_root_page_grow:
	movn x15, #4095
	add x9, x0, #24
	ldr x10, [x9]
	mov x11, #0x10000000
	sub x12, x11, #1
	add x13, x1, x12
	bic x14, x13, x12
	sub x1, x14, x10
	mov x0, x10
	mov x2, #3
	mov x8, #226
	svc #0
	cmp x0, x15
	b.hs cf_mmap_fail
	str x14, [x9]
	ret

.globl cf_mmap_fail
.p2align 2
cf_mmap_fail:
	mov x0, #71
	mov x8, #94
	svc #0

.globl cf_oom
.p2align 2
cf_oom:
	mov x0, #70
	mov x8, #94
	svc #0

.globl cf_oos
.p2align 2
cf_oos:
	mov x0, #72
	mov x8, #94
	svc #0

.globl cf_oob
.p2align 2
cf_oob:
	mov x0, #73
	mov x8, #94
	svc #0

.globl cf_oos_data
.p2align 2
cf_oos_data:
	.data
	.balign 8
cf_oos_win:
	.quad 1
	.quad 0
cf_oos_dflact:
	.skip 32
cf_oos_actbuf:
	.skip 32
	.bss
	.balign 16
cf_oos_stk:
	.skip 32768
	.text

.globl cf_oos_guard_init
.p2align 2
cf_oos_guard_init:
	stp x29, x30, [sp, #-64]!
	mov x29, sp
	mov x9, x0
	mov x0, #0
	mov x1, #3
	mov x2, #0
	add x3, sp, #16
	mov x8, #261
	svc #0
	ldr x10, [sp, #16]
	adrp x11, cf_oos_win
	add x11, x11, :lo12:cf_oos_win
	lsr x12, x10, #46
	cbnz x12, 2f
	sub x13, x9, x10
	mov x14, #524288
	sub x14, x13, x14
	str x14, [x11]
	mov x15, #2097152
	add x15, x13, x15
	str x15, [x11, #8]
2:
	adrp x12, cf_oos_stk
	add x12, x12, :lo12:cf_oos_stk
	str x12, [sp, #16]
	str xzr, [sp, #24]
	mov x13, #32768
	str x13, [sp, #32]
	add x0, sp, #16
	mov x1, #0
	mov x8, #132
	svc #0
	adrp x14, cf_oos_actbuf
	add x14, x14, :lo12:cf_oos_actbuf
	adrp x15, cf_oos_handler
	add x15, x15, :lo12:cf_oos_handler
	str x15, [x14]
	mov x13, #4
	movk x13, #0x0800, lsl #16
	str x13, [x14, #8]
	mov x0, #11
	mov x1, x14
	mov x2, #0
	mov x3, #8
	mov x8, #134
	svc #0
	ldp x29, x30, [sp], #64
	ret

.globl cf_oos_handler
.p2align 2
cf_oos_handler:
	ldr x9, [x1, #16]
	adrp x10, cf_oos_win
	add x10, x10, :lo12:cf_oos_win
	ldr x11, [x10]
	ldr x12, [x10, #8]
	cmp x9, x11
	b.lo 1f
	cmp x9, x12
	b.hi 1f
	b cf_oos
1:
	adrp x1, cf_oos_dflact
	add x1, x1, :lo12:cf_oos_dflact
	mov x2, #0
	mov x3, #8
	mov x8, #134
	svc #0
	ret

.globl _start
.p2align 2
_start:
	.weak __init_libc
	ldr x19, [sp]
	add x20, sp, #8
	add x21, x20, x19, lsl #3
	add x21, x21, #8
	adrp x2, :got:__init_libc
	ldr x2, [x2, :got_lo12:__init_libc]
	cbz x2, 1f
	mov x0, x21
	ldr x1, [x20]
	blr x2
1:
	bl cf_root_page_init
	adrp x0, cf_page
	add x0, x0, :lo12:cf_page
	mov x1, x19
	mov x2, x20
	bl cf_build_args
	mov x22, x0
	adrp x0, cf_page
	add x0, x0, :lo12:cf_page
	mov x1, x21
	bl cf_build_env
	mov x2, x0
	mov x1, x22
	adrp x0, cf_page
	add x0, x0, :lo12:cf_page
	bl main
	mov x8, #94
	svc #0

