#include "task.h"
#include "kernel.h"
#include "vmm.h"

// deliver a syscall result straight into a blocked task's saved frame
// (body lives in futex.c next to its futex-wake users)
void task_frame_syscall_result(struct task *t, long ret);

#include "pmm.h"
#include "console.h"
#include "string.h"
#include "serial.h"
#include "elf.h"
#include "pit.h"
#include "gdt.h"
#include "heap.h"
#include "vfs.h"
#include "signal.h"

#define MAP_SHARED 0x01
#define PTE_DIRTY  0x040

// clone flags (linux)
#define CLONE_VM            0x100
#define CLONE_FS            0x200
#define CLONE_FILES         0x400
#define CLONE_SIGHAND       0x800
#define CLONE_VFORK         0x4000
#define CLONE_THREAD        0x10000
#define CLONE_SYSVSEM       0x40000
#define CLONE_SETTLS        0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID  0x01000000

// gs scratch lives inside the task slot: zero allocations, cannot dangle.
// syscall_entry reads gs:0 (kstack_top) and gs:8 (user rsp stash)
static void kgs_init(struct task *t) {
    t->kgs_area[0] = t->kstack_top;
    t->kgs_area[1] = 0;
    t->kgs = (uint64_t)&t->kgs_area[0];
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
        if (task_table[i].state == T_FREE) {
            // reclaim /proc leftovers from the previous occupant; callers
            // memset the slot right after this returns
            if (task_table[i].cmdline)
                kfree(task_table[i].cmdline);
            return &task_table[i];
        }
    return 0;
}

// /proc comm: last path component, truncated to fit
void task_set_comm(struct task *t, const char *name) {
    const char *base = name;
    for (const char *p = name; p && *p; p++)
        if (*p == '/')
            base = p + 1;
    strncpy(t->comm, base, sizeof(t->comm) - 1);
    t->comm[sizeof(t->comm) - 1] = 0;
}

static int next_pid = 1;

// per-pid kstack window: 0x70000000 / 64k per slot below the VA top
#define KSTACK_PID_MAX 0x7000

// lowest free pid >= next_pid; pids must be recycled or the kstack VA
// window walks off the top of the address space
static int pid_alloc(void) {
    for (int n = 0; n < KSTACK_PID_MAX; n++) {
        int p = next_pid + n <= KSTACK_PID_MAX ? next_pid + n : 1 + n;
        int used = 0;
        for (int i = 0; i < TASK_MAX; i++)
            if (task_table[i].state != T_FREE && task_table[i].pid == p) {
                used = 1;
                break;
            }
        if (!used) {
            next_pid = p < KSTACK_PID_MAX ? p + 1 : 1;
            return p;
        }
    }
    return 0;
}

// map kernel stack of task: 4 pages at its per-pid slot
static void map_kstack(struct task *t) {
    t->kstack_top = KSTACK_VA_BASE + (uint64_t)t->pid * 0x10000 + KSTACK_SIZE;
    uint64_t va = KSTACK_VA_BASE + (uint64_t)t->pid * 0x10000;
    for (int i = 0; i < KSTACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p)
            panic("task: no mem for kstack");
        // a previous owner may not be reaped yet: replace any stale frame
        uint64_t old = vmm_get_phys(vmm_kernel_pml4(), va + i * PAGE_SIZE);
        if (old)
            pmm_free((void *)old);
        // map into the kernel half of BOTH current and new pml4? kernel half
        // is shared on pml4 level, so mapping into current pml4 is enough
        vmm_map(vmm_kernel_pml4(), va + i * PAGE_SIZE, (uint64_t)p,
                VMM_PRESENT | VMM_WRITE | VMM_NX);   // w^x: kstacks don't run
    }
}

// release kstack + gs scratch of a task whose slot is going away
static void free_kstack(struct task *t) {
    if (!t->kstack_top)
        return;
    uint64_t va = t->kstack_top - KSTACK_SIZE;
    for (int i = 0; i < KSTACK_PAGES; i++) {
        uint64_t phys = vmm_get_phys(vmm_kernel_pml4(), va + i * PAGE_SIZE);
        vmm_unmap(vmm_kernel_pml4(), va + i * PAGE_SIZE);
        if (phys)
            pmm_free((void *)phys);
    }
    t->kstack_top = 0;
    t->kgs = 0;   // scratch is in the slot: nothing to free
}

// exiting threads leave their kstack mapped in the slot: nobody may
// unmap the stack a task still runs on, and map_kstack replaces the
// stale frames when a recycled pid re-enters service
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
    t->pid = pid_alloc();
    if (!t->pid)
        panic("task: no free pid");
    t->state = T_READY;
    t->pml4 = vmm_kernel_pml4();
    map_kstack(t);
    kgs_init(t);

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

