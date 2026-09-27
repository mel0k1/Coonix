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
#include "pit.h"

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

// release a page range [s, e) inside a region: writeback shared file pages,
// unmap, free frames (the file ref itself is dropped by the caller)
static void mmap_release_range(struct mmap_region *m, uint64_t s, uint64_t e) {
    if (m->file && (m->flags & MAP_SHARED)) {
        for (uint64_t va = s; va < e; va += PAGE_SIZE) {
            uint64_t pte = vmm_get_pte(current->pml4, va);
            if ((pte & VMM_PRESENT) && (pte & PTE_DIRTY)) {
                uint64_t foff = m->off + (va - m->start);
                m->file->vn->ops->write(m->file->vn,
                                        phys2virt(pte & 0x000ffffffffff000ULL),
                                        foff, PAGE_SIZE);
            }
        }
    }
    for (uint64_t va = s; va < e; va += PAGE_SIZE) {
        uint64_t phys = vmm_get_phys(current->pml4, va);
        vmm_unmap(current->pml4, va);
        if (phys)
            pmm_free((void *)phys);
    }
}

// split regions so that boundaries exist at s and e (no page changes);
// keeps the list sorted. returns 0 ok, -1 out of memory
static int mmap_ensure_bounds(uint64_t s, uint64_t e) {
    for (int pass = 0; pass < 2; pass++) {
        uint64_t at = pass == 0 ? s : e;
        for (struct mmap_region *m = current->mmaps; m; m = m->next) {
            if (!(m->start < at && at < m->end))
                continue;
            struct mmap_region *right = kmalloc(sizeof(*right));
            if (!right)
                return -1;
            *right = *m;                      // same file/flags/prot
            right->start = at;
            right->off += at - m->start;
            right->end = m->end;
            m->end = at;
            right->next = m->next;
            m->next = right;
            if (right->file)
                right->file->refs++;          // both halves own a ref
            m = right;   // 'at' can't also be inside right
        }
    }
    return 0;
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

    // overlaps are a bug unless MAP_FIXED, which carves out its range
    // (linux semantics: only [start, start+len) is replaced, the rest of
    // a straddling region stays)
    if (flags & MAP_FIXED) {
        if (mmap_ensure_bounds(start, start + len) < 0) {
            r->rax = -1ULL;
            return (uint64_t)r;
        }
        for (struct mmap_region **pp = &current->mmaps; *pp;) {
            struct mmap_region *m = *pp;
            if (m->start >= start + len || m->end <= start) {
                pp = &m->next;
                continue;
            }
            // fully inside thanks to the bounds above
            mmap_release_range(m, m->start, m->end);
            *pp = m->next;
            if (m->file)
                vfs_close(m->file);
            kfree(m);
        }
    } else {
        for (struct mmap_region *m = current->mmaps; m; m = m->next)
            if (start < m->end && m->start < start + len) {
                r->rax = -1ULL;
                return (uint64_t)r;
            }
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
    if (!len)
        len = PAGE_SIZE;

    // carve region boundaries, then drop every region inside [addr, addr+len)
    if (mmap_ensure_bounds(addr, addr + len) < 0) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    for (struct mmap_region **pp = &current->mmaps; *pp;) {
        struct mmap_region *m = *pp;
        if (m->start >= addr + len || m->end <= addr) {
            pp = &m->next;
            continue;
        }
        mmap_release_range(m, m->start, m->end);
        *pp = m->next;
        if (m->file)
            vfs_close(m->file);
        kfree(m);
    }
    r->rax = 0;
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
    // lazy pages filled later must inherit the new perms too
    uint64_t end = addr + pages * PAGE_SIZE;
    if (mmap_ensure_bounds(addr, end) < 0) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    for (struct mmap_region *m = current->mmaps; m; m = m->next)
        if (m->start >= addr && m->end <= end)
            m->prot = prot;
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
    const char *uname = (const char *)r->rdi;
    // kernel-side copy of the program name (argv[0]); user pages are
    // readable while the caller's cr3 is still active
    char name[96];
    int i = 0;
    while (i < (int)sizeof(name) - 1) {
        char c = uname[i];
        name[i++] = c;
        if (!c)
            break;
    }
    name[sizeof(name) - 1] = 0;

    struct vnode *vn = vfs_resolve_prog(name);
    if (!vn) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    uint64_t fr = task_exec_current_named(vn, name);
    if (!fr) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    return fr;
}

// --- glibc-facing odds and ends ---

struct iovec {
    const void *base;
    uint64_t len;
};

static uint64_t sys_writev(struct regs *r) {
    int fd = (int)r->rdi;
    const struct iovec *iv = (const struct iovec *)r->rsi;
    uint64_t cnt = r->rdx;
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        // reuse the write path by hand: fd may be the console
        struct file *f = fd_get(fd);
        if (!f) {
            r->rax = -1ULL;
            return (uint64_t)r;
        }
        const char *buf = iv[i].base;
        uint64_t len = iv[i].len;
        long n;
        if (!f->vn) {
            for (uint64_t j = 0; j < len; j++)
                console_putc(buf[j]);
            n = (long)len;
        } else {
            n = vfs_write(f, buf, len);
            if (n < 0) {
                r->rax = -1ULL;
                return (uint64_t)r;
            }
        }
        total += (uint64_t)n;
    }
    r->rax = total;
    return (uint64_t)r;
}

