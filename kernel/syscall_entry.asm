; native syscall entry (EFER.SCE + LSTAR): what real linux binaries use.
; syscall gives: rcx = rip, r11 = rflags, rsp still user.
; gs base always points at the per-task kernel scratch:
;   gs:0 = kernel stack top, gs:8 = user rsp scratch
global syscall_entry
extern syscall_dispatch

section .text
syscall_entry:
    cli                          ; no ticks until iretq restores IF: the
                                 ; entry must not be interrupted mid-frame
    mov qword [gs:8], rsp        ; stash user rsp
    mov rsp, [gs:0]              ; switch to the task kernel stack

    ; iret frame (matches struct regs tail)
    push qword 0x1b              ; ss = SEL_UDATA | 3
    push qword [gs:8]            ; user rsp
    push r11                     ; rflags (syscall put them here)
    push qword 0x23              ; cs = SEL_UCODE | 3
    push rcx                     ; rip

    ; int_no + err (nothing checks them on this path)
    push qword 0
    push qword 64

    ; gprs — same order isr_common uses: rax first ... r15 last
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    cld
    call syscall_dispatch
    mov rsp, rax                 ; dispatch returns the (possibly switched) frame rsp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                  ; drop int_no + err
    iretq
