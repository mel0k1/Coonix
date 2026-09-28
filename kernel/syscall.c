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
#include "signal.h"
#include "futex.h"
#include "serial.h"

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

// errno values the kernel returns as negative results (linux abi)
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define ENOTBLK 15
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define ENOSYS  38
#define ENOTTY  25
#define ETIMEDOUT 110
#define EMFILE  24
#define EIO     5

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// clone flags
#define CLONE_VM             0x100
#define CLONE_THREAD         0x10000
#define CLONE_SETTLS         0x80000
#define CLONE_PARENT_SETTID  0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID   0x01000000

// clockids
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

// int 0x80 pushes rip AFTER the int insn; native syscall (LSTAR) leaves
// rip AFTER the syscall insn too (rcx). when a task blocks and its frame
// is later resumed, make it re-execute the syscall instead of returning
// whatever rax held at entry — wake paths either re-execute (kbd/child
// wakes, which never touch rax) or unwind this rewind when they stuff
// the result (futex/nanosleep, see task_frame_syscall_result)
static void replay_fixup(struct regs *r) {
    if (r->int_no == 128 || r->int_no == 64)
        r->rip -= 2;        // CD 80 / 0F 05
}

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
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (!f->vn) { // console
        for (uint64_t i = 0; i < len; i++)
            console_putc(buf[i]);
        r->rax = len;
        return (uint64_t)r;
    }
    long n = vfs_write(f, buf, len);
    r->rax = n < 0 ? (uint64_t)-EIO : (uint64_t)n;
    return (uint64_t)r;
}

static uint64_t sys_read(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    char *buf = (char *)r->rsi;
    if (!f) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (!f->vn) { // console: line discipline read
        long n = tty_read(buf, r->rdx ? (int)r->rdx : 1);
        if (n < 0) {
            // sleep until a key arrives; replay the syscall on wake.
            // cli first: the blocked state + saved frame + schedule pick
            // must be atomic vs the tick, or the tick re-saves a kernel
            // context over our syscall frame and the task never returns
            // to user (schedulable ghost)
            cli();
            replay_fixup(r);
            current->wait_reason = WAIT_KBD;
            current->state = T_BLOCKED;
            return task_schedule((uint64_t)r);
        }
        r->rax = (uint64_t)n;
        return (uint64_t)r;
    }
    long n = vfs_read(f, buf, r->rdx);
    r->rax = n < 0 ? (uint64_t)-EIO : (uint64_t)n;
    return (uint64_t)r;
}

static uint64_t sys_open(struct regs *r) {
    const char *path = (const char *)r->rdi;
    int flags = (int)r->rsi;
    struct vnode *vn = vfs_resolve(path);
    if (!vn && (flags & O_CREAT)) {
        vn = vfs_create(path);
        if (!vn) {
            r->rax = -ENOENT;
            return (uint64_t)r;
        }
    }
    if (!vn) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    if ((flags & O_TRUNC) && vn->type == VNODE_FILE)
        vfs_truncate(vn);
    struct file *f = vfs_open(vn);
    if (!f) {
        r->rax = -ENOMEM;
        return (uint64_t)r;
    }
    int fd = task_fd_alloc(f);
    if (fd < 0) {
        vfs_close(f);
        r->rax = -EMFILE;
        return (uint64_t)r;
    }
    r->rax = fd;
    return (uint64_t)r;
}

static uint64_t sys_close(struct regs *r) {
    int fd = (int)r->rdi;
    struct file *f = fd_get(fd);
    if (!f) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    current->fds[fd] = 0;
    vfs_close(f);
    r->rax = 0;
    return (uint64_t)r;
}