struct utsname_k {
    char sysname[65], nodename[65], release[65], version[65], machine[65],
         domainname[65];
};

static uint64_t sys_uname(struct regs *r) {
    struct utsname_k *u = (struct utsname_k *)r->rdi;
    if (!u) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Linux");       // glibc cares little, but be polite
    strcpy(u->nodename, "coonix");
    strcpy(u->release, "6.1.0-coonix");
    strcpy(u->version, "#1 SMP coonix");
    strcpy(u->machine, "x86_64");
    strcpy(u->domainname, "(none)");
    r->rax = 0;
    return (uint64_t)r;
}

// x86_64 glibc struct stat, 144 bytes
struct stat_k {
    uint64_t dev, ino, nlink;            // 24
    uint32_t mode, uid, gid, pad0;       // 16 -> 40
    uint64_t rdev, size, blksize, blocks;// 32 -> 72
    uint64_t atim_sec, atim_nsec;        // 16 -> 88
    uint64_t mtim_sec, mtim_nsec;        // 16 -> 104
    uint64_t ctim_sec, ctim_nsec;        // 16 -> 120
    uint64_t reserved[3];                // 24 -> 144
};

_Static_assert(sizeof(struct stat_k) == 144, "stat layout");

static uint64_t sys_fstat(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    struct stat_k *st = (struct stat_k *)r->rsi;
    if (!f || !st) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    memset(st, 0, sizeof(*st));
    if (!f->vn) {
        st->mode = 0x2000 | 0620;      // S_IFCHR | tty-ish perms
        st->blksize = 1024;
    } else {
        st->mode = 0x8000 | 0644;      // S_IFREG
        st->size = f->vn->size;
        st->blksize = 1024;
        st->nlink = 1;
        st->dev = f->vn->dev;
        st->ino = f->vn->ino;
    }
    r->rax = 0;
    return (uint64_t)r;
}

// ARCH_SET_FS 0x1002, ARCH_SET_GS 0x1001, ARCH_GET_FS 0x1003, ARCH_GET_GS 0x1004
static uint64_t sys_arch_prctl(struct regs *r) {
    uint64_t code = r->rdi, arg = r->rsi;
    switch (code) {
    case 0x1002:
        current->fs_base = arg;
        wrmsr(MSR_FS_BASE, arg);   // fs is free for user use
        r->rax = 0;
        break;
    case 0x1001:
        // gs is claimed by the kernel (syscall entry scratch); remember the
        // requested value but never load it
        current->gs_base = arg;
        r->rax = 0;
        break;
    case 0x1003:
        *(uint64_t *)arg = current->fs_base;
        r->rax = 0;
        break;
    case 0x1004:
        *(uint64_t *)arg = current->gs_base;
        r->rax = 0;
        break;
    default:
        r->rax = -1ULL;
    }
    return (uint64_t)r;
}

