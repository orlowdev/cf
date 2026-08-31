.globl f115
.p2align 2
f115:
	addi sp, sp, -16
	sd ra, 8(sp)
	call _cf_qbe_run
	ld ra, 8(sp)
	addi sp, sp, 16
	ret

.globl f119
.p2align 2
f119:
	mv a7, a0
	mv a0, a1
	mv a1, a2
	mv a2, a3
	mv a3, a4
	mv a4, a5
	mv a5, a6
	ecall
	ret

.globl f120
.p2align 2
f120:
	li a0, 2
	ret

.globl f408
.p2align 2
f408:
	mv a0, a0
	li a7, 94
	ecall
	ret

.globl cf_root_page_init
.p2align 2
cf_root_page_init:
	li t6, -4096
	li a0, 0
	li a1, 0x100000000
	li a2, 0
	li a3, 0x4022
	li a4, -1
	li a5, 0
	li a7, 222
	ecall
	bgeu a0, t6, cf_mmap_fail
	mv t0, a0
	mv a0, t0
	li a1, 0x10000000
	li a2, 3
	li a7, 226
	ecall
	bgeu a0, t6, cf_mmap_fail
	la t1, cf_page
	sd t0, 0(t1)
	li a1, 0x100000000
	add t2, t0, a1
	sd t2, 8(t1)
	li a1, 0x10000000
	add t3, t0, a1
	sd t3, 24(t1)
	li a0, 0
	li a1, 0x40000000
	li a2, 0
	li a3, 0x4022
	li a4, -1
	li a5, 0
	li a7, 222
	ecall
	bgeu a0, t6, cf_mmap_fail
	mv t0, a0
	mv a0, t0
	li a1, 0x1000000
	li a2, 3
	li a7, 226
	ecall
	bgeu a0, t6, cf_mmap_fail
	sd t0, 40(t1)
	li a1, 0x40000000
	add t2, t0, a1
	sd t2, 48(t1)
	li a1, 0x1000000
	add t3, t0, a1
	sd t3, 64(t1)
	ret

.globl cf_root_page_grow
.p2align 2
cf_root_page_grow:
	li t6, -4096
	addi t1, a0, 24
	ld t2, 0(t1)
	li t3, 0x10000000
	addi t4, t3, -1
	add t5, a1, t4
	not t0, t4
	and t5, t5, t0
	sub a1, t5, t2
	mv a0, t2
	li a2, 3
	li a7, 226
	ecall
	bgeu a0, t6, cf_mmap_fail
	sd t5, 0(t1)
	ret

.globl cf_mmap_fail
.p2align 2
cf_mmap_fail:
	li a0, 71
	li a7, 94
	ecall

.globl cf_oom
.p2align 2
cf_oom:
	li a0, 70
	li a7, 94
	ecall

.globl _start
.p2align 2
_start:
	.option push
	.option norelax
	lla gp, __global_pointer$
	.option pop
	.weak __init_libc
	ld s1, 0(sp)
	addi s2, sp, 8
	slli t0, s1, 3
	add t0, s2, t0
	addi s3, t0, 8
1:
	auipc t0, %got_pcrel_hi(__init_libc)
	ld t0, %pcrel_lo(1b)(t0)
	beqz t0, 2f
	mv a0, s3
	ld a1, 0(s2)
	jalr t0
2:
	call cf_root_page_init
	la a0, cf_page
	mv a1, s1
	mv a2, s2
	call cf_build_args
	mv s4, a0
	la a0, cf_page
	mv a1, s3
	call cf_build_env
	mv a2, a0
	mv a1, s4
	la a0, cf_page
	call main
	li a7, 94
	ecall

