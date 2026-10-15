.section .text
.global _start
_start:
    xor %rbp,%rbp
    mov (%rsp),%rdi
    lea 8(%rsp),%rsi
    lea 16(%rsp,%rdi,8),%rdx
    mov %rdx,environ(%rip)
    and $-16,%rsp
    call main
    mov %eax,%edi
    call exit
.global __kario_sigreturn
__kario_sigreturn:
    mov %rsp,%rdi
    mov $43,%rax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