// lseek: file fds keep an offset in struct file; console is a pipe
static uint64_t sys_lseek(struct regs *r) {
    int fd = (int)r->rdi;
    long off = (long)r->rsi;
    int whence = (int)r->rdx;
    struct file *f = fd_get(fd);
    if (!f) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (!f->vn) {
        r->rax = -ESPIPE;
        return (uint64_t)r;
    }
    uint64_t target;
    switch (whence) {
    case SEEK_SET: target = (uint64_t)off; break;
    case SEEK_CUR: target = f->off + off; break;
    case SEEK_END: target = f->vn->size + off; break;
    default:
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    f->off = target;
    r->rax = target;
    return (uint64_t)r;
}

// --- ioctl: terminal ioctls on console fds ---

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413

struct winsize_k {
    uint16_t row, col, xpixel, ypixel;
};

static uint64_t sys_ioctl(struct regs *r) {
    int fd = (int)r->rdi;
    uint64_t req = r->rsi;
    void *arg = (void *)r->rdx;
    struct file *f = fd_get(fd);
    if (!f) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (f->vn) {                    // regular files: no tty ioctls
        r->rax = -ENOTTY;
        return (uint64_t)r;
    }
    switch (req) {
    case TCGETS:
        memcpy(arg, &tty_termios, sizeof(tty_termios));
        r->rax = 0;
        return (uint64_t)r;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        memcpy(&tty_termios, arg, sizeof(tty_termios));
        r->rax = 0;
        return (uint64_t)r;
    case TIOCGWINSZ: {
        struct winsize_k *ws = arg;
        ws->row = 25;
        ws->col = 80;
        ws->xpixel = 0;
        ws->ypixel = 0;
        r->rax = 0;
        return (uint64_t)r;
    }
    default:
        r->rax = -ENOTTY;
        return (uint64_t)r;
    }
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
    long want = (long)r->rdi;          // pid filter: -1 = any child, 0 =
                                       // any child in my group (same here)
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_ZOMBIE && t->parent == current &&
            (want <= 0 || t->pid == want)) {
            if (status)
                *status = t->sig_death ? t->exit_code
                                       : (t->exit_code & 0xff) << 8;
            r->rax = t->pid;
            t->state = T_FREE;
            return (uint64_t)r;
        }
    }
    // no zombie child yet: sleep until one exits (cli: see sys_read)
    cli();
    replay_fixup(r);
    current->wait_reason = WAIT_CHILD;
    current->state = T_BLOCKED;
    return task_schedule((uint64_t)r);
}

// --- glibc depth: clone/futex/signals ---

// clone: pthread_create path (CLONE_THREAD) or plain fork fallback
static uint64_t sys_clone(struct regs *r) {
    uint64_t flags = r->rdi;
    uint64_t newsp = r->rsi;
    uint64_t parent_tid = r->rdx;
    uint64_t child_tid = r->r10;
    uint64_t tls = r->r8;
    struct task *c;
    if (flags & CLONE_THREAD) {
        c = task_clone_thread(r, flags, newsp, parent_tid, child_tid, tls);
        if (!c) {
            sti();
            r->rax = -EAGAIN;
            return (uint64_t)r;
        }
        // pathological preemption race: the child already ran and exited.
        // report success and stay in the parent
        if (c->state == T_FREE) {
            sti();
            r->rax = (uint64_t)(long)c->pid;
            return (uint64_t)r;
        }
        // parent returns the child's tid, child returns 0 (frame copy)
        r->rax = (uint64_t)(long)c->pid;
        // save the parent frame, mark it runnable, switch into the child;
        // both exits re-enable interrupts via their iretq
        current->rsp = (uint64_t)r;
        current->state = T_READY;
        return task_switch_to(c);
    }
    c = task_fork(r);
    r->rax = c ? (uint64_t)(long)c->pid : (uint64_t)-EAGAIN;
    return (uint64_t)r;
}

