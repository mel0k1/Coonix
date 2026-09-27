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
#include "heap.h"

struct task task_table[TASK_MAX];
struct task *current;

// static console fds: fd 0 = kbd in, 1/2 = console out
static struct file console_fds[3] = {
    { .vn = 0, .refs = 1, .is_console = 1 },
    { .vn = 0, .refs = 1, .is_console = 1 },
    { .vn = 0, .refs = 1, .is_console = 1 },
};

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

static void fds_init(struct task *t) {
    for (int i = 0; i < 3; i++) {
        t->fds[i] = &console_fds[i];
        console_fds[i].refs++;
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
    vmm_destroy_user(t->pml4);
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

struct task *task_spawn_user(const char *path, struct task *parent) {
    void *image;
    long size = vfs_read_file(path, &image);
    if (size < 0)
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
    kfree(image);

    if (!entry) {
        t->state = T_FREE;
        return 0;
    }
    user_stack_setup(t->pml4);
    fds_init(t);

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

// exec current task with a new image from a vnode (execve / disk exec)
uint64_t task_exec_current(struct vnode *vn) {
    if (!vn || vn->type != VNODE_FILE)
        return 0;
    void *image = kmalloc(vn->size ? vn->size : 1);
    if (!image)
        return 0;
    long size = vn->ops->read(vn, image, 0, vn->size);
    if (size < 0) {
        kfree(image);
        return 0;
    }

    uint64_t old_cr3 = vmm_kernel_pml4();
    uint64_t pml4 = vmm_create_pml4();
    uint64_t entry = elf_load_user(pml4, image, size);
    kfree(image);

    if (!entry) {
        // elf loader may have left us on the new pml4 — go back, then cleanup
        vmm_switch(old_cr3);
        vmm_destroy_user(pml4);
        return 0;
    }

    // drop old image (we are on the new pml4 already), keep going on new one
    vmm_destroy_user(current->pml4);
    current->pml4 = pml4;
    user_stack_setup(pml4);
    task_close_fds(current, 1);

    // fresh iret frame on our kernel stack
    struct regs *fr = (struct regs *)current->rsp;
    memset(fr, 0, sizeof(*fr));
    fr->rip = entry;
    fr->cs = SEL_UCODE | 3;
    fr->rflags = 0x202;
    fr->rsp = USER_STACK_TOP - 16;
    fr->ss = SEL_UDATA | 3;
    return (uint64_t)fr;
}

int task_fd_alloc(struct file *f) {
    for (int i = 0; i < FILE_MAX; i++)
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    return -1;
}

void task_close_fds(struct task *t, int keep_console) {
    for (int i = 0; i < FILE_MAX; i++) {
        if (!t->fds[i])
            continue;
        if (keep_console && t->fds[i]->is_console)
            continue;
        vfs_close(t->fds[i]);
        t->fds[i] = 0;
    }
}

// fork: clone address space via copy-on-write
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

    // cow-share user address space (pml4 entries 0..255): writable pages
    // become read-only + COW in both tasks, faults privatize them later
    uint64_t *src_pml4 = phys2virt(current->pml4);
    int downgraded = 0;
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
                    uint64_t pte = spt[m];
                    uint64_t phys = pte & 0x000ffffffffff000ULL;
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) | ((uint64_t)m << 12);
                    uint64_t flags = pte & 0xfff;   // incl. soft COW bit
                    if (pte & VMM_WRITE) {
                        flags &= ~VMM_WRITE;
                        flags |= VMM_COW;
                        spt[m] = (pte & ~VMM_WRITE) | VMM_COW; // parent too
                        downgraded = 1;
                    }
                    vmm_map(pml4, va, phys, flags);
                    pmm_ref((void *)phys);
                }
            }
        }
    }
    if (downgraded)
        vmm_switch(current->pml4);   // flush stale RW tlb entries

    // inherit open files
    for (int i = 0; i < FILE_MAX; i++)
        if (current->fds[i]) {
            c->fds[i] = current->fds[i];
            c->fds[i]->refs++;
        }

    // child frame: copy of parent's, rax=0
    struct regs *cr = (struct regs *)(c->kstack_top - sizeof(struct regs));
    memcpy(cr, frame, sizeof(*cr));
    cr->rax = 0;
    c->rsp = (uint64_t)cr;
    return c;
}

uint64_t task_exit_current(int code) {
    task_close_fds(current, 0);
    vmm_destroy_user(current->pml4);
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
    if (!next) {
        // nobody ready: if we were running, keep running (idle spin);
        // blocked/zombie current must never resume -> park on idle task
        if (current->state == T_RUNNING)
            return current->rsp;
        next = &task_table[0];
        if (next == current)
            return current->rsp;
    }

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