static int mmap_fill_page(struct mmap_region *m, uint64_t page);

// lazy fill of a file-backed mapping; called from the #PF path after the
// cow check says "not mine"
int task_mmap_fault(struct regs *r, uint64_t cr2) {
    (void)r;
    if (!current)
        return 0;
    uint64_t page = cr2 & ~(PAGE_SIZE - 1);
    for (struct mmap_region *m = current->mmaps; m; m = m->next) {
        if (page < m->start || page >= m->end)
            continue;
        if (!m->file)
            return 0;   // anon regions are mapped eagerly
        return mmap_fill_page(m, page);
    }
    return 0;
}

// fill one page of a file-backed region. static so the fault path and
// deliberate prefaulting share it. NOT reentrant: the fill issues ext2
// reads, and a nested fill (fault while a disk op is in flight) would
// recurse into the driver mid-transfer — the guard turns that into a
// clean SIGSEGV instead of a triple fault
static int mmap_filling;
static int mmap_fill_page(struct mmap_region *m, uint64_t page) {
    if (mmap_filling)
        return 0;
    mmap_filling = 1;
    void *p = pmm_alloc_zeroed();
    int ok = 0;
    if (p) {
        uint64_t foff = m->off + (page - m->start);
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
        vmm_map(current->pml4, page, (uint64_t)p, vflags);
        ok = 1;
    }
    mmap_filling = 0;
    return ok;
}

// deliberately fault in every lazily-backed page of a user range before
// the kernel copies to/from it (syscalls with kernel-side buffer
// access). must run BEFORE any disk op starts: the fill itself does
// ext2 reads and must not nest inside one
void task_prefault_range(struct task *t, uint64_t uaddr, uint64_t len) {
    if (!t || !t->pml4 || !len)
        return;
    if (uaddr >= VMM_USER_LIMIT || len > VMM_USER_LIMIT - uaddr)
        return;
    uint64_t last = (uaddr + len - 1) & ~0xfffULL;
    for (uint64_t va = uaddr & ~0xfffULL;; va += 0x1000) {
        if (!(vmm_get_pte(t->pml4, va) & VMM_PRESENT)) {
            for (struct mmap_region *m = t->mmaps; m; m = m->next)
                if (va >= m->start && va < m->end) {
                    if (m->file)
                        mmap_fill_page(m, va);
                    break;
                }
        }
        if (va == last)
            break;
    }
}

