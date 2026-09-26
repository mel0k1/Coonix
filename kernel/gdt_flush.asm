global gdt_flush
global tss_flush

bits 64

section .text

; reload segments from new gdt, rdi = gdtr pointer
gdt_flush:
    lgdt [rdi]
    mov dx, 0xe9
    mov al, 'A'
    out dx, al
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov al, 'B'
    out dx, al
    push 0x08
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov dx, 0xe9
    mov al, 'C'
    out dx, al
    mov ax, 0x28
    ltr ax
    mov al, 'D'
    out dx, al
    ret

; load TSS selector 0x28
tss_flush:
    mov ax, 0x28
    ltr ax
    ret
