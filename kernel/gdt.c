#include "gdt.h"
#include "kernel.h"
#include "string.h"

// layout: null | kcode | kdata | udata | ucode | tss(2 entries)
static uint64_t gdt[7];

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint32_t reserved1;
    uint64_t ist[7];
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
    uint8_t pad[8];       // total 104 bytes as cpu expects
} __attribute__((packed));

static struct tss tss;

extern void gdt_flush(uint64_t gdtr_ptr);
extern void tss_flush(void);

void tss_set_rsp0(uint64_t rsp) {
    tss.rsp0 = rsp;
}

// ist[idx] (0-based) = va; cpu switches to it when a gate names the slot
void tss_set_ist(int idx, uint64_t va) {
    tss.ist[idx] = va;
}

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    uint64_t e = 0;
    e |= (uint64_t)(limit & 0xffff);
    e |= (uint64_t)((base & 0xffff) << 16);
    e |= (uint64_t)((base >> 16) & 0xff) << 32;
    e |= (uint64_t)access << 40;
    e |= (uint64_t)(((limit >> 16) & 0x0f) | (gran & 0xf0)) << 48;
    e |= (uint64_t)((base >> 24) & 0xff) << 56;
    gdt[i] = e;
}

void gdt_init(void) {
    memset(&tss, 0, sizeof(tss));

    set_entry(0, 0, 0, 0, 0);                                  // null
    set_entry(1, 0, 0, 0x9a, 0x20);                            // kcode, L=1
    set_entry(2, 0, 0, 0x92, 0);                               // kdata
    set_entry(3, 0, 0, 0xf2, 0);                               // udata (DPL3)
    set_entry(4, 0, 0, 0xfa, 0x20);                            // ucode, L=1, DPL3

    uint64_t tss_base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;

    // tss descriptor spans gdt[5] and gdt[6]
    uint64_t lo = 0, hi = 0;
    lo |= (limit & 0xffff);
    lo |= (tss_base & 0xffff) << 16;
    lo |= ((tss_base >> 16) & 0xff) << 32;
    lo |= (uint64_t)0x89 << 40;          // present, TSS64
    lo |= ((limit >> 16) & 0x0f) << 48;
    lo |= ((tss_base >> 24) & 0xff) << 56;
    hi = (tss_base >> 32) & 0xffffffff;
    gdt[5] = lo;
    gdt[6] = hi;

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };

    // tss already loaded inside gdt_flush (ltr twice = #GP on busy tss)
    gdt_flush((uint64_t)&gdtr);
    outb(0xe9, 'E');
}