// user-pointer validation with the task's lazy mappings in mind. the
// strict page walk rejects not-yet-faulted pages, but a pointer into
// file-backed mmap space (libc .bss, untouched .data) is perfectly
// valid: the access faults it in via task_mmap_fault. so: strict walk
// first; pages that fail it must be covered by a file-backed region
// (anon regions are mapped eagerly, so non-present there is really
// invalid). need_write demands W or COW on mapped pages, PROT_WRITE on
// lazy ones
int task_user_range_ok(struct task *t, uint64_t uaddr, uint64_t len,
                       int need_write) {
    if (!t || !t->pml4)
        return 0;
    if (vmm_user_range_ok(t->pml4, uaddr, len, need_write))
        return 1;
    if (!len)
        return 1;
    if (uaddr >= VMM_USER_LIMIT || len > VMM_USER_LIMIT - uaddr)
        return 0;
    uint64_t last = (uaddr + len - 1) & ~0xfffULL;
    for (uint64_t va = uaddr & ~0xfffULL;; va += 0x1000) {
        uint64_t pte = vmm_get_pte(t->pml4, va);
        if (pte & VMM_PRESENT) {
            // strict walk already passed everything present; a present
            // page only fails the walk on the write check
            if (need_write && !(pte & (VMM_WRITE | VMM_COW)))
                return 0;
        } else {
            // not faulted yet: only a file-backed region can serve it
            struct mmap_region *m = t->mmaps;
            for (; m; m = m->next)
                if (va >= m->start && va < m->end)
                    break;
            if (!m || !m->file)
                return 0;
            if (need_write && !(m->prot & 0x2))
                return 0;
        }
        if (va == last)
            return 1;
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

// builds the initial user stack contents: argv/envp strings, auxv, then
// the arg vector itself. must run with the target pml4 active (kernel half
// is shared, so heap + our stack keep working). returns final entry rsp.
// ea = caller-captured argv/envp blocks (NUL-separated), 0 = defaults
#define EXEC_ARG_MAX 32
static uint64_t user_stack_build_args(uint64_t pml4, const char *prog,
                                      const struct elf_info *ei,
                                      const exec_args_t *ea) {
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

    // flatten the arg vectors
    const char *av[EXEC_ARG_MAX];
    int argc = 0;
    const char *ev[EXEC_ARG_MAX];
    int envc = 0;
    if (ea && ea->argv)
        for (const char *p = ea->argv; *p && argc < EXEC_ARG_MAX;
             p += strlen(p) + 1)
            av[argc++] = p;
    if (!argc)
        av[argc++] = prog;   // empty argv still needs argv[0] for auxv
    if (ea && ea->envp)
        for (const char *p = ea->envp; *p && envc < EXEC_ARG_MAX;
             p += strlen(p) + 1)
            ev[envc++] = p;
    if (!envc)
        ev[envc++] = "PATH=/bin";

    // strings grow down from the top of the stack
    uint64_t sp = USER_STACK_TOP;

    sp -= 16;
    sp &= ~0xfULL;
    for (int i = 0; i < 16; i++)
        ((uint8_t *)sp)[i] = rnd[i];
    uint64_t rand_ptr = sp;

    uint64_t argv_ptr[EXEC_ARG_MAX], envp_ptr[EXEC_ARG_MAX];
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t l = strlen(av[i]) + 1;
        sp -= l;
        memcpy((void *)sp, av[i], l);
        argv_ptr[i] = sp;
    }
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t l = strlen(ev[i]) + 1;
        sp -= l;
        memcpy((void *)sp, ev[i], l);
        envp_ptr[i] = sp;
    }

    sp &= ~0xfULL;

    // arg block: argc, argv+NULL, envp+NULL, auxv pairs, AT_NULL
    uint64_t block = 8                       // argc
                   + 8 * (argc + 1)          // argv vector
                   + 8 * (envc + 1)          // envp vector
                   + 8 * 2 * 15              // auxv pairs
                   + 8 * 2;                  // AT_NULL
    sp -= block;
    sp &= ~0xfULL;
    uint64_t *a = (uint64_t *)sp;
    int i = 0;
    a[i++] = (uint64_t)argc;
    for (int k = 0; k < argc; k++)
        a[i++] = argv_ptr[k];
    a[i++] = 0;
    for (int k = 0; k < envc; k++)
        a[i++] = envp_ptr[k];
    a[i++] = 0;
    #define AUXV(type, val) do { a[i++] = (uint64_t)(type); a[i++] = (uint64_t)(val); } while (0)
    AUXV(3, ei ? ei->phdr_va : 0);    // AT_PHDR
    AUXV(4, ei ? ei->phent : 0);      // AT_PHENT
    AUXV(5, ei ? ei->phnum : 0);      // AT_PHNUM
    AUXV(6, 4096);        // AT_PAGESZ
    AUXV(7, ei ? ei->base : 0);       // AT_BASE: ld.so load base
    AUXV(9, ei ? ei->entry : 0);      // AT_ENTRY: program entry
    AUXV(11, 0);          // AT_UID
    AUXV(12, 0);          // AT_EUID
    AUXV(13, 0);          // AT_GID
    AUXV(14, 0);          // AT_EGID
    AUXV(15, 0);          // AT_PLATFORM
    AUXV(17, 100);        // AT_CLKTCK
    AUXV(23, 0);          // AT_SECURE
    AUXV(25, rand_ptr);   // AT_RANDOM: glibc aborts without it
    AUXV(31, argv_ptr[0]);// AT_EXECFN
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
    t->pid = pid_alloc();
    t->tgid = t->pid;         // fresh process: own group
    if (!t->pid)
        return 0;
    t->pgid = t->pid;
    t->parent = parent;
    t->start_tick = pit_ticks();

    uint64_t pml4 = vmm_create_pml4();
    t->pml4 = pml4;
    map_kstack(t);
    kgs_init(t);

    // same cr3-dance hazard as task_execve: elf_load runs on the new
    // address space; keep the tick out of the copy (boot-time insurance)
    uint64_t eflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(eflags));
    cli();
    struct elf_info ei;
    uint64_t entry = elf_load_user_info(t->pml4, image, size, &ei);
    kfree(image);
    __asm__ volatile("pushq %0; popfq" :: "r"(eflags) : "memory");

    if (!entry) {
        vmm_destroy_user(pml4);
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
    strcpy(t->cwd, "/");
    task_set_comm(t, path);
    uint64_t plen = strlen(path) + 1;
    t->cmdline = kmalloc(plen);
    if (t->cmdline) {
        memcpy(t->cmdline, path, plen);
        t->cmdline_len = (int)plen;
    }

    uint64_t entry_rsp = user_stack_build_args(t->pml4, path, &ei, 0);

    // iret frame for ring 3 entry
    struct regs *r = (struct regs *)(t->kstack_top - sizeof(struct regs));
    memset(r, 0, sizeof(*r));
    r->rip = ei.jump ? ei.jump : entry;
    r->cs = SEL_UCODE | 3;
    r->rflags = 0x202;
    r->rsp = entry_rsp;
    r->ss = SEL_UDATA | 3;
    t->rsp = (uint64_t)r;
    t->state = T_READY;
    return t;
}

