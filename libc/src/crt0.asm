bits 64

global _start
extern main

section .text
_start:
    ; sysv abi at entry: [rsp]=argc, [rsp+8]=argv..., then envp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    lea rdx, [rsi + rdi*8 + 8]
    and rsp, -16
    call main
    mov rdi, rax
    mov rax, 60        ; SYS_exit
    int 0x80
.halt:
    hlt
    jmp .halt
