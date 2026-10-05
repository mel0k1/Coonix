#include "pit.h"
#include "kernel.h"
#include "idt.h"

static uint64_t ticks;

static void pit_irq(struct regs *r) {
    (void)r;
    ticks++;
}

void pit_init(uint32_t hz) {
    uint32_t div = 1193182 / hz;
    // divisor register is 16-bit: 0 means 65536, overflow silently
    // programs a wrong (faster) rate
    if (!div)
        div = 1;
    if (div > 0xffff)
        div = 0xffff;
    outb(0x43, 0x36);
    outb(0x40, div & 0xff);
    outb(0x40, (div >> 8) & 0xff);
    irq_install(0, pit_irq);
}

uint64_t pit_ticks(void) {
    return ticks;
}