// full execve: keep cwd, drop only CLOEXEC fds (redirections survive),
// build the new stack from the caller's argv/envp
uint64_t task_execve(struct vnode *vn, const char *name,
                     const exec_args_t *ea) {
    if (!vn || vn->type != VNODE_FILE)
        return 0;
    // the whole image swap runs with interrupts off. elf_load switches
    // cr3 to the new address space and copies segment bytes through USER
    // virtual addresses; a tick mid-copy would park this task with the
    // OLD pml4 in current->pml4, resume it on the OLD cr3, and the rest
    // of the memcpy would land in the old address space — the new image
    // keeps zeroed holes (ld.so: garbage relocs, lookup asserts, #GP).
    // exec is short; non-preemptible is fine. the success return goes to
    // the iretq of the new frame (IF restored); failure paths restore
    // the saved flags explicitly
    uint64_t eflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(eflags));
    cli();
    void *image = kmalloc(vn->size ? vn->size : 1);
    if (!image) {
        __asm__ volatile("pushq %0; popfq" :: "r"(eflags) : "memory");
        return 0;
    }
    long size = vn->ops->read(vn, image, 0, vn->size);
    if (size < 0) {
        kfree(image);
        __asm__ volatile("pushq %0; popfq" :: "r"(eflags) : "memory");
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
        __asm__ volatile("pushq %0; popfq" :: "r"(eflags) : "memory");
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
    current->fpu_ready = 0;   // fresh image: fpu state resets
    wrmsr(MSR_FS_BASE, 0);   // drop the old image's tls; gs stays kernel-owned
    current->entry_va = ei.entry;
    current->phdr_va = ei.phdr_va;
    current->phent = ei.phent;
    current->phnum = ei.phnum;
    current->clear_tid = 0;
    current->child_tid = 0;
    current->sig_pending = 0;
    // posix exec: caught dispositions reset to SIG_DFL (SIG_IGN survives),
    // the alternate signal stack does not survive exec; mask is kept
    for (int s = 1; s <= SIG_MAX; s++)
        if (current->sigact[s].handler > 1)
            current->sigact[s].handler = 0;
    current->alt_sp = 0;
    current->alt_size = 0;
    user_stack_setup(pml4);
    // exec: only CLOEXEC fds close (posix); console + redirect targets stay
    for (int i = 0; i < FILE_MAX; i++) {
        if (!current->fds[i])
            continue;
        if (current->fd_flags[i] & FD_CLOEXEC) {
            vfs_close(current->fds[i]);
            current->fds[i] = 0;
            current->fd_flags[i] = 0;
        }
    }

    uint64_t entry_rsp = user_stack_build_args(pml4, name, &ei, ea);

    // adopt argv for /proc/<pid>/cmdline (the block was kmalloc'd by the
    // execve syscall layer; on failure paths it stays with the caller).
    // runs after the last failure return: exec cannot fail past here
    char *old_cmdline = current->cmdline;
    current->cmdline = 0;
    current->cmdline_len = 0;
    const char *comm_src = name;
    if (ea && ea->argv) {
        int len = 0;
        for (const char *p = ea->argv; *p; p += strlen(p) + 1)
            len += (int)strlen(p) + 1;
        current->cmdline = ea->argv;
        current->cmdline_len = len;
        comm_src = ea->argv;          // argv[0]
    }
    task_set_comm(current, comm_src);
    if (old_cmdline)
        kfree(old_cmdline);

    // fresh iret frame on our kernel stack
    struct regs *fr = (struct regs *)current->rsp;
    memset(fr, 0, sizeof(*fr));
    fr->rip = ei.jump ? ei.jump : entry;
    fr->cs = SEL_UCODE | 3;
    fr->rflags = 0x202;
    fr->rsp = entry_rsp;
    fr->ss = SEL_UDATA | 3;
    return (uint64_t)fr;
}

