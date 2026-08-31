.globl _f45
.p2align 2
_f45:
	stp x29, x30, [sp, #-16]!
	bl _cf_qbe_run
	ldp x29, x30, [sp], #16
	ret

.globl _f51
.p2align 2
_f51:
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

.globl _f328
.p2align 2
_f328:
	mov x0, x0
	mov x16, #1
	svc #0x80
	ret

.globl _f329
.p2align 2
_f329:
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

