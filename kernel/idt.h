// IDT + exception/irq dispatch
#pragma once
#include <stdint.h>

struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void idt_setup_ist(void);   // df/nmi/mc private stacks, after pmm_init
void irq_install(int irq, irq_handler_t h);
void irq_uninstall(int irq);
