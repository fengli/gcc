	.file	"test.c"
	.text
	.globl	TSCHEDULE
	.type	TSCHEDULE, @function
TSCHEDULE:
.LFB0:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	subq	$32, %rsp
	movl	$6, -4(%rbp)
	movl	$100, -8(%rbp)
	movl	-8(%rbp), %edx
	movl	-4(%rbp), %ecx
	movq	-16(%rbp), %rax
	movl	%ecx, %esi
	movq	%rax, %rdi
	movl	$0, %eax
	call	__builtin_tstar_create
	cltq
	movq	%rax, -24(%rbp)
	leave
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE0:
	.size	TSCHEDULE, .-TSCHEDULE
	.globl	TDESTROY
	.type	TDESTROY, @function
TDESTROY:
.LFB1:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	#begin %rax (fp) < TDESTROY
	prefetchnta	766443525(%rax,%rax,1)
	#destroy TDESTROY

	movq	%rax, -8(%rbp)
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE1:
	.size	TDESTROY, .-TDESTROY
	.globl	TLOAD
	.type	TLOAD, @function
TLOAD:
.LFB2:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	#begin %rax (fp) < TLOAD
	prefetchnta	766443529(%rax,%rax,1)
	#end TLOAD

	movq	%rax, -8(%rbp)
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE2:
	.size	TLOAD, .-TLOAD
	.globl	TCACHE
	.type	TCACHE, @function
TCACHE:
.LFB3:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movq	-8(%rbp), %rax
	#begin %rax (tfp) < TCACHE %rax (tid)
	prefetchnta	766443530(%rax,%rax,1)
	#end TCACHE

	movq	%rax, -16(%rbp)
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE3:
	.size	TCACHE, .-TCACHE
	.globl	DECREASE_N
	.type	DECREASE_N, @function
DECREASE_N:
.LFB4:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movq	-8(%rbp), %rax
	movq	-16(%rbp), %rdx
	#begin TDECREASEN %rax (fp), %rdx (n)
	prefetchnta	766443532(%rax,%rdx,1)
	#end TDECREASEN

	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE4:
	.size	DECREASE_N, .-DECREASE_N
	.ident	"GCC: (GNU) 4.7.0 20110419 (experimental)"
	.section	.note.GNU-stack,"",@progbits
