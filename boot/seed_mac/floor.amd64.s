.globl _f112
.p2align 2
_f112:
	subq $8, %rsp
	call _cf_qbe_run
	addq $8, %rsp
	ret

.globl _f116
.p2align 2
_f116:
	movq %rdi, %rax
	orq $0x2000000, %rax
	movq %rsi, %rdi
	movq %rdx, %rsi
	movq %rcx, %rdx
	movq %r8, %r10
	movq %r9, %r8
	movq 8(%rsp), %r9
	syscall
	jnc 1f
	negq %rax
1:
	ret

.globl _f117
.p2align 2
_f117:
	movq $1, %rax
	ret

.globl _f405
.p2align 2
_f405:
	movq $0x2000001, %rax
	syscall
	ret

.globl _f406
.p2align 2
_f406:
	movq $0x2000002, %rax
	syscall
	jnc 1f
	negq %rax
	ret
1:
	testl %edx, %edx
	jz 2f
	xorl %eax, %eax
2:
	ret

.globl _cf_root_page_init
.p2align 2
_cf_root_page_init:
	movq $0, %rdi
	movq $0x100000000, %rsi
	movq $0, %rdx
	movq $0x1002, %r10
	movq $-1, %r8
	movq $0, %r9
	movq $0x20000c5, %rax
	syscall
	jc _cf_mmap_fail
	movq %rax, %r15
	movq %r15, %rdi
	movq $0x10000000, %rsi
	movq $3, %rdx
	movq $0x200004a, %rax
	syscall
	jc _cf_mmap_fail
	leaq _cf_page(%rip), %rbx
	movq %r15, (%rbx)
	movq $0x100000000, %rax
	addq %r15, %rax
	movq %rax, 8(%rbx)
	leaq 0x10000000(%r15), %rax
	movq %rax, 24(%rbx)
	movq $0, %rdi
	movq $0x40000000, %rsi
	movq $0, %rdx
	movq $0x1002, %r10
	movq $-1, %r8
	movq $0, %r9
	movq $0x20000c5, %rax
	syscall
	jc _cf_mmap_fail
	movq %rax, %r15
	movq %r15, %rdi
	movq $0x1000000, %rsi
	movq $3, %rdx
	movq $0x200004a, %rax
	syscall
	jc _cf_mmap_fail
	movq %r15, 40(%rbx)
	leaq 0x40000000(%r15), %rax
	movq %rax, 48(%rbx)
	leaq 0x1000000(%r15), %rax
	movq %rax, 64(%rbx)
	ret

.globl _cf_root_page_grow
.p2align 2
_cf_root_page_grow:
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
	movq $0x200004a, %rax
	syscall
	jc _cf_mmap_fail
	movq %r9, (%r8)
	ret

.globl _cf_mmap_fail
.p2align 2
_cf_mmap_fail:
	movq $71, %rdi
	movq $0x2000001, %rax
	syscall

.globl _cf_oom
.p2align 2
_cf_oom:
	movq $70, %rdi
	movq $0x2000001, %rax
	syscall

.globl _start
.p2align 2
_start:
	movq %rdi, %r12
	movq %rsi, %r13
	movq %rdx, %r14
	call _cf_root_page_init
	leaq _cf_page(%rip), %rdi
	movq %r12, %rsi
	movq %r13, %rdx
	call _cf_build_args
	movq %rax, %r15
	leaq _cf_page(%rip), %rdi
	movq %r14, %rsi
	call _cf_build_env
	movq %rax, %rdx
	movq %r15, %rsi
	leaq _cf_page(%rip), %rdi
	call _main
	movq %rax, %rdi
	movq $0x2000001, %rax
	syscall