// legacy wrappers: defaults for argv/envp, console-only fd policy
uint64_t task_exec_current_named(struct vnode *vn, const char *name) {
    return task_execve(vn, name, 0);
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

// fork: clone address space via copy-on-write. interrupts stay off for the
// whole walk: a tick mid-fork would let another task grab our slot or
// double-run the syscall
struct task *task_fork(struct regs *frame) {
    cli();
    struct task *c = task_find_free();
    if (!c) {
        sti();
        return 0;
    }
    fpu_flush(current);   // snapshot live fpu state before copying
    memset(c, 0, sizeof(*c));
    c->pid = pid_alloc();
    if (!c->pid) {
        sti();
        return 0;
    }
    c->tgid = c->pid;
    c->pgid = current->pgid;      // same console group
    c->parent = current;
    c->start_tick = pit_ticks();
    c->state = T_READY;

    uint64_t pml4 = vmm_create_pml4();
    c->pml4 = pml4;

    // map child kernel stack first (shared kernel half trick: map into
    // current pml4 since kernel half is common); map_kstack gives nx
    map_kstack(c);
    kgs_init(c);

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
                    // w^x: 0xfff keeps PRESENT/WRITE/USER/COW, but NX
                    // lives at bit 63 and must ride along too, or every
                    // fork child gets executable data pages
                    uint64_t flags = (pte & 0xfff) | (pte & VMM_NX);
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
    memcpy(c->fd_flags, current->fd_flags, sizeof(c->fd_flags));
    strcpy(c->cwd, current->cwd);

    // brk + anon mmap reservations are part of the image
    c->brk_base = current->brk_base;
    c->brk_cur = current->brk_cur;
    task_mmap_clone(c, current);

    // signal state: handlers copied, pending dropped, mask kept (posix)
    memcpy(c->sigact, current->sigact, sizeof(c->sigact));
    c->sig_mask = current->sig_mask;
    // fpu state is part of the execution context (posix fork semantics)
    memcpy(c->fpu_area, current->fpu_area, sizeof(c->fpu_area));
    c->fpu_ready = current->fpu_ready;
    c->tgid = c->pid;
    // tls: the child keeps the parent's thread pointer (glibc binaries
    // dereference %fs:... constantly; a zero fs_base sends them into the
    // elf header page and worse)
    c->fs_base = current->fs_base;
    c->gs_base = current->gs_base;

    // child frame: copy of parent's, rax=0
    struct regs *cr = (struct regs *)(c->kstack_top - sizeof(struct regs));
    memcpy(cr, frame, sizeof(*cr));
    cr->rax = 0;
    c->rsp = (uint64_t)cr;
    sti();
    return c;
}
// CLONE_THREAD: same address space (shared pml4), new tid, own kernel stack.
// glibc pthread_create needs SETTLS + PARENT_SETTID + CHILD_CLEARTID.
// interrupts off: the caller switches to the child before enabling them
struct task *task_clone_thread(struct regs *frame, uint64_t flags,
                               uint64_t newsp, uint64_t parent_tid,
                               uint64_t child_tid, uint64_t tls) {
    if (!(flags & CLONE_THREAD) || !(flags & CLONE_VM))
        return 0;   // caller falls back to fork semantics
    cli();
    struct task *c = task_find_free();
    if (!c) {
        sti();
        return 0;
    }
    fpu_flush(current);   // snapshot live fpu state before copying
    memset(c, 0, sizeof(*c));
    c->pid = pid_alloc();
    if (!c->pid) {
        sti();
        return 0;
    }
    c->tgid = current->tgid;
    c->pgid = current->pgid;
    c->start_tick = pit_ticks();
    c->parent = current->parent ? current->parent : current;
    c->state = T_READY;
    c->pml4 = current->pml4;      // shared address space
    map_kstack(c);
    kgs_init(c);

    // own copies of the fd table refs and mmap list (same pattern as fork;
    // pages themselves are shared via the pml4)
    for (int i = 0; i < FILE_MAX; i++)
        if (current->fds[i]) {
            c->fds[i] = current->fds[i];
            c->fds[i]->refs++;
        }
    memcpy(c->fd_flags, current->fd_flags, sizeof(c->fd_flags));
    strcpy(c->cwd, current->cwd);
    memcpy(c->comm, current->comm, sizeof(c->comm));   // threads share comm
    c->brk_base = current->brk_base;
    c->brk_cur = current->brk_cur;
    task_mmap_clone(c, current);

    c->fs_base = (flags & CLONE_SETTLS) ? tls : current->fs_base;
    c->clear_tid = current->clear_tid;
    if (flags & CLONE_CHILD_CLEARTID)
        c->child_tid = child_tid;
    memcpy(c->sigact, current->sigact, sizeof(c->sigact));
    c->sig_mask = current->sig_mask;

    // kernel writes into user memory: only through mapped user pages
    if (flags & CLONE_PARENT_SETTID &&
        task_user_range_ok(current, (uint64_t)parent_tid, 4, 1))
        *(int *)parent_tid = c->pid;
    if (flags & CLONE_CHILD_SETTID &&
        task_user_range_ok(current, (uint64_t)child_tid, 4, 1))
        *(int *)child_tid = c->pid;
    // fpu state is part of the execution context (clone shares it)
    memcpy(c->fpu_area, current->fpu_area, sizeof(c->fpu_area));
    c->fpu_ready = current->fpu_ready;

    // child frame = parent's with rax=0 and the new user stack
    struct regs *cr = (struct regs *)(c->kstack_top - sizeof(struct regs));
    memcpy(cr, frame, sizeof(*cr));
    cr->rax = 0;
    if (newsp)
        cr->rsp = newsp;
    c->rsp = (uint64_t)cr;
    return c;
}

// summed cpu time of every live/zombie thread of the group
void task_cpu_group(int tgid, uint64_t *utime, uint64_t *stime) {
    uint64_t u = 0, s = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state != T_FREE && t->tgid == tgid) {
            u += t->utime;
            s += t->stime;
        }
    }
    *utime = u;
    *stime = s;
}

