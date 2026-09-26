bits 64

extern kmain

section .text
global _start
_start:
    cli
    cld
    call kmain
.halt:
    hlt
    jmp .halt
