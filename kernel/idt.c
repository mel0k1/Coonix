#include "idt.h"
#include "kernel.h"
#include "gdt.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "vmm.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

static struct idt_entry idt[256];
static irq_handler_t irq_handlers[256];

extern void *isr_stub_table[256];

static const char *exc_names[32] = {
    "division by zero", "debug", "NMI", "breakpoint", "overflow",
    "bound range", "invalid opcode", "no math coproc", "double fault",
    "reserved", "invalid TSS", "segment not present", "stack fault",
    "general protection fault", "page fault", "reserved", "x87 FPU error",
    "alignment check", "machine check", "SIMD FP", "virt exception",
    "control protection", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "hypervisor injection", "vmm communication",
    "security exception", "reserved"
};

static void set_gate(int n, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low = addr & 0xffff;
    idt[n].selector = SEL_KCODE;
    idt[n].ist = 0;
    idt[n].type_attr = type_attr;
    idt[n].offset_mid = (addr >> 16) & 0xffff;
    idt[n].offset_high = (addr >> 32) & 0xffffffff;
    idt[n].reserved = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_stub_table[i], 0x8e);
    // syscall gate: DPL3 so user code can int 0x80
    set_gate(128, isr_stub_table[128], 0xee);

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));

    // remap PIC to 0x20..0x2f
    outb(0x20, 0x11); io_wait();
    outb(0xa0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xa1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xa1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xa1, 0x01); io_wait();
    outb(0x21, 0x00);
    outb(0xa1, 0x00);
}

// irq numbering: 0 = PIT (vector 32), 1 = kbd (vector 33), ...
void irq_install(int irq, irq_handler_t h) {
    irq_handlers[irq + 32] = h;
}

void irq_uninstall(int irq) {
    irq_handlers[irq + 32] = 0;
}

static void print_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    buf[16] = 0;
    console_puts(buf);
    serial_puts(buf);
}

static void print_dec(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + v % 10; v /= 10; }
    console_puts(&buf[i]);
    serial_puts(&buf[i]);
}

// returns the (possibly switched) kernel rsp to iretq from
uint64_t isr_handler(struct regs *r) {
    if (r->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        // cow faults are handled quietly
        if (vmm_page_fault(r, cr2))
            return (uint64_t)r;
        // lazy fill of file-backed mappings
        if (task_mmap_fault(r, cr2))
            return (uint64_t)r;
        // user-space fault (or kernel touching a bad user pointer): kill task
        if ((r->cs & 3) || cr2 < 0x800000000000ULL) {
            console_set_fg(0xff5555);
            console_puts("\nsegfault: pid ");
            print_dec(current->pid);
            console_puts(" wrote 0x");
            print_hex64(cr2);
            console_puts("\n");
            console_set_fg(CONSOLE_FG);
            return task_exit_current(139); // 128 + SIGSEGV
        }
        console_set_fg(0xff5555);
        console_puts("\npage fault in kernel at rip 0x");
        print_hex64(r->rip);
        console_puts(" cr2 0x");
        print_hex64(cr2);
        console_puts("\n");
        panic("page fault");
    }

    if (r->int_no < 32) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        console_set_fg(0xff5555);
        console_puts("\nexception: ");
        console_puts(exc_names[r->int_no]);
        console_puts(" at rip 0x");
        print_hex64(r->rip);
        console_puts(" cr2 0x");
        print_hex64(cr2);
        console_puts("\n");
        panic("cpu exception");
    }

    if (r->int_no == 128) {
        extern uint64_t syscall_dispatch(struct regs *r);
        return syscall_dispatch(r); // may switch tasks
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        if (r->int_no >= 40)
            outb(0xa0, 0x20); // slave eoi
        outb(0x20, 0x20);     // master eoi — must happen before task switch
    }

    if (irq_handlers[r->int_no]) {
        irq_handlers[r->int_no](r);
        // timer tick: preempt, schedule() picks another task if any
        if (r->int_no == 32) {
            extern uint64_t task_schedule(uint64_t old_rsp);
            return task_schedule((uint64_t)r);
        }
    }
    return (uint64_t)r;
}
