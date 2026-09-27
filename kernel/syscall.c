#include "syscall.h"
#include "task.h"
#include "kbd.h"
#include "console.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "gdt.h"
#include "heap.h"
#include "kernel.h"
#include "vfs.h"

// every handler sets r->rax and returns current frame rsp;
// blocking ones return a switched rsp instead

#define MAP_PRIVATE    0x02
#define MAP_SHARED     0x01
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

#define O_CREAT 0x40
#define O_TRUNC 0x200

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define PTE_DIRTY  0x040

static struct file *fd_get(int fd) {
    if (fd < 0 || fd >= FILE_MAX)
        return 0;
    return current->fds[fd];
}

static uint64_t sys_write(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    const char *buf = (const char *)r->rsi;
    uint64_t len = r->rdx;
    if (!f) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    if (!f->vn) { // console
        for (uint64_t i = 0; i < len; i++)
            console_putc(buf[i]);
        r->rax = len;
        return (uint64_t)r;
    }
    long n = vfs_write(f, buf, len);
    r->rax = n < 0 ? -1ULL : (uint64_t)n;
    return (uint64_t)r;
}

static uint64_t sys_read(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    char *buf = (char *)r->rsi;
    if (!f) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    if (!f->vn) { // console: blocking single-char read
        char c = kbd_getchar();
        if (c < 0) {
            // sleep until keypress; int 0x80 replays on wake
            current->wait_reason = WAIT_KBD;
            current->state = T_BLOCKED;
            return task_schedule((uint64_t)r);
        }
        buf[0] = c;
        r->rax = 1;
        return (uint64_t)r;
    }
    long n = vfs_read(f, buf, r->rdx);
    r->rax = n < 0 ? -1ULL : (uint64_t)n;
    return (uint64_t)r;
}

static uint64_t sys_open(struct regs *r) {
    const char *path = (const char *)r->rdi;
    int flags = (int)r->rsi;
    struct vnode *vn = vfs_resolve(path);
    if (!vn && (flags & O_CREAT)) {
        vn = vfs_create(path);
        if (!vn) {
            r->rax = -1ULL;
            return (uint64_t)r;
        }
    }
    if (!vn) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    if ((flags & O_TRUNC) && vn->type == VNODE_FILE)
        vfs_truncate(vn);
    struct file *f = vfs_open(vn);
    if (!f) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    int fd = task_fd_alloc(f);
    if (fd < 0) {
        vfs_close(f);
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    r->rax = fd;
    return (uint64_t)r;
}

static uint64_t sys_close(struct regs *r) {
    int fd = (int)r->rdi;
    struct file *f = fd_get(fd);
    if (!f) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    current->fds[fd] = 0;
    vfs_close(f);
    r->rax = 0;
    return (uint64_t)r;
}

// mmap: anon args arrive in rdi/rsi/rdx/r10, fd in r8, offset in r9
static uint64_t sys_mmap(struct regs *r) {
    uint64_t addr = r->rdi, len = r->rsi, prot = r->rdx, flags = r->r10;
    int fd = (int)r->r8;
    uint64_t off = r->r9;
    struct file *file = 0;
    uint64_t pml4 = current->pml4;

    if (!len || len > 0x40000000) {          // cap at 1 GiB per call
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    if (addr & 0xfff) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    if (addr && !(flags & MAP_FIXED)) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }

    if (!(flags & MAP_ANONYMOUS)) {
        // file-backed: fd must name a regular file, offset page aligned
        file = fd_get(fd);
        if (!file || !file->vn || file->vn->type != VNODE_FILE) {
            r->rax = -1ULL;
            return (uint64_t)r;
        }
        if (off & 0xfff) {
            r->rax = -1ULL;
            return (uint64_t)r;
        }
    }

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // pick a spot: first gap after USER_MMAP_BASE, or the tail
    uint64_t start = 0;
    if (flags & MAP_FIXED) {
        start = addr;
    } else {
        uint64_t hint = USER_MMAP_BASE;
        struct mmap_region *m = current->mmaps;
        for (; m; m = m->next) {
            if (m->start - hint >= len)
                break;
            hint = m->end;
        }
        start = hint;
    }

    // overlaps are always a bug, fixed or not
    for (struct mmap_region *m = current->mmaps; m; m = m->next)
        if (start < m->end && m->start < start + len) {
            r->rax = -1ULL;
            return (uint64_t)r;
        }

    // anonymous: map eagerly, zeroed. file-backed: record only, pages
    // appear on #PF (task_mmap_fault)
    if (!file) {
        uint64_t vflags = VMM_PRESENT | VMM_USER;
        if (prot & PROT_WRITE)
            vflags |= VMM_WRITE;
        if (!(prot & PROT_EXEC))
            vflags |= VMM_NX;
        for (uint64_t va = start; va < start + len; va += PAGE_SIZE) {
            void *p = pmm_alloc_zeroed();
            if (!p)
                panic("mmap: out of pages");
            vmm_map(pml4, va, (uint64_t)p, vflags);
        }
    }

    struct mmap_region *m = kmalloc(sizeof(*m));
    if (!m)
        panic("mmap: out of kernel heap");
    m->start = start;
    m->end = start + len;
    m->file = file;
    if (file)
        file->refs++;   // region owns a reference until munmap/teardown
    m->off = off;
    m->prot = prot;
    m->flags = flags;
    // keep the list sorted by start
    struct mmap_region **pp = &current->mmaps;
    while (*pp && (*pp)->start < start)
        pp = &(*pp)->next;
    m->next = *pp;
    *pp = m;

    r->rax = start;
    return (uint64_t)r;
}

static uint64_t sys_munmap(struct regs *r) {
    uint64_t addr = r->rdi, len = r->rsi;
    if (addr & 0xfff) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (struct mmap_region **pp = &current->mmaps; *pp; pp = &(*pp)->next) {
        struct mmap_region *m = *pp;
        if (m->start != addr || m->end != addr + len)
            continue;   // only whole-region munmap is supported

        // shared file mapping: flush dirty pages back to the file
        if (m->file && (m->flags & MAP_SHARED)) {
            for (uint64_t va = m->start; va < m->end; va += PAGE_SIZE) {
                uint64_t pte = vmm_get_pte(current->pml4, va);
                if ((pte & VMM_PRESENT) && (pte & PTE_DIRTY)) {
                    uint64_t foff = m->off + (va - m->start);
                    m->file->vn->ops->write(m->file->vn,
                                            phys2virt(pte & 0x000ffffffffff000ULL),
                                            foff, PAGE_SIZE);
                }
            }
        }

        // unmap and release physical pages
        for (uint64_t va = m->start; va < m->end; va += PAGE_SIZE) {
            uint64_t phys = vmm_get_phys(current->pml4, va);
            vmm_unmap(current->pml4, va);
            if (phys)
                pmm_free((void *)phys);
        }
        if (m->file)
            vfs_close(m->file);
        *pp = m->next;
        kfree(m);
        r->rax = 0;
        return (uint64_t)r;
    }
    r->rax = -1ULL;
    return (uint64_t)r;
}

static uint64_t sys_mprotect(struct regs *r) {
    uint64_t addr = r->rdi, len = r->rsi, prot = r->rdx;
    if ((addr & 0xfff) || !len) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t vflags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE)
        vflags |= VMM_WRITE;
    if (!(prot & PROT_EXEC))
        vflags |= VMM_NX;
    vmm_mprotect(current->pml4, addr, pages, vflags);
    r->rax = 0;
    return (uint64_t)r;
}

