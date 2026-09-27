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
#include "vfs.h"

#define MAP_SHARED 0x01
#define PTE_DIRTY  0x040

// gs-relative scratch the syscall entry uses to find the kernel stack
struct kgs_scratch {
    uint64_t kstack_top;
    uint64_t user_rsp;
};

static uint64_t kgs_alloc(uint64_t kstack_top) {
    struct kgs_scratch *k = kmalloc(sizeof(*k));
    if (!k)
        panic("task: no kgs");
    k->kstack_top = kstack_top;
    k->user_rsp = 0;
    return (uint64_t)k;
}

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
    t->kgs = kgs_alloc(t->kstack_top);

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

// -- mmap reservations ----------------------------------------------

// write dirty pages of a shared file mapping back to the file, then drop
// the region list. page freeing stays with vmm_destroy_user.
void task_mmap_teardown(struct task *t) {
    struct mmap_region *m = t->mmaps;
    while (m) {
        struct mmap_region *nx = m->next;
        if (m->file) {
            if (m->flags & MAP_SHARED) {
                for (uint64_t va = m->start; va < m->end; va += PAGE_SIZE) {
                    uint64_t pte = vmm_get_pte(t->pml4, va);
                    if ((pte & VMM_PRESENT) && (pte & PTE_DIRTY)) {
                        uint64_t foff = m->off + (va - m->start);
                        m->file->vn->ops->write(m->file->vn,
                                                phys2virt(pte & 0x000ffffffffff000ULL),
                                                foff, PAGE_SIZE);
                    }
                }
            }
            vfs_close(m->file);
        }
        kfree(m);
        m = nx;
    }
    t->mmaps = 0;
}

void task_mmap_clone(struct task *dst, const struct task *src) {
    task_mmap_teardown(dst);
    struct mmap_region **tail = &dst->mmaps;
    for (const struct mmap_region *m = src->mmaps; m; m = m->next) {
        struct mmap_region *c = kmalloc(sizeof(*c));
        if (!c)
            return;
        *c = *m;
        if (m->file)
            m->file->refs++;   // child holds its own reference
        c->next = 0;
        *tail = c;
        tail = &c->next;
    }
}

