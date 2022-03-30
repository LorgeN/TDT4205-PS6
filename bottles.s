.globl main

.section .rodata
.STR0: 
	.string "%ld green bottles hanging on the wall,\n"
.STR1: 
	.string "and if 1 green bottle should accidentally fall,\n"
.STR2: 
	.string "there'd be %ld green bottles hanging on the wall.\n"

.section .text
main:
	pushq %r12
	movq $10, %r12

main_wprint:
	movq $.STR0, %rdi
	movq %r12, %rsi
	call printf

	movq $.STR0, %rdi
	movq %r12, %rsi
	call printf

	movq $.STR1, %rdi
	call printf

	subq $1, %r12

	movq $.STR2, %rdi
	movq %r12, %rsi
	call printf

	cmp $0, %r12
	jg main_wprint

	popq %r12
	movq $0, %rax

	ret
 