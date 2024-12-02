.file	"switch_stack_linux.s"
.text
.globl	sysvmoc_switch_stack
.type	sysvmoc_switch_stack, @function
sysvmoc_switch_stack:
   movq %rsp, -16(%rdi)
   leaq -16(%rdi), %rsp
   movq %rdx, %rdi
   callq *%rsi
   mov (%rsp), %rsp
   retq
.size	sysvmoc_switch_stack, .-sysvmoc_switch_stack
.section	.note.GNU-stack,"",@progbits
