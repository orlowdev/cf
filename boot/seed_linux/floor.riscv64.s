.globl f133
.p2align 2
f133:
	addi sp, sp, -16
	sd ra, 8(sp)
	call _cf_qbe_run
	ld ra, 8(sp)
	addi sp, sp, 16
	ret

.globl f143
.p2align 2
f143:
	mv a7, a0
	mv a0, a1
	mv a1, a2
	mv a2, a3
	mv a3, a4
	mv a4, a5
	mv a5, a6
	ecall
	ret

.globl f144
.p2align 2
f144:
	li a0, 2
	ret

.globl f503
.p2align 2
f503:
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

.globl cf_oos
.p2align 2
cf_oos:
	li a0, 72
	li a7, 94
	ecall

.globl cf_oob
.p2align 2
cf_oob:
	li a0, 73
	li a7, 94
	ecall

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
	addi sp, sp, -64
	sd ra, 56(sp)
	mv t0, a0
	li a0, 0
	li a1, 3
	li a2, 0
	addi a3, sp, 16
	li a7, 261
	ecall
	ld t1, 16(sp)
	lla t2, cf_oos_win
	srli t3, t1, 46
	bnez t3, 2f
	sub t4, t0, t1
	li t5, 524288
	sub t5, t4, t5
	sd t5, 0(t2)
	li t6, 2097152
	add t6, t4, t6
	sd t6, 8(t2)
2:
	lla t3, cf_oos_stk
	sd t3, 16(sp)
	sd zero, 24(sp)
	li t4, 32768
	sd t4, 32(sp)
	addi a0, sp, 16
	li a1, 0
	li a7, 132
	ecall
	lla t5, cf_oos_actbuf
	lla t6, cf_oos_handler
	sd t6, 0(t5)
	li t4, 0x08000004
	sd t4, 8(t5)
	li a0, 11
	mv a1, t5
	li a2, 0
	li a3, 8
	li a7, 134
	ecall
	ld ra, 56(sp)
	addi sp, sp, 64
	ret

.globl cf_oos_handler
.p2align 2
cf_oos_handler:
	ld t0, 16(a1)
	lla t1, cf_oos_win
	ld t2, 0(t1)
	ld t3, 8(t1)
	bltu t0, t2, 1f
	bgtu t0, t3, 1f
	j cf_oos
1:
	lla a1, cf_oos_dflact
	li a2, 0
	li a3, 8
	li a7, 134
	ecall
	ret

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