// lazy fill of a file-backed mapping; called from the #PF path after the
// cow check says "not mine"
int task_mmap_fault(struct regs *r, uint64_t cr2) {
    (void)r;
    if (!current || (cr2 & 0xfff))
        return 0;
    for (struct mmap_region *m = current->mmaps; m; m = m->next) {
        if (cr2 < m->start || cr2 >= m->end)
            continue;
        if (!m->file)
            return 0;   // anon regions are mapped eagerly
        void *p = pmm_alloc_zeroed();
        if (!p)
            return 0;
        uint64_t foff = m->off + (cr2 - m->start);
        long n = m->file->vn->ops->read(m->file->vn,
                                        phys2virt((uint64_t)p), foff,
                                        PAGE_SIZE);
        if (n < 0)
            n = 0;   // beyond eof: page stays zero
        uint64_t vflags = VMM_PRESENT | VMM_USER;
        if (m->prot & 0x2)
            vflags |= VMM_WRITE;
        if (!(m->prot & 0x4))
            vflags |= VMM_NX;
        vmm_map(current->pml4, cr2, (uint64_t)p, vflags);
        return 1;
    }
    return 0;
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

// builds the initial user stack contents: argv/envp strings, auxv, then
// the arg vector itself. must run with the target pml4 active (kernel half
// is shared, so heap + our stack keep working). returns final entry rsp.
static uint64_t user_stack_build_args(uint64_t pml4, const char *prog,
                                      const struct elf_info *ei) {
    uint64_t old_cr3 = vmm_kernel_pml4();
    if (old_cr3 != pml4)
        vmm_switch(pml4);

    // 16 pseudo-random bytes for glibc's stack canary (AT_RANDOM)
    uint8_t rnd[16];
    uint64_t seed = pit_ticks();
    for (int i = 0; i < 16; i += 2) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        *(uint16_t *)(rnd + i) = (uint16_t)(seed >> 33);
    }

    // strings grow down from the top of the stack
    uint64_t sp = USER_STACK_TOP;

    sp -= 16;
    sp &= ~0xfULL;
    for (int i = 0; i < 16; i++)
        ((uint8_t *)sp)[i] = rnd[i];
    uint64_t rand_ptr = sp;

    // argv[0]
    uint64_t plen = strlen(prog) + 1;
    sp -= plen;
    memcpy((void *)sp, prog, plen);
    uint64_t argv0 = sp;

    // envp: a minimal PATH
    const char *env = "PATH=/bin";
    uint64_t elen = strlen(env) + 1;
    sp -= elen;
    memcpy((void *)sp, env, elen);
    uint64_t env0 = sp;

    sp &= ~0xfULL;

    // arg block: argc, argv[0], NULL, envp[0], NULL, 14 auxv pairs, AT_NULL
    uint64_t block = 8 * 1         // argc
                   + 8 * 2         // argv: ptr + NULL
                   + 8 * 2         // envp: ptr + NULL
                   + 8 * 2 * 14    // auxv pairs
                   + 8 * 2;        // AT_NULL
    sp -= block;
    sp &= ~0xfULL;
    uint64_t *a = (uint64_t *)sp;
    int i = 0;
    a[i++] = 1;           // argc
    a[i++] = argv0;
    a[i++] = 0;           // argv end
    a[i++] = env0;
    a[i++] = 0;           // envp end
    #define AUXV(type, val) do { a[i++] = (uint64_t)(type); a[i++] = (uint64_t)(val); } while (0)
    AUXV(3, ei ? ei->phdr_va : 0);    // AT_PHDR
    AUXV(4, ei ? ei->phent : 0);      // AT_PHENT
    AUXV(5, ei ? ei->phnum : 0);      // AT_PHNUM
    AUXV(6, 4096);        // AT_PAGESZ
    AUXV(9, ei ? ei->entry : 0);      // AT_ENTRY
    AUXV(11, 0);          // AT_UID
    AUXV(12, 0);          // AT_EUID
    AUXV(13, 0);          // AT_GID
    AUXV(14, 0);          // AT_EGID
    AUXV(15, 0);          // AT_PLATFORM
    AUXV(17, 100);        // AT_CLKTCK
    AUXV(23, 0);          // AT_SECURE
    AUXV(25, rand_ptr);   // AT_RANDOM: glibc aborts without it
    AUXV(31, argv0);      // AT_EXECFN
    #undef AUXV
    a[i++] = 0;  a[i++] = 0;          // AT_NULL

    if (old_cr3 != pml4)
        vmm_switch(old_cr3);
    return sp;
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
    t->kgs = kgs_alloc(t->kstack_top);

    struct elf_info ei;
    uint64_t entry = elf_load_user_info(t->pml4, image, size, &ei);
    kfree(image);

    if (!entry) {
        t->state = T_FREE;
        return 0;
    }
    t->entry_va = ei.entry;
    t->phdr_va = ei.phdr_va;
    t->phent = ei.phent;
    t->phnum = ei.phnum;
    t->brk_base = (ei.image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    t->brk_cur = t->brk_base;
    user_stack_setup(t->pml4);
    fds_init(t);

    uint64_t entry_rsp = user_stack_build_args(t->pml4, path, &ei);

    // iret frame for ring 3 entry
    struct regs *r = (struct regs *)(t->kstack_top - sizeof(struct regs));
    memset(r, 0, sizeof(*r));
    r->rip = entry;
    r->cs = SEL_UCODE | 3;
    r->rflags = 0x202;
    r->rsp = entry_rsp;
    r->ss = SEL_UDATA | 3;
    t->rsp = (uint64_t)r;
    t->state = T_READY;
    return t;
}

// exec current task with a new image from a vnode (execve / disk exec);
// name is a kernel-side copy used as argv[0]
uint64_t task_exec_current_named(struct vnode *vn, const char *name) {
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
    struct elf_info ei;
    uint64_t entry = elf_load_user_info(pml4, image, size, &ei);
    kfree(image);

    if (!entry) {
        // elf loader may have left us on the new pml4 — go back, then cleanup
        vmm_switch(old_cr3);
        vmm_destroy_user(pml4);
        return 0;
    }

    // write back shared mappings, then drop old image (we are on the new
    // pml4 already); pte walk must happen while the old pml4 still exists
    task_mmap_teardown(current);
    vmm_destroy_user(current->pml4);
    current->pml4 = pml4;
    current->brk_base = (ei.image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    current->brk_cur = current->brk_base;
    current->fs_base = 0;
    current->gs_base = 0;
    wrmsr(MSR_FS_BASE, 0);   // drop the old image's tls; gs stays kernel-owned
    current->entry_va = ei.entry;
    current->phdr_va = ei.phdr_va;
    current->phent = ei.phent;
    current->phnum = ei.phnum;
    current->clear_tid = 0;
    user_stack_setup(pml4);
    task_close_fds(current, 1);

    uint64_t entry_rsp = user_stack_build_args(pml4, name, &ei);

    // fresh iret frame on our kernel stack
    struct regs *fr = (struct regs *)current->rsp;
    memset(fr, 0, sizeof(*fr));
    fr->rip = entry;
    fr->cs = SEL_UCODE | 3;
    fr->rflags = 0x202;
    fr->rsp = entry_rsp;
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
    c->kgs = kgs_alloc(c->kstack_top);

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

    // brk + anon mmap reservations are part of the image
    c->brk_base = current->brk_base;
    c->brk_cur = current->brk_cur;
    task_mmap_clone(c, current);

    // child frame: copy of parent's, rax=0
    struct regs *cr = (struct regs *)(c->kstack_top - sizeof(struct regs));
    memcpy(cr, frame, sizeof(*cr));
    cr->rax = 0;
    c->rsp = (uint64_t)cr;
    return c;
}

uint64_t task_exit_current(int code) {
    task_close_fds(current, 0);
    task_mmap_teardown(current);
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
    // gs points at this task's syscall scratch; fs carries user tls
    wrmsr(MSR_GS_BASE, current->kgs);
    wrmsr(MSR_FS_BASE, current->fs_base);
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
    current->kgs = kgs_alloc(KSTACK_VA_BASE);
    wrmsr(MSR_GS_BASE, current->kgs);
    tss_set_rsp0(current->kstack_top);
    next_pid = 1;
}

// compat: exec without a program name
uint64_t task_exec_current(struct vnode *vn) {
    return task_exec_current_named(vn, "prog");
}
