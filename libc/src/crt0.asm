bits 64

global _start
extern main

section .text
_start:
    call main
    mov rdi, rax
    mov rax, 60        ; SYS_exit
    int 0x80
.halt:
    hlt
    jmp .halt