int task_count_group(int tgid, struct task *except) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (&task_table[i] != except && task_table[i].state != T_FREE &&
            task_table[i].tgid == tgid)
            n++;
    return n;
}

// thread death: everything the thread owns privately goes away, the shared
// address space and the other threads stay. self=1 when t == current:
// the kernel stack cannot be unmapped while we run on it, so it goes to
// the reap queue for the idle task
static void task_thread_release(struct task *t, int self) {
    // joiners watch *child_tid: zero it and wake the futex; the pointer
    // was validated at set_tid_address/clone time, re-check before the
    // write in case user unmapped it since
    uint64_t ct = t->child_tid ? t->child_tid : t->clear_tid;
    if (ct && task_user_range_ok(t, ct, 4, 1)) {
        *(uint32_t *)ct = 0;
        extern void futex_wake_addr(uint64_t uaddr, int n);
        futex_wake_addr(ct, -1);
    }
    task_close_fds(t, 0);
    task_mmap_teardown(t);        // own list copy: file refs back
    fpu_forget(t);
    if (self) {
        // keep the stack mapped, slot reuse frees it (see map_kstack)
        t->kgs = 0;
    } else {
        free_kstack(t);
    }
    t->state = T_FREE;
}

static uint64_t exit_common(int code, int sig_death, int force_group) {
    // interrupts off for the whole teardown: a tick mid-exit could pick a
    // sibling whose kstack/pml4 is already half-freed, or leave the exiting
    // task schedulable on a destroyed image. every path below ends in
    // task_schedule(0) and the next task's iretq restores IF
    cli();
    int i_am_leader = current->pid == current->tgid;
    int others = task_count_group(current->tgid, current);
    if (!force_group && !i_am_leader && others > 0) {
        // pthread_exit from a worker: only this thread goes away
        task_thread_release(current, 1);
        return task_schedule(0);
    }
    // the dying group's cpu totals move to the parent (getrusage children)
    // BEFORE sibling release flips their slots to T_FREE
    if (current->parent) {
        uint64_t gu = 0, gs = 0;
        for (int i = 0; i < TASK_MAX; i++) {
            struct task *t = &task_table[i];
            if (t->state != T_FREE && t->tgid == current->tgid) {
                gu += t->utime;
                gs += t->stime;
            }
        }
        current->parent->cutime += gu;
        current->parent->cstime += gs;
    }
    // group death: release every sibling thread, then tear down the process
    if (others > 0) {
        for (int i = 0; i < TASK_MAX; i++) {
            struct task *t = &task_table[i];
            if (t == current || t->state == T_FREE || t->tgid != current->tgid)
                continue;
            task_thread_release(t, 0);   // not running: safe to free now
        }
    }
    task_close_fds(current, 0);
    task_mmap_teardown(current);
    vmm_destroy_user(current->pml4);
    fpu_forget(current);
    // reparent children: there is no init, so orphans lose their parent
    // and zombie orphans are reaped at once (nobody would wait4 them)
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t == current || t->state == T_FREE || t->parent != current)
            continue;
        t->parent = 0;
        if (t->state == T_ZOMBIE) {
            kfree(t->cmdline);
            t->cmdline = 0;
            free_kstack(t);
            t->state = T_FREE;
        }
    }
    // leader slot stays for the parent's wait4 (zombie); an orphan group
    // has nobody to wait4 it: vanish instead
    struct task *leader = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].tgid == current->tgid && task_table[i].pid == task_table[i].tgid)
            leader = &task_table[i];
    if (leader && leader->parent) {
        leader->exit_code = code;
        leader->sig_death = sig_death;
        leader->state = T_ZOMBIE;
        if (leader->parent->state == T_BLOCKED &&
            leader->parent->wait_reason == WAIT_CHILD)
            leader->parent->state = T_READY;
    }
    if (!leader || !leader->parent || current != leader) {
        // stack stays mapped for slot reuse; we still run on it
        current->kgs = 0;
        current->state = T_FREE;
    }
    return task_schedule(0);
}

