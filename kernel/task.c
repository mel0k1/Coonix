#include "task.h"
#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "string.h"
#include "serial.h"
#include "elf.h"
#include "pit.h"
#include "gdt.h"

struct task task_table[TASK_MAX];
struct task *current;

// embedded user programs (objcopy'ed, see Makefile)
extern const uint8_t _binary_shell_elf_start[];
extern const uint8_t _binary_shell_elf_end[];
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];

static struct prog {
    const char *name;
    const uint8_t *start, *end;
} progs[] = {
    { "shell", _binary_shell_elf_start, _binary_shell_elf_end },
    { "hello", _binary_hello_elf_start, _binary_hello_elf_end },
    { 0, 0, 0 }
};

static const uint8_t *prog_find(const char *name, uint64_t *size) {
    for (struct prog *p = progs; p->name; p++)
        if (!strcmp(p->name, name)) {
            *size = p->end - p->start;
            return p->start;
        }
    return 0;
}

struct task *task_find_free(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state == T_FREE)
            return &task_table[i];
    return 0;
}

static int next_pid = 1;

// map kernel stack of task: 4 pages at its per-pid slot
static void map_kstack(struct task *t) {
    t->kstack_top = KSTACK_VA_BASE + (uint64_t)t->pid * 0x10000 + KSTACK_SIZE;
    uint64_t va = KSTACK_VA_BASE + (uint64_t)t->pid * 0x10000;
    for (int i = 0; i < KSTACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p)
            panic("task: no mem for kstack");
        // map into the kernel half of BOTH current and new pml4? kernel half
        // is shared on pml4 level, so mapping into current pml4 is enough
        vmm_map(vmm_kernel_pml4(), va + i * PAGE_SIZE, (uint64_t)p,
                VMM_PRESENT | VMM_WRITE);
    }
}

struct task *task_spawn_kernel(void (*entry)(void)) {
    struct task *t = task_find_free();
    if (!t)
        panic("task table full");
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->state = T_READY;
    t->pml4 = vmm_kernel_pml4();
    map_kstack(t);

    // forge a regs frame that "returns" into entry
    struct regs *r = (struct regs *)(t->kstack_top - sizeof(struct regs));
    memset(r, 0, sizeof(*r));
    r->rip = (uint64_t)entry;
    r->cs = SEL_KCODE;
    r->rflags = 0x202;
    r->rsp = t->kstack_top;
    r->ss = SEL_KDATA;
    t->rsp = (uint64_t)r;
    return t;
}

void task_unmap_user(struct task *t) {
    // walk user pml4 entries 0..255, free pages
    uint64_t *pml4 = phys2virt(t->pml4);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_PRESENT))
            continue;
        uint64_t *pdp = phys2virt(pml4[i] & 0x000ffffffffff000ULL);
        for (int j = 0; j < 512; j++) {
            if (!(pdp[j] & VMM_PRESENT))
                continue;
            uint64_t *pd = phys2virt(pdp[j] & 0x000ffffffffff000ULL);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_PRESENT))
                    continue;
                uint64_t *pt = phys2virt(pd[k] & 0x000ffffffffff000ULL);
                for (int m = 0; m < 512; m++) {
                    if (pt[m] & VMM_PRESENT)
                        pmm_free((void *)(pt[m] & 0x000ffffffffff000ULL));
                }
                pmm_free((void *)(pd[k] & 0x000ffffffffff000ULL));
            }
            pmm_free((void *)(pdp[j] & 0x000ffffffffff000ULL));
        }
        pmm_free((void *)(pml4[i] & 0x000ffffffffff000ULL));
        pml4[i] = 0;
    }
}

static void user_stack_setup(uint64_t pml4) {
    // 16 pages below USER_STACK_TOP
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p)
            panic("task: no mem for ustack");
        vmm_map(pml4, USER_STACK_TOP - (i + 1) * PAGE_SIZE,
                (uint64_t)p, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    }
}

struct task *task_spawn_user(const char *prog, struct task *parent) {
    uint64_t size;
    const uint8_t *image = prog_find(prog, &size);
    if (!image)
        return 0;


    struct task *t = task_find_free();
    if (!t)
        return 0;
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->parent = parent;

    uint64_t pml4 = vmm_create_pml4();
    t->pml4 = pml4;
    map_kstack(t);


    uint64_t entry = elf_load_user(t->pml4, image, size);

    if (!entry) {
        t->state = T_FREE;
        return 0;
    }
    user_stack_setup(t->pml4);


