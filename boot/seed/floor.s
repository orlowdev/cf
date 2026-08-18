.globl _start
.p2align 2
_start:
	bl _cf_build_args
	bl _main
	mov x16, #1
	svc #0x80
.globl _cf_arena_init
.globl _cf_oom
.p2align 2
_cf_arena_init:
	mov x0, #0
	mov x1, #0x10000000
	mov x2, #3
	mov x3, #0x1002
	mov x4, #-1
	mov x5, #0
	mov x16, #197
	svc #0x80
	b.cs _cf_mmap_fail
	adrp x9, _cf_top@PAGE
	add x9, x9, _cf_top@PAGEOFF
	str x0, [x9]
	mov x1, #0x10000000
	add x11, x0, x1
	adrp x10, _cf_limit@PAGE
	add x10, x10, _cf_limit@PAGEOFF
	str x11, [x10]
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

