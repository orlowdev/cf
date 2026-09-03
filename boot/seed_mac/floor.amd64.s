.globl _f133
.p2align 2
_f133:
	subq $8, %rsp
	call _cf_qbe_run
	addq $8, %rsp
	ret

.globl _f143
.p2align 2
_f143:
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

.globl _f144
.p2align 2
_f144:
	movq $1, %rax
	ret

.globl _f505
.p2align 2
_f505:
	movq $0x2000001, %rax
	syscall
	ret

.globl _f506
.p2align 2
_f506:
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

.globl _cf_oos
.p2align 2
_cf_oos:
	movq $72, %rdi
	movq $0x2000001, %rax
	syscall

.globl _cf_oob
.p2align 2
_cf_oob:
	movq $73, %rdi
	movq $0x2000001, %rax
	syscall

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
	pushq %rbp
	movq %rsp, %rbp
	pushq %rbx
	subq $56, %rsp
	movq %rdi, %rbx
	movl $3, %edi
	leaq 16(%rsp), %rsi
	call _getrlimit
	movq 16(%rsp), %r8
	leaq _cf_oos_win(%rip), %r9
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
	leaq _cf_oos_stk(%rip), %rcx
	movq %rcx, 16(%rsp)
	movq $32768, 24(%rsp)
	movl $0, 32(%rsp)
	leaq 16(%rsp), %rdi
	xorl %esi, %esi
	call _sigaltstack
	leaq _cf_oos_actbuf(%rip), %rbx
	leaq _cf_oos_handler(%rip), %rcx
	movq %rcx, (%rbx)
	movl $0x41, 12(%rbx)
	movl $11, %edi
	movq %rbx, %rsi
	xorl %edx, %edx
	call _sigaction
	movl $10, %edi
	movq %rbx, %rsi
	xorl %edx, %edx
	call _sigaction
	addq $56, %rsp
	popq %rbx
	popq %rbp
	ret

.globl _cf_oos_handler
.p2align 2
_cf_oos_handler:
	movq 24(%rsi), %r8
	leaq _cf_oos_win(%rip), %r9
	movq (%r9), %r10
	movq 8(%r9), %r11
	cmpq %r10, %r8
	jb 1f
	cmpq %r11, %r8
	ja 1f
	jmp _cf_oos
1:
	pushq %rbp
	movq %rsp, %rbp
	leaq _cf_oos_dflact(%rip), %rsi
	xorl %edx, %edx
	call _sigaction
	popq %rbp
	ret

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

