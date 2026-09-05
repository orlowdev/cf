.globl _f133
.p2align 2
_f133:
	stp x29, x30, [sp, #-16]!
	bl _cf_qbe_run
	ldp x29, x30, [sp], #16
	ret

.globl _f143
.p2align 2
_f143:
	mov x16, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0x80
	b.cc 1f
	neg x0, x0
1:
	ret

.globl _f144
.p2align 2
_f144:
	mov x0, #0
	ret

.globl _f503
.p2align 2
_f503:
	mov x0, x0
	mov x16, #1
	svc #0x80
	ret

.globl _f504
.p2align 2
_f504:
	mov x16, #2
	svc #0x80
	b.cc 1f
	neg x0, x0
	ret
1:
	cbz x1, 2f
	mov x0, #0
2:
	ret

.globl _cf_root_page_init
.p2align 2
_cf_root_page_init:
	mov x0, #0
	mov x1, #0x100000000
	mov x2, #0
	mov x3, #0x1002
	mov x4, #-1
	mov x5, #0
	mov x16, #197
	svc #0x80
	b.cs _cf_mmap_fail
	mov x12, x0
	mov x0, x12
	mov x1, #0x10000000
	mov x2, #3
	mov x16, #74
	svc #0x80
	b.cs _cf_mmap_fail
	adrp x9, _cf_page@PAGE
	add x9, x9, _cf_page@PAGEOFF
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
	mov x3, #0x1002
	mov x4, #-1
	mov x5, #0
	mov x16, #197
	svc #0x80
	b.cs _cf_mmap_fail
	mov x12, x0
	mov x0, x12
	mov x1, #0x1000000
	mov x2, #3
	mov x16, #74
	svc #0x80
	b.cs _cf_mmap_fail
	str x12, [x9, #40]
	mov x1, #0x40000000
	add x11, x12, x1
	str x11, [x9, #48]
	mov x1, #0x1000000
	add x13, x12, x1
	str x13, [x9, #64]
	ret

.globl _cf_root_page_grow
.p2align 2
_cf_root_page_grow:
	add x9, x0, #24
	ldr x10, [x9]
	mov x11, #0x10000000
	sub x12, x11, #1
	add x13, x1, x12
	bic x14, x13, x12
	sub x1, x14, x10
	mov x0, x10
	mov x2, #3
	mov x16, #74
	svc #0x80
	b.cs _cf_mmap_fail
	str x14, [x9]
	ret

.globl _cf_mmap_fail
.p2align 2
_cf_mmap_fail:
	mov x0, #71
	mov x16, #1
	svc #0x80

.globl _cf_oom
.p2align 2
_cf_oom:
	mov x0, #70
	mov x16, #1
	svc #0x80

.globl _cf_oos
.p2align 2
_cf_oos:
	mov x0, #72
	mov x16, #1
	svc #0x80

.globl _cf_oob
.p2align 2
_cf_oob:
	mov x0, #73
	mov x16, #1
	svc #0x80

.globl _cf_oos_data
.p2align 2
_cf_oos_data:
	.data
	.balign 8
_cf_oos_win:
	.quad 1
	.quad 0
_cf_oos_dflact:
	.space 32
_cf_oos_actbuf:
	.space 32
	.zerofill __DATA,__bss,_cf_oos_stk,32768,3
	.text

.globl _cf_oos_guard_init
.p2align 2
_cf_oos_guard_init:
	stp x29, x30, [sp, #-64]!
	mov x29, sp
	str x19, [sp, #56]
	mov x19, x0
	mov x0, #3
	add x1, sp, #16
	bl _getrlimit
	ldr x9, [sp, #16]
	adrp x10, _cf_oos_win@PAGE
	add x10, x10, _cf_oos_win@PAGEOFF
	lsr x11, x9, #46
	cbnz x11, 2f
	sub x12, x19, x9
	mov x13, #524288
	sub x13, x12, x13
	str x13, [x10]
	mov x14, #2097152
	add x14, x12, x14
	str x14, [x10, #8]
2:
	adrp x11, _cf_oos_stk@PAGE
	add x11, x11, _cf_oos_stk@PAGEOFF
	str x11, [sp, #16]
	mov x12, #32768
	str x12, [sp, #24]
	str wzr, [sp, #32]
	add x0, sp, #16
	mov x1, #0
	bl _sigaltstack
	adrp x13, _cf_oos_actbuf@PAGE
	add x13, x13, _cf_oos_actbuf@PAGEOFF
	adrp x14, _cf_oos_handler@PAGE
	add x14, x14, _cf_oos_handler@PAGEOFF
	str x14, [x13]
	mov w15, #0x41
	str w15, [x13, #12]
	mov x0, #11
	mov x1, x13
	mov x2, #0
	bl _sigaction
	mov x0, #10
	mov x1, x13
	mov x2, #0
	bl _sigaction
	ldr x19, [sp, #56]
	ldp x29, x30, [sp], #64
	ret

.globl _cf_oos_handler
.p2align 2
_cf_oos_handler:
	ldr x9, [x1, #24]
	adrp x10, _cf_oos_win@PAGE
	add x10, x10, _cf_oos_win@PAGEOFF
	ldr x11, [x10]
	ldr x12, [x10, #8]
	cmp x9, x11
	b.lo 1f
	cmp x9, x12
	b.hi 1f
	b _cf_oos
1:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	adrp x1, _cf_oos_dflact@PAGE
	add x1, x1, _cf_oos_dflact@PAGEOFF
	mov x2, #0
	bl _sigaction
	ldp x29, x30, [sp], #16
	ret

.globl _start
.p2align 2
_start:
	mov x19, x0
	mov x20, x1
	mov x21, x2
	bl _cf_root_page_init
	adrp x0, _cf_page@PAGE
	add x0, x0, _cf_page@PAGEOFF
	mov x1, x19
	mov x2, x20
	bl _cf_build_args
	mov x22, x0
	adrp x0, _cf_page@PAGE
	add x0, x0, _cf_page@PAGEOFF
	mov x1, x21
	bl _cf_build_env
	mov x2, x0
	mov x1, x22
	adrp x0, _cf_page@PAGE
	add x0, x0, _cf_page@PAGEOFF
	bl _main
	mov x16, #1
	svc #0x80