uint64_t task_exit_current(int code) {
    return exit_common(code, 0, 0);
}

uint64_t task_exit_current_group(int code) {
    return exit_common(code, 0, 1);
}

uint64_t task_exit_current_sig(int sig) {
    return exit_common(sig, 1, 1);
}

void task_wake_kbd(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state == T_BLOCKED && task_table[i].wait_reason == WAIT_KBD)
            task_table[i].state = T_READY;
}

// unblock readers/writers parked on a pipe; the replayed syscall sees the
// new buffer state (or EOF/EPIPE)
void task_wake_pipe(struct pipe *p) {
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state == T_BLOCKED &&
            task_table[i].wait_reason == WAIT_PIPE &&
            task_table[i].wait_pipe == p)
            task_table[i].state = T_READY;
}

void task_tick_wake(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state != T_BLOCKED || !t->wake_tick || now < t->wake_tick)
            continue;
        if (t->wait_reason == WAIT_SLEEP) {
            // nanosleep completes: deliver rax=0 into the saved frame.
            // frame write only for still-blocked tasks: a task woken by a
            // signal first is T_READY with a live kernel/user context whose
            // rax we must not clobber
            task_frame_syscall_result(t, 0);
            // stale state must not outlive the wake: a later futex block
            // with leftover WAIT_SLEEP+wake_tick gets spuriously tick-woken
            t->wake_tick = 0;
            t->wait_reason = WAIT_NONE;
            t->state = T_READY;
        } else if (t->wait_reason == WAIT_SIGNAL &&
                   t->sig_wait_kind == SW_SIGWAIT) {
            // rt_sigtimedwait timeout: -EAGAIN. the frame was NOT rewound
            // (sigwait blocks without replay_fixup), so stuff rax only —
            // task_frame_syscall_result would skip 2 bytes of user code
            t->sig_wait_kind = 0;
            ((struct regs *)t->rsp)->rax = (uint64_t)-11;   // -EAGAIN
            t->sig_woke_rewind = 0;
            t->wake_tick = 0;
            t->wait_reason = WAIT_NONE;
            t->state = T_READY;
        }
    }
}

uint64_t task_schedule(uint64_t old_rsp) {
    // the pick + state updates must be atomic against the tick: a tick
    // mid-schedule would save the interrupted frame into whatever task
    // `current` points at, mixing contexts. every exit path here resumes
    // through iretq, which restores IF from the target frame
    cli();
    if (!current)
        return old_rsp; // scheduler not initialized yet
    if (old_rsp) {
        current->rsp = old_rsp;
        // cpu accounting: the frame we just saved tells the mode the task
        // ran in since its last schedule (ring 3 = user, else kernel)
        if (current->pid != 0) {
            struct regs *fr = (struct regs *)old_rsp;
            if ((fr->cs & 3) == 3)
                current->utime++;
            else
                current->stime++;
        }
    }

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

    return task_switch_to(next);
}

// ring of recent switches for crash forensics: {pid, rsp, cs of the frame
// we are about to iretq into}
struct sw_rec { uint32_t pid; uint64_t rsp; uint64_t cs; };
static struct sw_rec sw_ring[8];
static int sw_head;
void task_dump_switches(void) {
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t);
    for (int i = 0; i < 8; i++) {
        int k = (sw_head + i) % 8;
        serial_puts(i ? "," : "[sw ");
        serial_puthex(sw_ring[k].pid);
        serial_puts("@");
        serial_puthex(sw_ring[k].rsp);
        serial_puts("/");
        serial_puthex(sw_ring[k].cs);
    }
    serial_puts("]\n");
}

