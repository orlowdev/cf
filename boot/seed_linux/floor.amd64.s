.globl f133
.p2align 2
f133:
	subq $8, %rsp
	call _cf_qbe_run
	addq $8, %rsp
	ret

.globl f143
.p2align 2
f143:
	movq %rdi, %rax
	movq %rsi, %rdi
	movq %rdx, %rsi
	movq %rcx, %rdx
	movq %r8, %r10
	movq %r9, %r8
	movq 8(%rsp), %r9
	syscall
	ret

.globl f144
.p2align 2
f144:
	movq $1, %rax
	ret

.globl f505
.p2align 2
f505:
	movl $231, %eax
	syscall
	ret

.globl cf_root_page_init
.p2align 2
cf_root_page_init:
	movq $0, %rdi
	movq $0x100000000, %rsi
	movq $0, %rdx
	movq $0x4022, %r10
	movq $-1, %r8
	movq $0, %r9
	movq $9, %rax
	syscall
	cmpq $-4096, %rax
	jae cf_mmap_fail
	movq %rax, %r15
	movq %r15, %rdi
	movq $0x10000000, %rsi
	movq $3, %rdx
	movq $10, %rax
	syscall
	cmpq $-4096, %rax
	jae cf_mmap_fail
	leaq cf_page(%rip), %rbx
	movq %r15, (%rbx)
	movq $0x100000000, %rax
	addq %r15, %rax
	movq %rax, 8(%rbx)
	leaq 0x10000000(%r15), %rax
	movq %rax, 24(%rbx)
	movq $0, %rdi
	movq $0x40000000, %rsi
	movq $0, %rdx
	movq $0x4022, %r10
	movq $-1, %r8
	movq $0, %r9
	movq $9, %rax
	syscall
	cmpq $-4096, %rax
	jae cf_mmap_fail
	movq %rax, %r15
	movq %r15, %rdi
	movq $0x1000000, %rsi
	movq $3, %rdx
	movq $10, %rax
	syscall
	cmpq $-4096, %rax
	jae cf_mmap_fail
	movq %r15, 40(%rbx)
	leaq 0x40000000(%r15), %rax
	movq %rax, 48(%rbx)
	leaq 0x1000000(%r15), %rax
	movq %rax, 64(%rbx)
	ret

.globl cf_root_page_grow
.p2align 2
cf_root_page_grow:
	leaq 24(%rdi), %r8
	movq (%r8), %r10
	movq $0x0fffffff, %r9
	leaq (%rsi,%r9), %rax
	notq %r9
	andq %rax, %r9
	movq %r9, %rsi
	subq %r10, %rsi
	movq %r10, %rdi
	movq $3, %rdx
	movq $10, %rax
	syscall
	cmpq $-4096, %rax
	jae cf_mmap_fail
	movq %r9, (%r8)
	ret

.globl cf_mmap_fail
.p2align 2
cf_mmap_fail:
	movq $71, %rdi
	movq $231, %rax
	syscall

.globl cf_oom
.p2align 2
cf_oom:
	movq $70, %rdi
	movq $231, %rax
	syscall

.globl cf_oos
.p2align 2
cf_oos:
	movq $72, %rdi
	movq $231, %rax
	syscall

.globl cf_oob
.p2align 2
cf_oob:
	movq $73, %rdi
	movq $231, %rax
	syscall

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
	pushq %rbp
	movq %rsp, %rbp
	pushq %rbx
	subq $56, %rsp
	movq %rdi, %rbx
	xorl %edi, %edi
	movl $3, %esi
	xorl %edx, %edx
	leaq 16(%rsp), %r10
	movl $302, %eax
	syscall
	movq 16(%rsp), %r8
	leaq cf_oos_win(%rip), %r9
	movq %r8, %rcx
	shrq $46, %rcx
	jnz 2f
	movq %rbx, %rcx
	subq %r8, %rcx
	leaq -524288(%rcx), %rdx
	movq %rdx, (%r9)
	leaq 2097152(%rcx), %rdx
	movq %rdx, 8(%r9)
2:
	leaq cf_oos_stk(%rip), %rcx
	movq %rcx, 16(%rsp)
	movq $0, 24(%rsp)
	movq $32768, 32(%rsp)
	leaq 16(%rsp), %rdi
	xorl %esi, %esi
	movl $131, %eax
	syscall
	leaq cf_oos_actbuf(%rip), %r9
	leaq cf_oos_handler(%rip), %rcx
	movq %rcx, (%r9)
	movq $0x0C000004, %rcx
	movq %rcx, 8(%r9)
	leaq cf_oos_restorer(%rip), %rcx
	movq %rcx, 16(%r9)
	movl $11, %edi
	movq %r9, %rsi
	xorl %edx, %edx
	movl $8, %r10d
	movl $13, %eax
	syscall
	addq $56, %rsp
	popq %rbx
	popq %rbp
	ret

.globl cf_oos_handler
.p2align 2
cf_oos_handler:
	movq 16(%rsi), %r8
	leaq cf_oos_win(%rip), %r9
	movq (%r9), %r10
	movq 8(%r9), %r11
	cmpq %r10, %r8
	jb 1f
	cmpq %r11, %r8
	ja 1f
	jmp cf_oos
1:
	leaq cf_oos_dflact(%rip), %rsi
	xorl %edx, %edx
	movl $8, %r10d
	movl $13, %eax
	syscall
	ret

.globl cf_oos_restorer
.p2align 2
cf_oos_restorer:
	movl $15, %eax
	syscall

.globl _start
.p2align 2
_start:
	.weak __init_libc
	movq (%rsp), %r12
	leaq 8(%rsp), %r13
	leaq 16(%rsp,%r12,8), %r14
	movq __init_libc@GOTPCREL(%rip), %rax
	testq %rax, %rax
	jz 1f
	movq %r14, %rdi
	movq (%r13), %rsi
	call *%rax
1:
	call cf_root_page_init
	leaq cf_page(%rip), %rdi
	movq %r12, %rsi
	movq %r13, %rdx
	call cf_build_args
	movq %rax, %r15
	leaq cf_page(%rip), %rdi
	movq %r14, %rsi
	call cf_build_env
	movq %rax, %rdx
	movq %r15, %rsi
	leaq cf_page(%rip), %rdi
	call main
	movq %rax, %rdi
	movq $231, %rax
	syscall

