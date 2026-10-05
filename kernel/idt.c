#include "idt.h"
#include "kernel.h"
#include "gdt.h"
#include "pmm.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "vmm.h"
#include "signal.h"

// private exception stacks, mapped once pmm is up (idt_init runs before)
#define IST_VA_BASE 0xffffa00000000000ULL
#define IST_PAGES 8

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

static void set_gate(int n, void *handler, uint8_t type_attr, uint8_t ist) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low = addr & 0xffff;
    idt[n].selector = SEL_KCODE;
    idt[n].ist = ist;
    idt[n].type_attr = type_attr;
    idt[n].offset_mid = (addr >> 16) & 0xffff;
    idt[n].offset_high = (addr >> 32) & 0xffffffff;
    idt[n].reserved = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_stub_table[i], 0x8e, 0);
    // syscall gate: DPL3 so user code can int 0x80
    set_gate(128, isr_stub_table[128], 0xee, 0);

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

// df/nmi/mc must not run on the interrupted stack: it may be the thing
// that faulted (df) or a half-switched scheduler frame (nmi). private
// ist stacks give the panic path a guaranteed-good rsp. runs after pmm
void idt_setup_ist(void) {
    static const struct { int vec, slot; } map[3] = {
        { 8, 1 }, { 2, 2 }, { 18, 3 }   // df, nmi, mc
    };
    for (int i = 0; i < 3; i++) {
        uint64_t va = IST_VA_BASE + i * IST_PAGES * PAGE_SIZE;
        for (int p = 0; p < IST_PAGES; p++) {
            void *pg = pmm_alloc();
            if (!pg)
                panic("idt: no mem for ist");
            vmm_map(vmm_kernel_pml4(), va + p * PAGE_SIZE, (uint64_t)pg,
                    VMM_PRESENT | VMM_WRITE | VMM_NX);
        }
        tss_set_ist(map[i].slot - 1, va + IST_PAGES * PAGE_SIZE);
        set_gate(map[i].vec, isr_stub_table[map[i].vec], 0x8e, map[i].slot);
    }
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
        uint64_t cr2, cr3_now;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_now));
        // cow faults are handled quietly
        if (vmm_page_fault(r, cr2))
            return (uint64_t)r;
        // lazy fill of file-backed mappings
        if (task_mmap_fault(r, cr2))
            return (uint64_t)r;
        // user-space fault (or kernel touching a bad user pointer):
        // a SIGSEGV handler gets it, default kills the task
        if ((r->cs & 3) && current->pid == 0) {
            extern void task_dump_switches(void);
            serial_puts("[ANOMALY user-fault cur=idle frame=");
            serial_puthex((uint64_t)r);
            serial_puts(" rip=");
            serial_puthex(r->rip);
            serial_puts(" rsp=");
            serial_puthex(r->rsp);
            serial_puts(" rax=");
            serial_puthex(r->rax);
            serial_puts(" rbx=");
            serial_puthex(r->rbx);
            serial_puts(" rdi=");
            serial_puthex(r->rdi);
            serial_puts(" intno=");
            serial_puthex(r->int_no);
            serial_puts(" cr3=");
            serial_puthex(cr3_now);
            serial_puts(" idlersp=");
            {
                extern struct task *current;
                serial_puthex(current->rsp);
            }
            serial_puts(" ");
            task_dump_switches();
            // full forensics: every live slot + the frame idle would resume
            for (int i = 0; i < TASK_MAX; i++) {
                struct task *t = &task_table[i];
                if (t->state == T_FREE && !t->kstack_top)
                    continue;
                static const char *stname[] =
                    { "FREE", "READY", "RUN ", "BLCK", "ZOMB" };
                serial_puts(" t");
                serial_puthex(t->pid);
                serial_puts(":");
                serial_puts(stname[t->state]);
                serial_puts(" rsp=");
                serial_puthex(t->rsp);
                serial_puts(" kst=");
                serial_puthex(t->kstack_top);
                serial_puts(" pml4=");
                serial_puthex(t->pml4);
                if (t->rsp) {
                    struct regs *fr = (struct regs *)t->rsp;
                    serial_puts(" fcs=");
                    serial_puthex(fr->cs);
                    serial_puts(" frip=");
                    serial_puthex(fr->rip);
                }
                serial_puts("\n");
            }
            serial_puts("[HALT for inspection]\n");
            for (;;)
                __asm__ volatile("cli; hlt");
        }
        if ((r->cs & 3) || cr2 < 0x800000000000ULL) {
            int have_handler = current->sigact[SIGSEGV].handler &&
                               current->sigact[SIGSEGV].handler != 1 &&
                               !(current->sig_mask & (1ULL << (SIGSEGV - 1)));
            if (have_handler) {
                signal_send_task(current, SIGSEGV);
                uint64_t fr;
                signal_deliver(r, &fr);
                return fr;
            }
            console_set_fg(0xff5555);
            console_puts("\nsegfault: pid ");
            print_dec(current->pid);
            console_puts(r->err & 2 ? " wrote 0x" : " read/exec 0x");
            print_hex64(cr2);
            console_puts(" rip 0x");
            print_hex64(r->rip);
            console_puts(" err ");
            print_hex64(r->err);
            console_puts("\n");
            console_set_fg(CONSOLE_FG);
            return task_exit_current_sig(SIGSEGV);
        }
        console_set_fg(0xff5555);
        console_puts("\npage fault in kernel at rip 0x");
        print_hex64(r->rip);
        console_puts(" cr2 0x");
        print_hex64(cr2);
        console_puts(" rsp 0x");
        print_hex64(r->rsp);
        console_puts("\n  cur pid ");
        print_dec(current->pid);
        console_puts(" kgs 0x");
        print_hex64(current->kgs);
        console_puts(" kt 0x");
        print_hex64(current->kstack_top);
        console_puts("\n");
        panic("page fault");
    }

    if (r->int_no < 32) {
        // user-mode program faults become signals where one exists
        if (r->cs & 3) {
            int sig = 0;
            if (r->int_no == 6)  sig = SIGILL;
            if (r->int_no == 0)  sig = SIGFPE;
            if (r->int_no == 3)  sig = SIGTRAP;
            if (r->int_no == 4)  sig = SIGFPE;
            if (sig && current->sigact[sig].handler &&
                current->sigact[sig].handler != 1 &&
                !(current->sig_mask & (1ULL << (sig - 1)))) {
                signal_send_task(current, sig);
                uint64_t fr;
                signal_deliver(r, &fr);
                return fr;
            }
        }
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        console_set_fg(0xff5555);
        console_puts("\nexception: ");
        console_puts(exc_names[r->int_no]);
        console_puts(" at rip 0x");
        print_hex64(r->rip);
        console_puts(" cr2 0x");
        print_hex64(cr2);
        console_puts("\n  frame rsp 0x");
        print_hex64((uint64_t)r);
        console_puts(" urip 0x");
        print_hex64(r->rip);
        console_puts(" cs 0x");
        print_hex64(r->cs);
        console_puts(" ss 0x");
        print_hex64(r->ss);
        console_puts("\n  u_rsp 0x");
        print_hex64(r->rsp);
        console_puts(" rfl 0x");
        print_hex64(r->rflags);
        console_puts(" rax 0x");
        print_hex64(r->rax);
        console_puts(" err 0x");
        print_hex64(r->err);
        console_puts("\n");
        panic("cpu exception");
    }

    if (r->int_no == 128) {
        extern uint64_t syscall_dispatch(struct regs *r);
        uint64_t fr = syscall_dispatch(r); // may switch tasks
        // red line: never iretq to ring 3 while the scheduler thinks the
        // idle task is current — that context belongs to nobody
        if (((struct regs *)fr)->cs & 3 && current->pid == 0) {
            extern void task_dump_switches(void);
            serial_puts("[REDLINE int80->user cur=idle fr=");
            serial_puthex(fr);
            serial_puts(" ");
            task_dump_switches();
            for (;;)
                __asm__ volatile("cli; hlt");
        }
        return fr;
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        if (r->int_no >= 40)
            outb(0xa0, 0x20); // slave eoi
        outb(0x20, 0x20);     // master eoi — must happen before task switch
    }

    if (irq_handlers[r->int_no]) {
        irq_handlers[r->int_no](r);
        // timer tick: deliver pending signals to the interrupted user
        // context, then preempt
        if (r->int_no == 32) {
            extern void futex_tick(void);
            extern void task_tick_wake(void);
            futex_tick();
            task_tick_wake();
            uint64_t fr;
            int act = signal_deliver(r, &fr);
            if (act == 2)
                return fr;   // signal killed us; it scheduled the next task
            if (act == 1) {
                // red line: the handler frame replaces ours — but only a
                // real task may resume to ring 3 here
                if (current->pid == 0) {
                    extern void task_dump_switches(void);
                    serial_puts("[REDLINE tick-signal cur=idle fr=");
                    serial_puthex(fr);
                    serial_puts(" ");
                    task_dump_switches();
                    for (;;)
                        __asm__ volatile("cli; hlt");
                }
                return fr;   // handler frame replaces ours, no preemption
            }
            // user-mode preemption: the tick may preempt user code only
            // when another task is ready; kernel-mode interruption still
            // runs its handler and returns into the interrupted task
            return task_schedule(fr);
        }
    }
    return (uint64_t)r;
}