static uint64_t sys_futex(struct regs *r) {
    uint32_t *uaddr = (uint32_t *)r->rdi;
    int op = (int)r->rsi & ~0x180;      // strip PRIVATE (128) + CLOCK_REALTIME (256)
    uint32_t val = (uint32_t)r->rdx;
    const void *timeout = (const void *)r->r10;
    uint32_t *uaddr2 = (uint32_t *)r->r8;
    uint64_t key = futex_key_of((uint64_t)uaddr);
    if (!key) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    switch (op) {
    case FUT_WAIT:
    case FUT_WAIT_BITSET: {
        if (*uaddr != val) {
            r->rax = -EAGAIN;
            return (uint64_t)r;
        }
        // WAIT: relative timespec; WAIT_BITSET: absolute.
        // rewind so the resume re-executes (or the wake unwinds it after
        // stuffing the result); futex_wait_key returns the frame rsp the
        // entry asm iretqs from — never touch r after this point
        cli();
        replay_fixup(r);
        uint64_t deadline = futex_deadline_from_timespec(
            timeout, op == FUT_WAIT_BITSET);
        return futex_wait_key(key, deadline, (uint64_t)r);
    }
    case FUT_WAKE:
    case FUT_WAKE_BITSET:
        r->rax = (uint64_t)futex_wake_key(key, (int)val);
        return (uint64_t)r;
    case FUT_REQUEUE:
    case FUT_CMP_REQUEUE:
        if (*uaddr != val) {
            r->rax = -EAGAIN;
            return (uint64_t)r;
        }
        r->rax = (uint64_t)futex_requeue_key(key, futex_key_of((uint64_t)uaddr2),
                                             (int)r->r10);
        return (uint64_t)r;
    default:
        r->rax = (uint64_t)-ENOSYS;
        return (uint64_t)r;
    }
}

static uint64_t sys_rt_sigaction_w(struct regs *r) {
    long ret = signal_sys_rt_sigaction((int)r->rdi,
                                       (const struct k_sigaction *)r->rsi,
                                       (struct k_sigaction *)r->rdx, r->r10);
    r->rax = (uint64_t)ret;
    return (uint64_t)r;
}

static uint64_t sys_rt_sigprocmask_w(struct regs *r) {
    long ret = signal_sys_rt_sigprocmask((int)r->rdi, (const uint64_t *)r->rsi,
                                         (uint64_t *)r->rdx, r->r10);
    r->rax = (uint64_t)ret;
    return (uint64_t)r;
}

static uint64_t sys_kill(struct regs *r) {
    long pid = (long)r->rdi;
    int sig = (int)r->rsi;
    if (sig < 1 || sig > SIG_MAX) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    int ret = signal_send_group((int)pid, sig);
    r->rax = ret == 0 ? 0 : (uint64_t)-ESRCH;
    return (uint64_t)r;
}

static uint64_t sys_tgkill(struct regs *r) {
    long tgid = (long)r->rdi;
    long tid = (long)r->rsi;
    int sig = (int)r->rdx;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state != T_FREE && t->pid == tid && t->tgid == tgid) {
            int ret = signal_send_task(t, sig);
            r->rax = ret == 0 ? 0 : (uint64_t)-EINVAL;
            return (uint64_t)r;
        }
    }
    r->rax = (uint64_t)-ESRCH;
    return (uint64_t)r;
}

static uint64_t sys_tkill(struct regs *r) {
    long tid = (long)r->rdi;
    int sig = (int)r->rsi;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state != T_FREE && t->pid == tid) {
            int ret = signal_send_task(t, sig);
            r->rax = ret == 0 ? 0 : (uint64_t)-EINVAL;
            return (uint64_t)r;
        }
    }
    r->rax = (uint64_t)-ESRCH;
    return (uint64_t)r;
}

// nanosleep: block until the tick deadline
static uint64_t sys_nanosleep(struct regs *r) {
    const uint64_t *ts = (const uint64_t *)r->rdi;
    if (!ts) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    uint64_t t = ts[0] * 100 + ts[1] / 10000000;
    if (!t)
        t = 1;
    cli();               // blocking window must be atomic (see sys_read)
    replay_fixup(r);
    current->wake_tick = pit_ticks() + t;
    current->wait_reason = WAIT_SLEEP;
    current->state = T_BLOCKED;
    return task_schedule((uint64_t)r);
}