static uint64_t sys_set_tid_address(struct regs *r) {
    current->clear_tid = r->rdi;
    r->rax = (uint64_t)(long)current->pid;
    return (uint64_t)r;
}

// SYS_newfstatat: dirfd + path ("" or AT_EMPTY_PATH => stat the fd itself)
static uint64_t sys_newfstatat(struct regs *r) {
    struct stat_k *st = (struct stat_k *)r->rdx;
    const char *path = (const char *)r->rsi;
    int empty = !path || !path[0] || (r->r10 & 0x1000);   // AT_EMPTY_PATH
    if (!st || (!empty && !path)) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    struct vnode *vn = 0;
    if (empty) {
        struct file *f = fd_get((int)r->rdi);
        if (f)
            vn = f->vn;
    } else {
        vn = vfs_resolve(path);
    }
    if (!vn || vn->type != VNODE_FILE) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    memset(st, 0, sizeof(*st));
    st->mode = 0x8000 | 0644;      // S_IFREG
    st->size = vn->size;
    st->blksize = 1024;
    st->nlink = 1;
    st->dev = vn->dev;
    st->ino = vn->ino;
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_pread64(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    uint8_t *buf = (uint8_t *)r->rsi;
    uint64_t count = r->rdx;
    uint64_t off = r->r10;   // arg4 lives in r10
    if (!f || !f->vn) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    long n = f->vn->ops->read(f->vn, buf, off, count);
    r->rax = n < 0 ? -1ULL : (uint64_t)n;
    return (uint64_t)r;
}

static uint64_t sys_getrandom(struct regs *r) {
    uint8_t *buf = (uint8_t *)r->rdi;
    uint64_t len = r->rsi;
    uint64_t seed = pit_ticks() ^ (r->rsp << 17);
    for (uint64_t i = 0; i < len; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(seed >> 33);
    }
    r->rax = len;
    return (uint64_t)r;
}

static uint64_t sys_openat(struct regs *r) {
    // dirfd ignored: only absolute paths resolve anyway
    struct regs tmp = *r;
    tmp.rdi = r->rsi;   // path
    tmp.rsi = r->rdx;   // flags
    (void)sys_open(&tmp);
    // sys_open returned a frame rsp; the result itself is in tmp.rax
    r->rax = tmp.rax;
    return (uint64_t)r;
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
    case SYS_writev:  return sys_writev(r);
    case SYS_open:    return sys_open(r);
    case SYS_openat:  return sys_openat(r);
    case SYS_close:   return sys_close(r);
    case SYS_fstat:   return sys_fstat(r);
    case SYS_fstatat: return sys_newfstatat(r);
    case SYS_pread64: return sys_pread64(r);
    case SYS_mmap:    return sys_mmap(r);
    case SYS_mprotect: return sys_mprotect(r);
    case SYS_munmap:  return sys_munmap(r);
    case SYS_brk:     return sys_brk(r);
    case SYS_getpid:  r->rax = current->pid; return (uint64_t)r;
    case SYS_gettid:  r->rax = current->pid; return (uint64_t)r;
    case SYS_getuid: case SYS_getgid: case SYS_geteuid:
    case SYS_getegid: r->rax = 0; return (uint64_t)r;
    case SYS_uname:   return sys_uname(r);
    case SYS_fork: {
        struct task *c = task_fork(r);
        r->rax = c ? (uint64_t)(long)c->pid : -1ULL;
        return (uint64_t)r;
    }
    case SYS_execve:  return sys_execve(r);
    case SYS_exit:
    case SYS_exit_group: return task_exit_current((int)r->rdi);
    case SYS_wait4:   return sys_wait4(r);
    case SYS_arch_prctl: return sys_arch_prctl(r);
    case SYS_set_tid_address: return sys_set_tid_address(r);
    case SYS_set_robust_list: r->rax = 0; return (uint64_t)r;
    case SYS_getrandom: return sys_getrandom(r);
    default:
        r->rax = -1ULL;
        return (uint64_t)r;
}
}