// forensics: 1 if the frame lives on the task's kernel stack (or anywhere
// when it is the idle task, which runs on the boot stack)
int task_frame_owned(struct regs *fr) {
    uint64_t f = (uint64_t)fr;
    // idle: its own kstack slot or the boot stack (higher half via hhdm)
    if (current->pid == 0)
        return (f >= 0xffff800000000000ULL) ||
               (f >= current->kstack_top - KSTACK_SIZE &&
                f < current->kstack_top);
    return f >= current->kstack_top - KSTACK_SIZE && f < current->kstack_top;
}

void task_dump_inversion(struct regs *r) {
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t);
    serial_puts("[TICK-INVERSION cur=idle rip="); serial_puthex(r->rip);
    serial_puts(" rsp="); serial_puthex(r->rsp);
    serial_puts(" fr=");
    serial_puthex((uint64_t)r);
    task_dump_switches();
}

uint64_t task_switch_to(struct task *next) {
    // atomic switch: current, rsp0, cr3 and gs/fs must all move together.
    // interrupts stay off; every resume goes through iretq which restores
    // IF from the target's saved rflags
    cli();
    current = next;
    current->state = T_RUNNING;
    struct regs *fr = (struct regs *)next->rsp;
    sw_ring[sw_head].pid = (uint32_t)next->pid;
    sw_ring[sw_head].rsp = next->rsp;
    sw_ring[sw_head].cs = fr ? fr->cs : 0;
    sw_head = (sw_head + 1) % 8;
    // a switch into the idle must always resume ring-0 context; a user
    // frame on the idle means somebody corrupted idle->rsp — catch the
    // first occurrence instead of letting the cascade begin
    if (next->pid == 0 && (fr->cs & 3)) {
        extern void serial_puts(const char *);
        extern void serial_puthex(uint64_t);
        serial_puts("[FAKE-RESUME idle user frame rsp=");
        serial_puthex(next->rsp);
        serial_puts(" frip=");
        serial_puthex(fr->rip);
        serial_puts(" ");
        task_dump_switches();
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    tss_set_rsp0(current->kstack_top);
    vmm_switch(current->pml4);
    // fpu: eager save/restore on every switch. the kernel never uses
    // the fpu (built -mno-sse), so the live fpu always belongs to user
    // code; without this, parallel sse-heavy tasks (glibc) clobber each
    // other's xmm state at each tick (ld.so hash tables -> garbage)
    fpu_switch_to(next);
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
    current->tgid = 0;
    current->state = T_RUNNING;
    current->pml4 = vmm_kernel_pml4();
    // give the idle slot a REAL stack: stale rsp0/gs from this slot would
    // otherwise push into an unmapped page and triple-fault
    map_kstack(current);
    kgs_init(current);
    wrmsr(MSR_GS_BASE, current->kgs);
    tss_set_rsp0(current->kstack_top);
    next_pid = 1;
}

// -- fpu state --------------------------------------------------------------
// the kernel itself never touches sse (built -mno-sse), so the fpu always
// belongs to user code; glibc is sse-heavy, so its state must survive
// every preemption/task switch

static struct task *fpu_owner;

static const uint16_t fpu_cw = 0x037f;       // all exceptions masked
static const uint32_t fpu_mxcsr = 0x1f80;    // default sse control

void fpu_forget(struct task *t) {
    if (fpu_owner == t)
        fpu_owner = 0;
}

// eager fpu switch: called from task_switch_to with interrupts off
void fpu_switch_to(struct task *next) {
    if (fpu_owner == next)
        return;
    if (fpu_owner && fpu_owner->fpu_ready)
        __asm__ volatile("fxsave %0" :: "m"(fpu_owner->fpu_area) : "memory");
    fpu_owner = next;
    if (next->fpu_ready) {
        __asm__ volatile("fxrstor %0" :: "m"(next->fpu_area) : "memory");
    } else {
        __asm__ volatile("fninit");
        __asm__ volatile("fldcw %0" :: "m"(fpu_cw));
        __asm__ volatile("ldmxcsr %0" :: "m"(fpu_mxcsr));
        next->fpu_ready = 1;
    }
}

// flush the live fpu state of t into t->fpu_area (fork/clone snapshot);
// a no-op when t is not the current fpu owner or never used the fpu
void fpu_flush(struct task *t) {
    __asm__ volatile("clts");
    if (fpu_owner == t && t->fpu_ready)
        __asm__ volatile("fxsave %0" :: "m"(t->fpu_area) : "memory");
}

// compat: exec without a program name
uint64_t task_exec_current(struct vnode *vn) {
    return task_exec_current_named(vn, "prog");
}