// brk(0) queries; grow maps zero pages, shrink unmaps
static uint64_t sys_brk(struct regs *r) {
    uint64_t want = r->rdi;
    uint64_t cur = current->brk_cur;
    if (!want) {
        r->rax = cur;
        return (uint64_t)r;
    }
    if (want < current->brk_base) {
        r->rax = cur;   // refuse
        return (uint64_t)r;
    }
    uint64_t pml4 = current->pml4;
    if (want > cur) {
        for (uint64_t va = (cur + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
             va < want; va += PAGE_SIZE) {
            void *p = pmm_alloc_zeroed();
            if (!p)
                panic("brk: out of pages");
            vmm_map(pml4, va, (uint64_t)p,
                    VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
        }
        current->brk_cur = want;
        r->rax = want;
    } else {
        for (uint64_t va = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
             va < cur; va += PAGE_SIZE) {
            uint64_t phys = vmm_get_phys(pml4, va);
            vmm_unmap(pml4, va);
            if (phys)
                pmm_free((void *)phys);
        }
        current->brk_cur = want;
        r->rax = want;
    }
    return (uint64_t)r;
}

static uint64_t sys_execve(struct regs *r) {
    const char *name = (const char *)r->rdi;
    struct vnode *vn = vfs_resolve_prog(name);
    if (!vn) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    uint64_t fr = task_exec_current(vn);
    if (!fr) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    return fr;
}

static uint64_t sys_wait4(struct regs *r) {
    int *status = (int *)r->rsi;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_ZOMBIE && t->parent == current) {
            if (status)
                *status = t->exit_code;
            r->rax = t->pid;
            t->state = T_FREE;
            return (uint64_t)r;
        }
    }
    // no zombie child yet: sleep until one exits
    current->wait_reason = WAIT_CHILD;
    current->state = T_BLOCKED;
    return task_schedule((uint64_t)r);
}

uint64_t syscall_dispatch(struct regs *r) {
    switch (r->rax) {
    case SYS_read:    return sys_read(r);
    case SYS_write:   return sys_write(r);
    case SYS_open:    return sys_open(r);
    case SYS_close:   return sys_close(r);
    case SYS_mmap:    return sys_mmap(r);
    case SYS_mprotect: return sys_mprotect(r);
    case SYS_munmap:  return sys_munmap(r);
    case SYS_brk:     return sys_brk(r);
    case SYS_getpid:  r->rax = current->pid; return (uint64_t)r;
    case SYS_fork: {
        struct task *c = task_fork(r);
        r->rax = c ? (uint64_t)(long)c->pid : -1ULL;
        return (uint64_t)r;
    }
    case SYS_execve:  return sys_execve(r);
    case SYS_exit:    return task_exit_current((int)r->rdi);
    case SYS_wait4:   return sys_wait4(r);
    default:
        r->rax = -1ULL;
        return (uint64_t)r;
    }
}