    // iret frame for ring 3 entry
    struct regs *r = (struct regs *)(t->kstack_top - sizeof(struct regs));
    memset(r, 0, sizeof(*r));
    r->rip = entry;
    r->cs = SEL_UCODE | 3;
    r->rflags = 0x202;
    r->rsp = USER_STACK_TOP - 16;
    r->ss = SEL_UDATA | 3;
    t->rsp = (uint64_t)r;
    t->state = T_READY;
    return t;
}

// fork: clone current task with full page copy
struct task *task_fork(struct regs *frame) {
    struct task *c = task_find_free();
    if (!c)
        return 0;
    memset(c, 0, sizeof(*c));
    c->pid = next_pid++;
    c->parent = current;
    c->state = T_READY;

    uint64_t pml4 = vmm_create_pml4();
    c->pml4 = pml4;

    // map child kernel stack first (shared kernel half trick: map into
    // current pml4 since kernel half is common)
    c->kstack_top = KSTACK_VA_BASE + (uint64_t)c->pid * 0x10000 + KSTACK_SIZE;
    uint64_t kva = KSTACK_VA_BASE + (uint64_t)c->pid * 0x10000;
    for (int i = 0; i < KSTACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p)
            return 0;
        vmm_map(vmm_kernel_pml4(), kva + i * PAGE_SIZE, (uint64_t)p,
                VMM_PRESENT | VMM_WRITE);
    }

    // copy user address space (pml4 entries 0..255)
    uint64_t *src_pml4 = phys2virt(current->pml4);
    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & VMM_PRESENT))
            continue;
        uint64_t *spdp = phys2virt(src_pml4[i] & 0x000ffffffffff000ULL);
        for (int j = 0; j < 512; j++) {
            if (!(spdp[j] & VMM_PRESENT))
                continue;
            uint64_t *spd = phys2virt(spdp[j] & 0x000ffffffffff000ULL);
            for (int k = 0; k < 512; k++) {
                if (!(spd[k] & VMM_PRESENT))
                    continue;
                uint64_t *spt = phys2virt(spd[k] & 0x000ffffffffff000ULL);
                for (int m = 0; m < 512; m++) {
                    if (!(spt[m] & VMM_PRESENT))
                        continue;
                    uint64_t src_phys = spt[m] & 0x000ffffffffff000ULL;
                    void *newpage = pmm_alloc();
                    if (!newpage)
                        return 0;
                    memcpy(phys2virt((uint64_t)newpage), phys2virt(src_phys), PAGE_SIZE);
                    // build child pt path
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) | ((uint64_t)m << 12);
                    vmm_map(c->pml4, va, (uint64_t)newpage,
                            (spt[m] & 0xffe) | (spt[m] & VMM_NX) | VMM_PRESENT);
                }
            }
        }
    }

    // child frame: copy of parent's, rax=0
    struct regs *cr = (struct regs *)(c->kstack_top - sizeof(struct regs));
    memcpy(cr, frame, sizeof(*cr));
    cr->rax = 0;
    c->rsp = (uint64_t)cr;
    return c;
}

uint64_t task_exit_current(int code) {
    task_unmap_user(current);
    current->exit_code = code;
    current->state = T_ZOMBIE;
    // wake parent if waiting
    if (current->parent && current->parent->state == T_BLOCKED &&
        current->parent->wait_reason == WAIT_CHILD) {
        current->parent->state = T_READY;
    }
    return task_schedule(0);
}

void task_wake_kbd(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state == T_BLOCKED && task_table[i].wait_reason == WAIT_KBD)
            task_table[i].state = T_READY;
}

uint64_t task_schedule(uint64_t old_rsp) {
    if (!current)
        return old_rsp; // scheduler not initialized yet
    if (old_rsp)
        current->rsp = old_rsp;

    // pick next ready task
    struct task *next = 0;
    for (int i = 1; i <= TASK_MAX; i++) {
        int idx = current ? (int)(current - task_table + i) % TASK_MAX : i - 1;
        if (task_table[idx].state == T_READY) {
            next = &task_table[idx];
            break;
        }
    }
    if (!next)
        return current ? current->rsp : 0;

    if (current && current->state == T_RUNNING)
        current->state = T_READY; // preempted, will resume later
    if (next == current)
        return current->rsp;

    current = next;
    current->state = T_RUNNING;
    tss_set_rsp0(current->kstack_top);
    vmm_switch(current->pml4);
    return current->rsp;
}

void task_init(void) {
    memset(task_table, 0, sizeof(task_table));
    // idle = kmain itself
    current = &task_table[0];
    current->pid = 0;
    current->state = T_RUNNING;
    current->pml4 = vmm_kernel_pml4();
    current->kstack_top = KSTACK_VA_BASE; // unused, we live on boot stack
    tss_set_rsp0(current->kstack_top);
    next_pid = 1;
}
