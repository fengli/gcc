	.file	"test.c"
	.text
	.globl	TREAD
	.type	TREAD, @function
TREAD:
.LFB0:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movl	-4(%rbp), %eax
	;;begin TREAD %rax, %rax
	clc
	cmovc	%rax,$-2147483648
	;;end TREAD
	movl	%eax, -8(%rbp)
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE0:
	.size	TREAD, .-TREAD
	.globl	TSCHEDULE
	.type	TSCHEDULE, @function
TSCHEDULE:
.LFB1:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movl	$6, -4(%rbp)
	movl	-4(%rbp), %edx
	movq	-16(%rbp), %rax
	;;begin TSCHEDULE %rax, %eax, %rdx
	clc;
	cmovc	 %eax, $65536;
	;;end TSCHEDULE
	movq	%rax, -24(%rbp)
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE1:
	.size	TSCHEDULE, .-TSCHEDULE
	.globl	TSTORE
	.type	TSTORE, @function
TSTORE:
.LFB2:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movl	$5, -4(%rbp)
	movl	$66, -8(%rbp)
	movl	-4(%rbp), %ecx
	movl	-8(%rbp), %eax
	movq	-16(%rbp), %rdx
	;;begin TSTORE %eax, %rdx, %ecx
	clc
	cmovc	%rdx, $-1877868544
	;;end TSTORE
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE2:
	.size	TSTORE, .-TSTORE
	.globl	TDESTROY
	.type	TDESTROY, @function
TDESTROY:
.LFB3:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	TDESTROY
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE3:
	.size	TDESTROY, .-TDESTROY
	.ident	"GCC: (GNU) 4.7.0 20110419 (experimental)"
	.section	.note.GNU-stack,"",@progbits
