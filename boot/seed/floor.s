.globl _start
.p2align 2
_start:
	bl _cf_build_args
	bl _main
	mov x16, #1
	svc #0x80
.globl _cf_arena_init
.globl _cf_grow
.globl _cf_oom
.p2align 2
_cf_arena_init:
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
	adrp x9, _cf_top@PAGE
	add x9, x9, _cf_top@PAGEOFF
	str x12, [x9]
	mov x1, #0x100000000
	add x11, x12, x1
	adrp x10, _cf_limit@PAGE
	add x10, x10, _cf_limit@PAGEOFF
	str x11, [x10]
	mov x0, x12
	mov x1, #0x10000000
	mov x2, #3
	mov x16, #74
	svc #0x80
	b.cs _cf_mmap_fail
	mov x1, #0x10000000
	add x13, x12, x1
	adrp x14, _cf_committed@PAGE
	add x14, x14, _cf_committed@PAGEOFF
	str x13, [x14]
	ret
_cf_grow:
	adrp x9, _cf_committed@PAGE
	add x9, x9, _cf_committed@PAGEOFF
	ldr x10, [x9]
	mov x11, #0x10000000
	sub x12, x11, #1
	add x13, x0, x12
	bic x14, x13, x12
	sub x1, x14, x10
	mov x0, x10
	mov x2, #3
	mov x16, #74
	svc #0x80
	b.cs _cf_mmap_fail
	str x14, [x9]
	ret
_cf_mmap_fail:
	mov x0, #71
	mov x16, #1
	svc #0x80
_cf_oom:
	mov x0, #70
	mov x16, #1
	svc #0x80
.globl _f1
.p2align 2
_f1:
	mov x16, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0x80
	ret

.globl _f15
.p2align 2
_f15:
	mov x0, x0
	mov x16, #1
	svc #0x80
	ret