static uint64_t sys_clock_gettime(struct regs *r) {
    uint64_t *tp = (uint64_t *)r->rsi;
    int clk = (int)r->rdi;
    if (!tp || (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    uint64_t t = pit_ticks();
    tp[0] = t / 100;
    tp[1] = (t % 100) * 10000000ULL;
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_time(struct regs *r) {
    uint64_t *tp = (uint64_t *)r->rdi;
    uint64_t t = pit_ticks() / 100;
    if (tp)
        *tp = t;
    r->rax = t;
    return (uint64_t)r;
}

static uint64_t sys_getppid(struct regs *r) {
    r->rax = current->parent ? (uint64_t)(long)current->parent->tgid : 0;
    return (uint64_t)r;
}

// prlimit64: report an 8 MiB stack, infinity elsewhere; sets are ignored
static uint64_t sys_prlimit64(struct regs *r) {
    struct rlim64 { uint64_t cur, max; };
    struct rlim64 *old = (struct rlim64 *)r->r10;
    if (old) {
        old->max = ~0ULL;
        old->cur = (int)r->rsi == 3 ? 8ULL << 20 : ~0ULL;   // RLIMIT_STACK
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_sched_getaffinity(struct regs *r) {
    uint64_t *mask = (uint64_t *)r->rsi;
    if (mask) {
        mask[0] = 1;        // one cpu
    }
    r->rax = 8;
    return (uint64_t)r;
}

static uint64_t sys_madvise(struct regs *r) {
    (void)r;
    r->rax = 0;
    return (uint64_t)r;
}

uint64_t syscall_dispatch(struct regs *r) {
    // entry guard: a syscall arriving while the scheduler thinks the idle
    // task is current means a context escaped from the scheduler — catch
    // it here with full state instead of iretq-ing into limbo
    if (current->pid == 0) {
        extern void serial_puts(const char *);
        extern void serial_puthex(uint64_t);
        uint64_t gsbase;
        __asm__ volatile("rdmsr" : "=A"(gsbase) : "c"(0xC0000101));
        serial_puts("[REDLINE-ENTRY syscall cur=idle nr=");
        serial_puthex(r->rax);
        serial_puts(" gsbase=");
        serial_puthex(gsbase);
        serial_puts(" rip=");
        serial_puthex(r->rip);
        serial_puts(" intno=");
        serial_puthex(r->int_no);
        serial_puts(" frame=");
        serial_puthex((uint64_t)r);
        extern void task_dump_switches(void);
        serial_puts(" \n");
        task_dump_switches();
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
            serial_puts(" pml4=");
            serial_puthex(t->pml4);
            serial_puts("\n");
        }
        serial_puts("[HALT]\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    // pending signals land before the syscall: the handler frame replaces
    // ours and rt_sigreturn replays the syscall afterwards (SA_RESTART
    // style). the syscall itself must not run with rewritten registers
    uint64_t fr;
    int act = signal_deliver(r, &fr);
    if (act)
        return fr;   // 1: handler frame in place; 2: killed, already scheduled

    switch (r->rax) {
    case SYS_read:    fr = sys_read(r); break;
    case SYS_write:   fr = sys_write(r); break;
    case SYS_writev:  fr = sys_writev(r); break;
    case SYS_open:    fr = sys_open(r); break;
    case SYS_openat:  fr = sys_openat(r); break;
    case SYS_close:   fr = sys_close(r); break;
    case SYS_lseek:   fr = sys_lseek(r); break;
    case SYS_ioctl:   fr = sys_ioctl(r); break;
    case SYS_fstat:   fr = sys_fstat(r); break;
    case SYS_fstatat: fr = sys_newfstatat(r); break;
    case SYS_pread64: fr = sys_pread64(r); break;
    case SYS_mmap:    fr = sys_mmap(r); break;
    case SYS_mprotect: fr = sys_mprotect(r); break;
    case SYS_munmap:  fr = sys_munmap(r); break;
    case SYS_madvise: fr = sys_madvise(r); break;
    case SYS_brk:     fr = sys_brk(r); break;
    case SYS_futex:   fr = sys_futex(r); break;
    case SYS_getpid:  r->rax = (uint64_t)(long)current->tgid; fr = (uint64_t)r; break;
    case SYS_gettid:  r->rax = (uint64_t)(long)current->pid; fr = (uint64_t)r; break;
    case SYS_getppid: fr = sys_getppid(r); break;
    case SYS_getuid: case SYS_getgid: case SYS_geteuid:
    case SYS_getegid: r->rax = 0; fr = (uint64_t)r; break;
    case SYS_uname:   fr = sys_uname(r); break;
    case SYS_fork: {
        struct task *c = task_fork(r);
        r->rax = c ? (uint64_t)(long)c->pid : (uint64_t)-EAGAIN;
        fr = (uint64_t)r;
        break;
    }
    case SYS_vfork: {
        // no shared-mmu vfork: plain fork is close enough for libc users
        struct task *c = task_fork(r);
        r->rax = c ? (uint64_t)(long)c->pid : (uint64_t)-EAGAIN;
        fr = (uint64_t)r;
        break;
    }
    case SYS_clone:   fr = sys_clone(r); break;
    case SYS_execve:  fr = sys_execve(r); break;
    case SYS_exit:    fr = task_exit_current((int)r->rdi); break;
    case SYS_exit_group: fr = task_exit_current_group((int)r->rdi); break;
    case SYS_wait4:   fr = sys_wait4(r); break;
    case SYS_rt_sigaction:  fr = sys_rt_sigaction_w(r); break;
    case SYS_rt_sigprocmask: fr = sys_rt_sigprocmask_w(r); break;
    case SYS_rt_sigreturn:  return signal_sigreturn(r);   // restores mask
    case SYS_kill:    fr = sys_kill(r); break;
    case SYS_tkill:   fr = sys_tkill(r); break;
    case SYS_tgkill:  fr = sys_tgkill(r); break;
    case SYS_set_tid_address: fr = sys_set_tid_address(r); break;
    case SYS_set_robust_list: r->rax = 0; fr = (uint64_t)r; break;
    case SYS_arch_prctl: fr = sys_arch_prctl(r); break;
    case SYS_nanosleep: fr = sys_nanosleep(r); break;
    case SYS_clock_gettime: fr = sys_clock_gettime(r); break;
    case SYS_time:    fr = sys_time(r); break;
    case SYS_sched_getaffinity: fr = sys_sched_getaffinity(r); break;
    case SYS_prlimit64: fr = sys_prlimit64(r); break;
    case SYS_getrandom: fr = sys_getrandom(r); break;
    default:
        r->rax = (uint64_t)-ENOSYS;
        fr = (uint64_t)r;
        break;
    }

    // signals that arrived while the syscall ran
    uint64_t fr2;
    signal_deliver((struct regs *)fr, &fr2);
    if (!task_frame_owned((struct regs *)fr2)) {
        extern void serial_puts(const char *);
        extern void serial_puthex(uint64_t);
        serial_puts("[FRAME-MISMATCH cur=");
        serial_puthex((uint64_t)current->pid);
        serial_puts(" fr2="); serial_puthex(fr2);
        serial_puts(" kt="); serial_puthex(current->kstack_top);
        serial_puts("]\n");
    }
    // red line (covers BOTH the native syscall entry and int 0x80): never
    // iretq to ring 3 while the scheduler thinks the idle task is current
    if (((struct regs *)fr2)->cs & 3 && current->pid == 0) {
        extern void serial_puts(const char *);
        extern void serial_puthex(uint64_t);
        struct regs *bad = (struct regs *)fr2;
        serial_puts("[REDLINE syscall->user cur=idle nr=");
        serial_puthex(bad->rax);
        serial_puts(" fr=");
        serial_puthex(fr2);
        serial_puts(" rip=");
        serial_puthex(bad->rip);
        serial_puts(" \n");
        extern void task_dump_switches(void);
        task_dump_switches();
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
            serial_puts("\n");
        }
        serial_puts("[HALT]\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    return fr2;
}
