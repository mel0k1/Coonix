#include "syscall.h"
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
#include "net.h"
#include "futex.h"
#include "serial.h"
#include "pipe.h"
#include "task.h"

// every handler sets r->rax and returns current frame rsp;
// blocking ones return a switched rsp instead

#define MAP_PRIVATE    0x02
#define MAP_SHARED     0x01
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// top of the user half: anything at or above belongs to the kernel
#define USER_LIMIT     0x800000000000ULL

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define PTE_DIRTY  0x040

// user path capture: copy the string, make absolute against cwd.
// returns 0 ok, -1 bad pointer/empty
static int user_path(uint64_t uptr, char *out, int cap) {
    const char *up = (const char *)uptr;
    if (!up)
        return -1;
    char tmp[256];
    int i = 0;
    while (i < (int)sizeof(tmp) - 1) {
        char c = up[i];
        tmp[i++] = c;
        if (!c)
            break;
    }
    tmp[sizeof(tmp) - 1] = 0;
    if (!tmp[0])
        return -1;
    if (tmp[0] == '/') {
        strncpy(out, tmp, cap - 1);
        out[cap - 1] = 0;
        return 0;
    }
    // relative: cwd + "/" + tmp
    int n = 0;
    const char *cwd = current->cwd;
    while (cwd[n] && n < cap - 1) {
        out[n] = cwd[n];
        n++;
    }
    if (n && out[n - 1] != '/' && n < cap - 1)
        out[n++] = '/';
    for (int k = 0; tmp[k] && n < cap - 1; k++)
        out[n++] = tmp[k];
    out[n] = 0;
    return 0;
}

// in-place canonicalization of an absolute path: drop "." and "x/.."
static void path_canon(char *p) {
    if (!p || p[0] != '/')
        return;
    char tmp[256];
    strncpy(tmp, p, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    const char *comps[64];
    uint16_t clen[64];
    int n = 0;
    const char *s = tmp;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        const char *start = s;
        while (*s && *s != '/')
            s++;
        uint64_t len = (uint64_t)(s - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n)
                n--;
            continue;
        }
        if (n < 64) {
            comps[n] = start;
            clen[n] = (uint16_t)len;
            n++;
        }
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        p[w++] = '/';
        for (int k = 0; k < clen[i]; k++)
            p[w++] = comps[i][k];
    }
    if (!w)
        p[w++] = '/';
    p[w] = 0;
}

// capture a NUL-terminated array of user strings into one kernel block
// ("str\0str\0...\0"); returns 0 ok, -1 oversized, -2 bad user pointer,
// -3 out of memory
#define EXEC_STRV_MAX 32
// per-block byte budget: argv + envp + the auxv block must fit the
// 64k user stack with room to spare
#define EXEC_STRV_BYTES (16 << 10)
static int capture_strv(uint64_t up, char **out, int *cnt) {
    *out = 0;
    *cnt = 0;
    if (!up)
        return 0;
    const uint64_t *uv = (const uint64_t *)up;
    const char *strs[EXEC_STRV_MAX];
    uint64_t lens[EXEC_STRV_MAX];
    int n = 0;
    uint64_t total = 0;
    while (n < EXEC_STRV_MAX) {
        // the vector itself is user memory: check every entry we read
        if (!task_user_range_ok(current, (uint64_t)&uv[n], 8, 0))
            return -2;
        uint64_t p = uv[n];
        if (!p)
            break;
        const char *s = (const char *)p;
        // strlen page by page: touching an unmapped page from the kernel
        // would fault (SIGSEGV kill) instead of failing the syscall
        uint64_t len = 0;
        while (len < 1024) {
            if ((len & 0xfff) == 0 &&
                !task_user_range_ok(current, (uint64_t)s + len, 1, 0))
                return -2;
            if (!s[len])
                break;
            len++;
        }
        if (len >= 1024)
            return -1;
        strs[n] = s;
        lens[n] = len + 1;
        total += len + 1;
        if (total > EXEC_STRV_BYTES)
            return -1;
        n++;
    }
    if (!n)
        return 0;
    // +1: double NUL at the end — consumers scan strings until an empty
    // one, so the block must end "str\0str\0\0"
    char *blk = kmalloc(total + 1);
    if (!blk)
        return -3;
    uint64_t off = 0;
    for (int i = 0; i < n; i++) {
        memcpy(blk + off, strs[i], lens[i]);
        off += lens[i];
    }
    blk[total] = 0;
    *out = blk;
    *cnt = n;
    return 0;
}

// unified read/write across console / pipe / file; bytes moved or -1
static long file_write(struct file *f, const void *buf, uint64_t len) {
    if (f->is_console) {
        // one write() = one uninterleaved chunk on the console
        const char *b = buf;
        console_lock();
        for (uint64_t i = 0; i < len; i++)
            console_putc(b[i]);
        console_unlock();
        return (long)len;
    }
    if (f->pipe)
        return (long)pipe_write_nb(f->pipe, buf, len);
    if (f->is_socket)
        return net_sendto(f->sock, buf,
                          len > 0xffff ? 0xffff : (uint16_t)len, 0, 0);
    return vfs_write(f, buf, len);
}

static long file_read(struct file *f, void *buf, uint64_t len);
__attribute__((unused)) static long file_read(struct file *f, void *buf, uint64_t len) {
    if (f->is_console)
        return tty_read(buf, len ? (int)len : 1);
    if (f->pipe)
        return (long)pipe_read_nb(f->pipe, buf, len);
    if (f->is_socket)
        return net_recvfrom(f->sock, buf,
                            len > 0xffff ? 0xffff : (uint16_t)len, 0, 0);
    return vfs_read(f, buf, len);
}

// errno values the kernel returns as negative results (linux abi)
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EAFNOSUPPORT 97
#define ESOCKTNOSUPPORT 94
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
#define EPIPE   32
#define ECHILD  10
#define ERANGE  34
#define ENOTEMPTY 39
#define EMSGSIZE 90
#define EPROTONOSUPPORT 93
#define EDESTADDRREQ 89
#define ENOTSOCK 88
#define EADDRINUSE 98
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111
#define EALREADY 114
#define EINPROGRESS 115

#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_CLOEXEC 0x80000
#define E2BIG    7

// cap for one read/write: user_range_ok walks every 4k page, an
// unchecked SIZE_MAX count would spin the walker forever
#define MAX_RW (16 << 20)

#define SIGPIPE 13

#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD 0
#define F_DUPFD_CLOEXEC 1030

#define DT_REG  8
#define DT_DIR  4
#define DT_LNK  10

#define MREMAP_MAYMOVE 1

#define AT_FDCWD (-100)

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
    if (len > MAX_RW)
        len = MAX_RW;
    // the buffer is copied from inside the kernel: a bad user pointer
    // would page-fault in kernel mode and kill the task with SIGSEGV
    if (len && !task_user_range_ok(current, (uint64_t)buf, len, 0)) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    // fault lazy pages in up front: a fill mid-file_write would nest an
    // ext2 read inside a disk op
    if (len)
        task_prefault_range(current, (uint64_t)buf, len);
    if (f->pipe && !f->pipe->readers) {
        signal_send_task(current, SIGPIPE);   // default kills
        r->rax = -EPIPE;
        return (uint64_t)r;
    }
    long n = file_write(f, buf, len);
    if (n >= 0 && f->pipe)
        task_wake_pipe(f->pipe);
    if (f->pipe && n == 0 && len) {
        // full ring: re-try under cli, then park until a reader drains.
        // returning 0 would read as EOF and truncate pipelines; parking
        // without the re-check could sleep through a drain that already
        // happened between the write attempt and the block
        cli();
        n = file_write(f, buf, len);
        if (n != 0) {
            sti();
            if (n > 0)
                task_wake_pipe(f->pipe);
            r->rax = (uint64_t)n;
            return (uint64_t)r;
        }
        if (!f->pipe->readers) {
            sti();
            signal_send_task(current, SIGPIPE);
            r->rax = -EPIPE;
            return (uint64_t)r;
        }
        replay_fixup(r);
        current->wait_reason = WAIT_PIPE;
        current->wait_pipe = f->pipe;
        current->state = T_BLOCKED;
        return task_schedule((uint64_t)r);
    }
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
    // every path below copies into the user buffer in kernel mode
    uint64_t rlen = r->rdx ? r->rdx : 1;
    if (rlen > MAX_RW)
        rlen = MAX_RW;
    if (!task_user_range_ok(current, (uint64_t)buf, rlen, 1)) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    // same as sys_write: prefault before the kernel copies through here
    task_prefault_range(current, (uint64_t)buf, rlen);
    if (f->pipe) {
        // pipe: return what fits; block only when empty and writers live.
        // the examine+block runs inside one cli window: a writer that
        // fills, drains or closes between the read attempt and the park
        // would otherwise lose its wakeup (or the EOF) forever
        cli();
        uint64_t n = pipe_read_nb(f->pipe, (uint8_t *)buf, rlen);
        if (n) {
            sti();
            task_wake_pipe(f->pipe);   // freed space may wake a writer
            r->rax = n;
            return (uint64_t)r;
        }
        if (!f->pipe->writers) {
            sti();
            r->rax = 0;   // EOF
            return (uint64_t)r;
        }
        replay_fixup(r);
        current->wait_reason = WAIT_PIPE;
        current->wait_pipe = f->pipe;
        current->state = T_BLOCKED;
        return task_schedule((uint64_t)r);
    }
    if (f->is_console) { // console: line discipline read
        uint64_t rl = r->rdx ? r->rdx : 1;
        if (rl > 0x7fffffff)
            rl = 0x7fffffff;   // tty_read takes an int: keep it positive
        long n = tty_read(buf, (int)rl);
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
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
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
    // absolute path for /proc/<pid>/fd readlink targets
    strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = 0;
    int fd = task_fd_alloc(f);
    if (fd < 0) {
        vfs_close(f);
        r->rax = -EMFILE;
        return (uint64_t)r;
    }
    if (flags & O_CLOEXEC)
        current->fd_flags[fd] |= FD_CLOEXEC;
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
    vfs_close(f);        // socket stubs release their net slot here
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
    if (!f->is_console) {           // pipes and regular files: no tty ioctls
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
    case 0x540F: {                  // TIOCGPGRP: the foreground group
        int *p = arg;
        *p = current->pgid;
        r->rax = 0;
        return (uint64_t)r;
    }
    case 0x5410:                    // TIOCSPGRP: accepted, single fg group
        r->rax = 0;
        return (uint64_t)r;
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
    // MAP_FIXED must land inside the user half
    if ((flags & MAP_FIXED) &&
        (addr >= USER_LIMIT || len > USER_LIMIT - addr)) {
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
    // kernel half is off limits no matter how the spot was picked
    if (start >= USER_LIMIT || len > USER_LIMIT - start) {
        r->rax = -1ULL;
        return (uint64_t)r;
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

    // anonymous private: demand-paged — record only, zero frames appear
    // on #PF (task_mmap_fault). anonymous shared: eager, so forked
    // sharers see the same frames from the first touch. file-backed:
    // record only, pages appear on #PF
    if (!file && !(flags & MAP_SHARED)) {
        ;   // fully lazy: nothing to map or roll back here
    } else if (!file) {
        uint64_t vflags = VMM_PRESENT | VMM_USER;
        if (prot & PROT_WRITE)
            vflags |= VMM_WRITE;
        if (!(prot & PROT_EXEC))
            vflags |= VMM_NX;
        for (uint64_t va = start; va < start + len; va += PAGE_SIZE) {
            void *p = pmm_alloc_zeroed();
            if (!p) {
                // report ENOMEM, never panic on user-caused oom
                for (uint64_t u = start; u < va; u += PAGE_SIZE) {
                    uint64_t ph = vmm_get_phys(pml4, u);
                    vmm_unmap(pml4, u);
                    if (ph)
                        pmm_free((void *)ph);
                }
                r->rax = -ENOMEM;
                return (uint64_t)r;
            }
            vmm_map(pml4, va, (uint64_t)p, vflags);
        }
    }

    struct mmap_region *m = kmalloc(sizeof(*m));
    if (!m) {
        if (!file) {
            for (uint64_t va = start; va < start + len; va += PAGE_SIZE) {
                uint64_t ph = vmm_get_phys(pml4, va);
                vmm_unmap(pml4, va);
                if (ph)
                    pmm_free((void *)ph);
            }
        }
        r->rax = -ENOMEM;
        return (uint64_t)r;
    }
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

static uint64_t sys_mprotect(struct regs *r) {
    uint64_t addr = r->rdi, len = r->rsi, prot = r->rdx;
    if ((addr & 0xfff) || !len) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    // keep protection changes out of the kernel half
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    uint64_t pages = len / PAGE_SIZE;
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
    if (want > USER_MMAP_BASE) {
        r->rax = cur;   // keep the break inside the user half
        return (uint64_t)r;
    }
    uint64_t pml4 = current->pml4;
    if (want > cur) {
        // stop at the first failed alloc, report what stuck
        uint64_t top = cur;
        for (uint64_t va = (cur + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
             va < want; va += PAGE_SIZE) {
            void *p = pmm_alloc_zeroed();
            if (!p)
                break;
            vmm_map(pml4, va, (uint64_t)p,
                    VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
            top = va + PAGE_SIZE;
        }
        current->brk_cur = top < want ? top : want;
        r->rax = current->brk_cur;
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
    // kernel-side copy of the program name; user pages are readable while
    // the caller's cr3 is still active (same for argv/envp below)
    char raw[128];
    int i = 0;
    while (i < (int)sizeof(raw) - 1) {
        char c = uname[i];
        raw[i++] = c;
        if (!c)
            break;
    }
    raw[sizeof(raw) - 1] = 0;

    int has_slash = 0;
    for (const char *p = raw; *p; p++)
        if (*p == '/') { has_slash = 1; break; }

    char path[256];
    struct vnode *vn;
    if (has_slash) {
        if (user_path(r->rdi, path, sizeof(path)) < 0) {
            r->rax = -EFAULT;
            return (uint64_t)r;
        }
        vn = vfs_resolve(path);
    } else {
        // bare name: PATH search (kernel policy: /bin/<name>)
        vn = vfs_resolve_prog(raw);
        strcpy(path, raw);
    }
    if (!vn || vn->type != VNODE_FILE) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }

    // capture argv/envp before the image swap; blocks freed below
    exec_args_t ea = { 0, 0, 0, 0 };
    int rc = capture_strv(r->rsi, &ea.argv, &ea.argc);
    if (!rc)
        rc = capture_strv(r->rdx, &ea.envp, &ea.envc);
    if (rc) {
        if (ea.argv)
            kfree(ea.argv);
        r->rax = rc == -2 ? (uint64_t)-EFAULT
               : rc == -1 ? (uint64_t)-E2BIG
                          : (uint64_t)-ENOMEM;
        return (uint64_t)r;
    }

    uint64_t fr = task_execve(vn, path, &ea);
    if (!fr) {
        // exec failed: the captures are still ours to free. on success the
        // task adopts argv for /proc/<pid>/cmdline
        if (ea.argv)
            kfree(ea.argv);
    }
    if (ea.envp)
        kfree(ea.envp);
    if (!fr) {
        r->rax = -ENOENT;
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
    if (cnt > 1024)
        cnt = 1024;
    // the vector array itself lives in user memory
    if (cnt && !task_user_range_ok(current, (uint64_t)iv,
                                  cnt * sizeof(struct iovec), 0)) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        // reuse the write path by hand: fd may be the console or a pipe
        struct file *f = fd_get(fd);
        if (!f) {
            r->rax = total ? total : (uint64_t)-EBADF;
            return (uint64_t)r;
        }
        uint64_t len = iv[i].len;
        if (len > MAX_RW)
            len = MAX_RW;
        if (len && !task_user_range_ok(current,
                                      (uint64_t)iv[i].base, len, 0)) {
            r->rax = -EFAULT;
            return (uint64_t)r;
        }
        if (len)
            task_prefault_range(current, (uint64_t)iv[i].base, len);
        if (f->pipe && !f->pipe->readers) {
            signal_send_task(current, SIGPIPE);
            r->rax = total ? total : (uint64_t)-EPIPE;
            return (uint64_t)r;
        }
        long n = file_write(f, iv[i].base, len);
        if (n < 0) {
            r->rax = -EIO;
            return (uint64_t)r;
        }
        total += (uint64_t)n;
        if (f->pipe) {
            task_wake_pipe(f->pipe);
            if (n == 0 && len) {
                // full ring: park and replay the whole writev, like
                // sys_write. returning 0 would read as EOF and truncate
                // pipelines (glibc stdio writes through writev)
                cli();
                n = file_write(f, iv[i].base, len);
                if (n != 0) {
                    sti();
                    if (n > 0) {
                        total += (uint64_t)n;
                        task_wake_pipe(f->pipe);
                    }
                    r->rax = total;
                    return (uint64_t)r;
                }
                if (!f->pipe->readers) {
                    sti();
                    signal_send_task(current, SIGPIPE);
                    r->rax = total ? total : (uint64_t)-EPIPE;
                    return (uint64_t)r;
                }
                replay_fixup(r);
                current->wait_reason = WAIT_PIPE;
                current->wait_pipe = f->pipe;
                current->state = T_BLOCKED;
                return task_schedule((uint64_t)r);
            }
        }
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

static void stat_fill(struct stat_k *st, struct vnode *vn) {
    memset(st, 0, sizeof(*st));
    if (vn->mode)
        st->mode = vn->mode;
    else if (vn->type == VNODE_DIR)
        st->mode = 0x4000 | 0755;      // S_IFDIR
    else
        st->mode = 0x8000 | 0644;      // S_IFREG
    st->size = vn->size;
    st->blksize = 1024;
    st->nlink = 1;
    st->dev = vn->dev;
    st->ino = vn->ino;
}

static uint64_t sys_fstat(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    struct stat_k *st = (struct stat_k *)r->rsi;
    if (!f || !st) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    if (f->is_console) {
        memset(st, 0, sizeof(*st));
        st->mode = 0x2000 | 0620;      // S_IFCHR | tty-ish perms
        st->blksize = 1024;
        r->rax = 0;
        return (uint64_t)r;
    }
    if (f->pipe) {
        memset(st, 0, sizeof(*st));
        st->mode = 0x1000 | 0600;      // S_IFIFO
        st->blksize = 4096;
        r->rax = 0;
        return (uint64_t)r;
    }
    stat_fill(st, f->vn);
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
        if (!task_user_range_ok(current, arg, 8, 1)) {
            r->rax = -EFAULT;
            break;
        }
        *(uint64_t *)arg = current->fs_base;
        r->rax = 0;
        break;
    case 0x1004:
        if (!task_user_range_ok(current, arg, 8, 1)) {
            r->rax = -EFAULT;
            break;
        }
        *(uint64_t *)arg = current->gs_base;
        r->rax = 0;
        break;
    default:
        r->rax = -1ULL;
    }
    return (uint64_t)r;
}

static uint64_t sys_set_tid_address(struct regs *r) {
    // the kernel writes *clear_tid = 0 on exit: only accept a mapped
    // user-writable word, anything else would be a kernel write
    if (!task_user_range_ok(current, r->rdi, 4, 1)) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    current->clear_tid = r->rdi;
    r->rax = (uint64_t)(long)current->pid;
    return (uint64_t)r;
}

// SYS_newfstatat: dirfd + path ("" or AT_EMPTY_PATH => stat the fd itself)
static uint64_t sys_newfstatat(struct regs *r) {
    struct stat_k *st = (struct stat_k *)r->rdx;
    const char *path = (const char *)r->rsi;
    int flags = (int)r->r10;
    int empty = !path || !path[0] || (flags & 0x1000);   // AT_EMPTY_PATH
    int nofollow = flags & 0x100;                        // AT_SYMLINK_NOFOLLOW
    if (!st || (!empty && !path)) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct vnode *vn = 0;
    if (empty) {
        struct file *f = fd_get((int)r->rdi);
        if (f && f->vn)
            vn = f->vn;
    } else {
        char p[256];
        if (user_path(r->rsi, p, sizeof(p)) < 0) {
            r->rax = -EFAULT;
            return (uint64_t)r;
        }
        // lstat(): report the link itself, not its target (busybox ls -l
        // on /proc/<pid>/fd otherwise chases the target and gets ENOENT
        // for /dev/console which has no vnode)
        vn = nofollow ? vfs_resolve_nofollow(p) : vfs_resolve(p);
    }
    if (!vn) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    stat_fill(st, vn);
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
    // cli across the whole scan+block: the child's exit_common also runs
    // with IF=0, so (single cpu) a child cannot exit between our "no zombie
    // found" scan and the blocked-state store — that interleave used to
    // park the parent in WAIT_CHILD forever (busybox children exit fast)
    int *status = (int *)r->rsi;
    long want = (long)r->rdi;          // pid filter: -1 = any child, 0 =
                                       // any child in my group (same here)
    cli();
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_ZOMBIE && t->parent == current &&
            (want <= 0 || t->pid == want)) {
            if (status) {
                *status = t->sig_death ? t->exit_code
                                       : (t->exit_code & 0xff) << 8;
            }
            r->rax = t->pid;
            kfree(t->cmdline);   // zombie held it for /proc; gone now
            t->cmdline = 0;
            t->cmdline_len = 0;
            t->parent = 0;
            t->state = T_FREE;
            return (uint64_t)r;        // iretq restores IF
        }
    }
    // posix: no (live or zombie) children at all -> ECHILD, not a block
    // forever. busybox sh counts on this after reaping a pipeline
    int have_child = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state != T_FREE &&
            task_table[i].parent == current) {
            have_child = 1;
            break;
        }
    if (!have_child) {
        r->rax = (uint64_t)-ECHILD;
        return (uint64_t)r;
    }
    // no zombie child yet: sleep until one exits (replay re-runs wait4)
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
        if (timeout &&
            !task_user_range_ok(current, (uint64_t)timeout, 16, 0)) {
            r->rax = -EFAULT;
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
        // lost wakeup guard: re-read the word now that we sit inside the
        // cli window — a waker that changed it after the preemptible
        // check above must not strand us on the queue forever
        if (*uaddr != val) {
            sti();
            task_frame_syscall_result(current, -EAGAIN);
            return (uint64_t)r;
        }
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
    if (!ts || !task_user_range_ok(current, (uint64_t)ts, 16, 0)) {
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

// clock_nanosleep(230): what modern glibc actually calls for sleep().
// TIMER_ABSTIME uses our tick-based clock directly (realtime and
// monotonic share the tick epoch here); relative waits block like
// nanosleep. the rem pointer is a nanosleep-only concept — ignored
#define TIMER_ABSTIME 1
static uint64_t sys_clock_nanosleep(struct regs *r) {
    int clk = (int)r->rdi;
    int flags = (int)r->rsi;
    const uint64_t *req = (const uint64_t *)r->rdx;
    if (!req || (flags & ~TIMER_ABSTIME) ||
        (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    if (!task_user_range_ok(current, (uint64_t)req, 16, 0)) {
        r->rax = (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    uint64_t target = req[0] * 100 + req[1] / 10000000;
    uint64_t t;
    if (flags & TIMER_ABSTIME) {
        t = target > pit_ticks() ? target - pit_ticks() : 0;
    } else {
        t = target;
        if (!t)
            t = 1;
    }
    if (!t) {
        r->rax = 0;   // deadline already passed
        return (uint64_t)r;
    }
    cli();               // blocking window must be atomic (see sys_read)
    replay_fixup(r);
    current->wake_tick = pit_ticks() + t;
    current->wait_reason = WAIT_SLEEP;
    current->state = T_BLOCKED;
    return task_schedule((uint64_t)r);
}

// --- signal waits: pause / rt_sigsuspend / rt_sigtimedwait ----------------

// bits whose handler would do nothing on delivery (SIG_IGN or a default
// action of ignore): they must not wake a signal waiter
static uint64_t sigwait_catchable_mask(void) {
    uint64_t m = 0;
    for (int s = 1; s <= SIG_MAX; s++) {
        uint64_t h = current->sigact[s].handler;
        if (h != 1 && !(h == 0 && signal_default_ignores(s)))
            m |= 1ULL << (s - 1);
    }
    return m;
}

// pause(34): sleeps until a catchable signal is delivered (handler runs,
// then -EINTR) or kills the task. never restarts (posix)
static uint64_t sys_pause(struct regs *r) {
    cli();               // check/block window must be atomic vs senders
    replay_fixup(r);     // re-enter for the delivery, then return EINTR
    current->wait_reason = WAIT_SIGNAL;
    current->sig_wait_kind = SW_PAUSE;
    current->wake_tick = 0;
    // a catchable signal already pending: take the delivery right away
    // (posix: the handler runs before pause returns). nothing would wake
    // us later for it — wake-ups fire on sends only
    if (current->sig_pending & ~current->sig_mask & sigwait_catchable_mask())
        current->state = T_READY;
    else
        current->state = T_BLOCKED;
    return task_schedule((uint64_t)r);
}

// rt_sigsuspend(130): mask = arg until a non-arg signal is delivered; the
// handler runs, the ORIGINAL mask is restored by rt_sigreturn and the
// syscall reports -EINTR (never restarts)
static uint64_t sys_sigsuspend(struct regs *r) {
    const uint64_t *uset = (const uint64_t *)r->rdi;
    if (!uset || r->rsi != 8 ||
        !task_user_range_ok(current, (uint64_t)uset, 8, 0)) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    uint64_t newmask = *uset & ~((1ULL << (SIGKILL - 1)) |
                                 (1ULL << (SIGSTOP - 1)));
    cli();
    uint64_t saved = current->sig_mask;
    current->sig_mask = newmask;
    // a catchable signal already deliverable under the temp mask: take
    // the delivery now (posix: the handler runs before sigsuspend
    // returns) — rewind + ready re-enters the dispatcher for it
    if (current->sig_pending & ~newmask & sigwait_catchable_mask())
        current->state = T_READY;
    else
        current->state = T_BLOCKED;
    replay_fixup(r);
    current->wait_reason = WAIT_SIGNAL;
    current->sig_wait_kind = SW_SUSPEND;
    current->sig_wait_saved_mask = saved;
    current->wake_tick = 0;
    return task_schedule((uint64_t)r);
}

// rt_sigtimedwait(128): like sigwaitinfo — signals in set are CONSUMED
// (no handler runs) and returned as the syscall result; optional timeout
// reports -EAGAIN. siginfo goes to the user when asked. layout must stay
// 128 bytes: that is what glibc's siginfo_t allocates on the stack
struct siginfo_k { int signo, errno_, code; uint64_t pad[14]; };

static uint64_t sys_sigtimedwait(struct regs *r) {
    const uint64_t *uset = (const uint64_t *)r->rdi;
    struct siginfo_k *uinfo = (struct siginfo_k *)r->rsi;
    const uint64_t *ts = (const uint64_t *)r->rdx;
    if (!uset || r->r10 != 8 ||   // arg4 (sigsetsize) rides r10
        !task_user_range_ok(current, (uint64_t)uset, 8, 0) ||
        (uinfo && !task_user_range_ok(current, (uint64_t)uinfo,
                                     sizeof(*uinfo), 1)) ||
        (ts && !task_user_range_ok(current, (uint64_t)ts, 16, 0))) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    uint64_t set = *uset & ~((1ULL << (SIGKILL - 1)) |
                             (1ULL << (SIGSTOP - 1)));
    cli();
    // a signal consumed while queued (sender parked sig_wait_result and
    // rewound us back here): report it
    if (current->sig_wait_result) {
        int sig = current->sig_wait_result;
        current->sig_wait_result = 0;
        current->sig_woke_rewind = 0;
        sti();
        if (uinfo) {
            memset(uinfo, 0, sizeof(*uinfo));
            uinfo->signo = sig;
            uinfo->code = -6;   // SI_TKILL
        }
        r->rax = (uint64_t)sig;
        return (uint64_t)r;
    }
    // pending in set right now: consume directly (ignored signals are
    // accepted too — posix sigwait semantics)
    uint64_t hit = current->sig_pending & set;
    if (hit) {
        int sig = 1;
        while (!(hit & (1ULL << (sig - 1))))
            sig++;
        current->sig_pending &= ~(1ULL << (sig - 1));
        sti();
        if (uinfo) {
            memset(uinfo, 0, sizeof(*uinfo));
            uinfo->signo = sig;
            uinfo->code = -6;
        }
        r->rax = (uint64_t)sig;
        return (uint64_t)r;
    }
    if (ts && !ts[0] && !ts[1]) {   // zero timeout: poll
        sti();
        r->rax = (uint64_t)-EAGAIN;
        return (uint64_t)r;
    }
    // block. the frame is NOT rewound: wakes stuff the result (timeout,
    // consume-at-wake rewinds, see signal_send_task) instead of re-entry
    current->wait_reason = WAIT_SIGNAL;
    current->sig_wait_kind = SW_SIGWAIT;
    current->sig_wait_set = set;
    current->sig_wait_result = 0;
    current->wake_tick = ts ? futex_deadline_from_timespec(ts, 0) : 0;
    current->state = T_BLOCKED;
    return task_schedule((uint64_t)r);
}

// setsid(106): new session = own process group (no session tracking yet;
// busybox init asks for exactly this). EPERM when a group leader exists
// with our pid
static uint64_t sys_setsid(struct regs *r) {
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state != T_FREE &&
            task_table[i].pgid == current->pid) {
            r->rax = (uint64_t)-EPERM;
            return (uint64_t)r;
        }
    current->pgid = current->tgid;
    r->rax = (uint64_t)(long)current->tgid;
    return (uint64_t)r;
}

static uint64_t sys_clock_gettime(struct regs *r) {
    uint64_t *tp = (uint64_t *)r->rsi;
    int clk = (int)r->rdi;
    if (!tp || (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    if (!task_user_range_ok(current, (uint64_t)tp, 16, 1)) {
        r->rax = (uint64_t)-EFAULT;
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
    if (tp && !task_user_range_ok(current, (uint64_t)tp, 8, 1)) {
        r->rax = (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
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

// --- userspace porting set: cwd, getdents, pipes, fds, namei --------------

// getdents64: f->off doubles as the readdir cursor; linux_dirent64 layout
struct linux_dirent64_k {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    // char d_name[]; NUL-terminated, padded to 8
};

static uint64_t sys_getdents64(struct regs *r) {
    int fd = (int)r->rdi;
    uint8_t *ubuf = (uint8_t *)r->rsi;
    uint64_t count = r->rdx;
    struct file *f = fd_get(fd);
    if (!f || !f->vn || f->vn->type != VNODE_DIR) {
        r->rax = f && f->vn ? (uint64_t)-ENOTDIR : (uint64_t)-EBADF;
        return (uint64_t)r;
    }
    uint64_t written = 0;
    char name[256];
    for (;;) {
        uint64_t saved = f->off;
        uint64_t ctx = saved;
        uint64_t ino;
        int type;
        if (!vfs_readdir(f->vn, &ctx, &ino, &type, name, (int)sizeof(name)))
            break;
        uint64_t nl = strlen(name) + 1;
        uint64_t reclen = (19 + nl + 7) & ~(uint64_t)7;
        if (written + reclen > count) {
            f->off = saved;   // entry stays pending for the next call
            break;
        }
        uint8_t *e = ubuf + written;
        *(uint64_t *)(e + 0) = ino;
        *(int64_t *)(e + 8) = (int64_t)ctx;
        *(uint16_t *)(e + 16) = (uint16_t)reclen;
        e[18] = type == VNODE_DIR ? DT_DIR
              : type == VNODE_LNK ? DT_LNK : DT_REG;
        memcpy(e + 19, name, nl);
        memset(e + 19 + nl, 0, reclen - 19 - nl);
        written += reclen;
        f->off = ctx;
    }
    r->rax = written;
    return (uint64_t)r;
}

static uint64_t sys_getcwd(struct regs *r) {
    char *buf = (char *)r->rdi;
    uint64_t size = r->rsi;
    uint64_t len = strlen(current->cwd) + 1;
    if (!buf || size < len) {
        r->rax = size && buf ? (uint64_t)-ERANGE : (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    memcpy(buf, current->cwd, len);
    r->rax = len;
    return (uint64_t)r;
}

static uint64_t sys_chdir(struct regs *r) {
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct vnode *vn = vfs_resolve(path);
    if (!vn || vn->type != VNODE_DIR) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    path_canon(path);
    strcpy(current->cwd, path);
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_pipe2(struct regs *r) {
    int *ufds = (int *)r->rdi;
    uint64_t flags = r->rsi;
    if (!ufds) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct pipe *p = pipe_create();
    if (!p) {
        r->rax = -ENOMEM;
        return (uint64_t)r;
    }
    struct file *fr = kmalloc(sizeof(*fr));
    struct file *fw = kmalloc(sizeof(*fw));
    if (!fr || !fw) {
        if (fr) kfree(fr);
        if (fw) kfree(fw);
        kfree(p);
        r->rax = -ENOMEM;
        return (uint64_t)r;
    }
    fr->vn = 0; fr->pipe = p; fr->pipe_writer = 0; fr->off = 0;
    fr->refs = 1; fr->is_console = 0;
    fw->vn = 0; fw->pipe = p; fw->pipe_writer = 1; fw->off = 0;
    fw->refs = 1; fw->is_console = 0;
    int rfd = task_fd_alloc(fr);
    if (rfd < 0) {
        kfree(fr);
        kfree(fw);
        kfree(p);
        r->rax = -EMFILE;
        return (uint64_t)r;
    }
    int wfd = task_fd_alloc(fw);
    if (wfd < 0) {
        current->fds[rfd] = 0;
        vfs_close(fr);   // readers->0; the pipe dies with the write end
        kfree(fw);
        kfree(p);
        r->rax = -EMFILE;
        return (uint64_t)r;
    }
    if (flags & O_CLOEXEC) {
        current->fd_flags[rfd] |= FD_CLOEXEC;
        current->fd_flags[wfd] |= FD_CLOEXEC;
    }
    ufds[0] = rfd;
    ufds[1] = wfd;
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_pipe(struct regs *r) {
    struct regs tmp = *r;
    tmp.rsi = 0;   // no flags
    sys_pipe2(&tmp);
    r->rax = tmp.rax;   // shim: return OUR frame, only the result moves
    return (uint64_t)r;
}

static uint64_t sys_dup(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    if (!f) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    f->refs++;
    int fd = task_fd_alloc(f);
    if (fd < 0) {
        f->refs--;
        r->rax = -EMFILE;
        return (uint64_t)r;
    }
    current->fd_flags[fd] = 0;
    r->rax = (uint64_t)fd;
    return (uint64_t)r;
}

static uint64_t sys_dup2(struct regs *r) {
    int oldfd = (int)r->rdi;
    int newfd = (int)r->rsi;
    struct file *f = fd_get(oldfd);
    if (!f || newfd < 0 || newfd >= FILE_MAX) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (oldfd == newfd) {
        r->rax = (uint64_t)newfd;
        return (uint64_t)r;
    }
    struct file *prev = current->fds[newfd];
    current->fds[newfd] = f;
    current->fd_flags[newfd] = 0;
    f->refs++;
    if (prev)
        vfs_close(prev);
    r->rax = (uint64_t)newfd;
    return (uint64_t)r;
}

static uint64_t sys_dup3(struct regs *r) {
    uint64_t fr = sys_dup2(r);
    if ((int)r->rsi != (int)r->rdi && fr == (uint64_t)r &&
        r->rax == (uint64_t)(int)r->rsi && (r->rdx & O_CLOEXEC))
        current->fd_flags[(int)r->rsi] |= FD_CLOEXEC;
    return fr;
}

static uint64_t sys_fcntl(struct regs *r) {
    int fd = (int)r->rdi;
    int cmd = (int)r->rsi;
    uint64_t arg = r->rdx;
    struct file *f = fd_get(fd);
    if (!f) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    switch (cmd) {
    case F_GETFD:
        r->rax = current->fd_flags[fd];
        return (uint64_t)r;
    case F_SETFD:
        current->fd_flags[fd] = (arg & FD_CLOEXEC) ? FD_CLOEXEC : 0;
        r->rax = 0;
        return (uint64_t)r;
    case F_GETFL:
        r->rax = 2;   // O_RDWR: we do not track open modes
        return (uint64_t)r;
    case F_SETFL:
        r->rax = 0;   // O_NONBLOCK et al: accepted, not implemented
        return (uint64_t)r;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if ((int)arg < 0 || (int)arg >= FILE_MAX) {
            r->rax = -EINVAL;
            return (uint64_t)r;
        }
        for (int i = (int)arg; i < FILE_MAX; i++) {
            if (current->fds[i])
                continue;
            f->refs++;
            current->fds[i] = f;
            current->fd_flags[i] =
                cmd == F_DUPFD_CLOEXEC ? FD_CLOEXEC : 0;
            r->rax = (uint64_t)i;
            return (uint64_t)r;
        }
        r->rax = -EMFILE;
        return (uint64_t)r;
    }
    default:
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
}

static uint64_t sys_mkdir(struct regs *r) {
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    if (!vfs_mkdir(path)) {
        r->rax = -EEXIST;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_rmdir(struct regs *r) {
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct vnode *vn = vfs_resolve(path);
    if (!vn || vn->type != VNODE_DIR) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    if (!vfs_rmdir(path)) {
        r->rax = -ENOTEMPTY;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_unlink(struct regs *r) {
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    if (!vfs_unlink(path)) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_rename(struct regs *r) {
    char oldp[256], newp[256];
    if (user_path(r->rdi, oldp, sizeof(oldp)) < 0 ||
        user_path(r->rsi, newp, sizeof(newp)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    if (vfs_rename(oldp, newp) < 0) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_symlink(struct regs *r) {
    char path[256];
    const char *ut = (const char *)r->rsi;
    char target[128];
    int i = 0;
    if (ut)
        while (i < (int)sizeof(target) - 1) {
            char c = ut[i];
            target[i++] = c;
            if (!c)
                break;
        }
    target[sizeof(target) - 1] = 0;
    if (!target[0] || user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    if (!vfs_symlink(path, target)) {
        r->rax = -EEXIST;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_readlink(struct regs *r) {
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct vnode *vn = vfs_resolve_nofollow(path);
    if (!vn || vn->type != VNODE_LNK) {
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    char *buf = (char *)r->rsi;
    uint64_t size = r->rdx;
    char target[128];
    long n = vfs_readlink_vn(vn, target, sizeof(target));
    if (n < 0) {
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    if ((uint64_t)n > size)
        n = (long)size;
    memcpy(buf, target, (uint64_t)n);
    r->rax = (uint64_t)n;
    return (uint64_t)r;
}

static uint64_t sys_ftruncate(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    uint64_t len = r->rsi;
    if (!f || !f->vn) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (vfs_truncate_to(f->vn, len) < 0) {
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_fchmod(struct regs *r) {
    struct file *f = fd_get((int)r->rdi);
    if (!f || !f->vn) {
        r->rax = -EBADF;
        return (uint64_t)r;
    }
    if (vfs_chmod(f->vn, (uint32_t)r->rsi) < 0) {
        r->rax = -EPERM;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_fchmodat(struct regs *r) {
    char path[256];
    if (user_path(r->rsi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct vnode *vn = vfs_resolve(path);
    if (!vn) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    if (vfs_chmod(vn, (uint32_t)r->rdx) < 0) {
        r->rax = -EPERM;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_link(struct regs *r) {
    char oldp[256], newp[256];
    if (user_path(r->rdi, oldp, sizeof(oldp)) < 0 ||
        user_path(r->rsi, newp, sizeof(newp)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    if (vfs_link(oldp, newp) < 0) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_chmod(struct regs *r) {
    char path[256];
    if (user_path(r->rdi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    struct vnode *vn = vfs_resolve(path);
    if (!vn) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    if (vfs_chmod(vn, (uint32_t)r->rsi) < 0) {
        r->rax = -EPERM;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

// glibc struct sysinfo (x86_64: 112 bytes, tail-padded)
struct sysinfo_k {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram;
    uint64_t totalswap, freeswap;
    uint16_t procs, pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
};

_Static_assert(sizeof(struct sysinfo_k) == 112, "sysinfo layout");

// --- scheduler priorities: nice(154), getpriority(140), setpriority(141)

static int nice_clamp(int v) {
    if (v < -20)
        v = -20;
    if (v > 19)
        v = 19;
    return v;
}

static uint64_t sys_nice(struct regs *r) {
    long inc = (long)r->rdi;
    current->nice = nice_clamp(current->nice + (int)inc);
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_getpriority(struct regs *r) {
    int which = (int)r->rdi;
    long who = (long)r->rsi;
    if (which != 0) {   // PRIO_PROCESS only
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    if (!who)
        who = current->tgid;   // posix: who=0 means the caller
    int best = 100;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_FREE)
            continue;
        if (t->tgid != who && t->pid != who)
            continue;
        if (t->nice < best)
            best = t->nice;
    }
    if (best == 100) {
        r->rax = -ESRCH;
        return (uint64_t)r;
    }
    // linux convention: 20 - nice, so higher is more favored
    r->rax = (uint64_t)(long)(20 - best);
    return (uint64_t)r;
}

static uint64_t sys_setpriority(struct regs *r) {
    int which = (int)r->rdi;
    long who = (long)r->rsi;
    int prio = (int)r->rdx;
    if (which != 0) {
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    int hits = 0;
    int nice = nice_clamp(prio);
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_FREE)
            continue;
        if (who && t->tgid != who && t->pid != who)
            continue;
        t->nice = nice;
        hits++;
    }
    r->rax = hits ? 0 : (uint64_t)-ESRCH;
    return (uint64_t)r;
}

static uint64_t sys_sysinfo(struct regs *r) {
    struct sysinfo_k *si = (struct sysinfo_k *)r->rdi;
    if (!si) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    memset(si, 0, sizeof(*si));
    si->uptime = (int64_t)(pit_ticks() / 100);
    si->totalram = pmm_total_mem();
    si->freeram = pmm_free_mem();
    si->mem_unit = 1;
    int procs = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state != T_FREE && task_table[i].pid)
            procs++;
    si->procs = (uint16_t)procs;
    r->rax = 0;
    return (uint64_t)r;
}

// --- cpu accounting: times(43) and getrusage(98) -------------------------

// glibc struct tms (x86_64: 4 clock_t = 32 bytes)
struct tms_k {
    int64_t utime, stime, cutime, cstime;
};

static uint64_t sys_times(struct regs *r) {
    struct tms_k *tm = (struct tms_k *)r->rdi;
    if (!tm) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    uint64_t u, s;
    task_cpu_group(current->tgid, &u, &s);
    tm->utime = (int64_t)u;
    tm->stime = (int64_t)s;
    tm->cutime = (int64_t)current->cutime;
    tm->cstime = (int64_t)current->cstime;
    r->rax = pit_ticks();          // uptime in clock ticks (CLK_TCK = 100)
    return (uint64_t)r;
}

// glibc struct rusage (x86_64: 144 bytes)
struct rusage_k {
    int64_t utime_sec, utime_usec;   // ru_utime
    int64_t stime_sec, stime_usec;   // ru_stime
    int64_t rest[14];                // maxrss .. ru_nivcsw, all zeroed
};

_Static_assert(sizeof(struct rusage_k) == 144, "rusage layout");

static void ticks_to_timeval(uint64_t ticks, int64_t *sec, int64_t *usec) {
    *sec = (int64_t)(ticks / 100);
    *usec = (int64_t)((ticks % 100) * 10000);   // 100 Hz -> 10 ms per tick
}

static uint64_t sys_getrusage(struct regs *r) {
    int who = (int)r->rdi;
    struct rusage_k *ru = (struct rusage_k *)r->rsi;
    if (!ru) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    uint64_t u, s;
    if (who == 1) {                // RUSAGE_THREAD: this thread only
        u = current->utime;
        s = current->stime;
    } else if (who == (int)-1) {   // RUSAGE_CHILDREN: reaped children
        u = current->cutime;
        s = current->cstime;
    } else {                       // RUSAGE_SELF: whole thread group
        task_cpu_group(current->tgid, &u, &s);
    }
    memset(ru, 0, sizeof(*ru));
    ticks_to_timeval(u, &ru->utime_sec, &ru->utime_usec);
    ticks_to_timeval(s, &ru->stime_sec, &ru->stime_usec);
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_faccessat(struct regs *r) {
    char path[256];
    if (user_path(r->rsi, path, sizeof(path)) < 0) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    // mode: F_OK existence check; R/W/X all granted (we run as root)
    struct vnode *vn = vfs_resolve(path);
    if (!vn) {
        r->rax = -ENOENT;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_access(struct regs *r) {
    struct regs tmp = *r;
    tmp.rsi = r->rdi;   // path
    tmp.rdx = r->rsi;   // mode
    sys_faccessat(&tmp);
    r->rax = tmp.rax;   // shim: return OUR frame, only the result moves
    return (uint64_t)r;
}

static uint64_t sys_faccessat(struct regs *r);
static uint64_t sys_access(struct regs *r);
static int munmap_range(uint64_t addr, uint64_t len);

static uint64_t sys_mremap(struct regs *r) {
    uint64_t addr = r->rdi;
    uint64_t old_len = (r->rsi + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_len = (r->rdx + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if ((addr & 0xfff) || !old_len || !new_len) {
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    // exact anonymous region only (what musl's realloc mremaps)
    struct mmap_region *m = 0;
    for (struct mmap_region *x = current->mmaps; x; x = x->next)
        if (x->start == addr && x->end == addr + old_len && !x->file) {
            m = x;
            break;
        }
    if (!m) {
        r->rax = -EINVAL;
        return (uint64_t)r;
    }
    if (new_len <= old_len) {
        if (new_len < old_len)
            munmap_range(addr + new_len, old_len - new_len);
        r->rax = addr;
        return (uint64_t)r;
    }
    // grow: fresh gap, eager map, copy, drop the old range
    uint64_t hint = USER_MMAP_BASE;
    struct mmap_region *g = current->mmaps;
    for (; g; g = g->next) {
        if (g->start - hint >= new_len)
            break;
        hint = g->end;
    }
    uint64_t start = hint;
    for (struct mmap_region *x = current->mmaps; x; x = x->next)
        if (start < x->end && x->start < start + new_len) {
            r->rax = -ENOMEM;
            return (uint64_t)r;
        }
    uint64_t vflags = VMM_PRESENT | VMM_USER;
    if (m->prot & PROT_WRITE)
        vflags |= VMM_WRITE;
    if (!(m->prot & PROT_EXEC))
        vflags |= VMM_NX;
    for (uint64_t va = start; va < start + new_len; va += PAGE_SIZE) {
        void *pg = pmm_alloc_zeroed();
        if (!pg) {
            // unwind what this grow mapped so far, report ENOMEM
            for (uint64_t u = start; u < va; u += PAGE_SIZE) {
                uint64_t ph = vmm_get_phys(current->pml4, u);
                vmm_unmap(current->pml4, u);
                if (ph)
                    pmm_free((void *)ph);
            }
            r->rax = -ENOMEM;
            return (uint64_t)r;
        }
        vmm_map(current->pml4, va, (uint64_t)pg, vflags);
    }
    uint64_t copy = old_len < new_len ? old_len : new_len;
    // the old range may be demand-paged: fill its pages before the
    // kernel-side copy touches them
    task_prefault_range(current, addr, copy);
    memcpy((void *)start, (const void *)addr, copy);
    struct mmap_region *nm = kmalloc(sizeof(*nm));
    if (!nm) {
        for (uint64_t va = start; va < start + new_len; va += PAGE_SIZE) {
            uint64_t ph = vmm_get_phys(current->pml4, va);
            vmm_unmap(current->pml4, va);
            if (ph)
                pmm_free((void *)ph);
        }
        r->rax = -ENOMEM;
        return (uint64_t)r;
    }
    *nm = *m;
    nm->start = start;
    nm->end = start + new_len;
    nm->next = 0;
    struct mmap_region **pp = &current->mmaps;
    while (*pp && (*pp)->start < start)
        pp = &(*pp)->next;
    nm->next = *pp;
    *pp = nm;
    munmap_range(addr, old_len);
    r->rax = start;
    return (uint64_t)r;
}

// shared tail of munmap: carve bounds, drop every region inside
static int munmap_range(uint64_t addr, uint64_t len) {
    if (mmap_ensure_bounds(addr, addr + len) < 0)
        return -1;
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
    return 0;
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
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    if (munmap_range(addr, len) < 0) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_gettimeofday(struct regs *r) {
    uint64_t *tv = (uint64_t *)r->rdi;
    if (!tv) {
        r->rax = -EFAULT;
        return (uint64_t)r;
    }
    uint64_t t = pit_ticks();
    tv[0] = t / 100;                 // seconds
    tv[1] = (t % 100) * 10000;       // usec
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_setpgid(struct regs *r) {
    long pid = (long)r->rdi;
    long pgid = (long)r->rsi;
    struct task *t = current;
    if (pid != 0 && pid != current->pid) {
        t = 0;
        for (int i = 0; i < TASK_MAX; i++)
            if (task_table[i].state != T_FREE &&
                task_table[i].pid == pid &&
                task_table[i].parent == current) {
                t = &task_table[i];
                break;
            }
        if (!t) {
            r->rax = -ESRCH;
            return (uint64_t)r;
        }
    }
    if (pgid == 0)
        pgid = t->tgid;
    t->pgid = (int)pgid;
    r->rax = 0;
    return (uint64_t)r;
}

static uint64_t sys_getpgid(struct regs *r) {
    long pid = (long)r->rdi;
    if (pid == 0) {
        r->rax = (uint64_t)current->pgid;
        return (uint64_t)r;
    }
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state != T_FREE && task_table[i].pid == pid) {
            r->rax = (uint64_t)task_table[i].pgid;
            return (uint64_t)r;
        }
    r->rax = -ESRCH;
    return (uint64_t)r;
}

// sigaltstack(2): real alternate signal stack state lives in signal.c;
// user_rsp decides the SS_ONSTACK report (posix: cannot swap while on it)
static uint64_t sys_sigaltstack(struct regs *r) {
    long ret = signal_sys_sigaltstack((const uint64_t *)r->rdi,
                                      (uint64_t *)r->rsi, r->rsp);
    r->rax = (uint64_t)ret;
    return (uint64_t)r;
}

static uint64_t sys_utimensat(struct regs *r) {
    (void)r;
    r->rax = 0;
    return (uint64_t)r;
}

// --- minimal sockets: udp, icmp ping and tcp streams over the net stack ---
// sockets live in the process fd table as struct file stubs so their
// numbers never collide with open files and close() just works

struct sockaddr_in_k {
    uint16_t family;
    uint16_t port;      // big endian on the wire; kept as-is here
    uint32_t addr;      // big endian
    uint8_t zero[8];
};

static struct file *sock_file_alloc(int slot) {
    struct file *f = kmalloc(sizeof(*f));
    if (!f)
        return 0;
    memset(f, 0, sizeof(*f));
    f->refs = 1;
    f->is_socket = 1;
    f->sock = slot;
    return f;
}

// resolve an fd to a net stack socket slot
static int sock_slot(int fd, struct file **out) {
    struct file *f = fd_get(fd);
    if (!f || !f->is_socket)
        return -1;
    if (out)
        *out = f;
    return f->sock;
}

static uint64_t sys_socket(struct regs *r) {
    long domain = (long)r->rdi, type = (long)r->rsi, proto = (long)r->rdx;
    if (domain != 2) {          // AF_INET only
        r->rax = (uint64_t)-EAFNOSUPPORT;
        return (uint64_t)r;
    }
    int nproto;
    if (type == 2)              // SOCK_DGRAM: udp or raw icmp
        nproto = (int)proto;
    else if (type == 1)         // SOCK_STREAM: tcp
        nproto = (proto == 0 || proto == 6) ? NET_PROTO_TCP : -1;
    else {
        r->rax = (uint64_t)-ESOCKTNOSUPPORT;
        return (uint64_t)r;
    }
    if (nproto != NET_PROTO_UDP && nproto != NET_PROTO_ICMP &&
        nproto != NET_PROTO_TCP) {
        r->rax = (uint64_t)-EPROTONOSUPPORT;
        return (uint64_t)r;
    }
    long slot = net_socket(nproto);
    if (slot < 0) {
        r->rax = (uint64_t)-ENOMEM;
        return (uint64_t)r;
    }
    struct file *f = sock_file_alloc((int)slot);
    if (!f) {
        net_close((int)slot);
        r->rax = (uint64_t)-ENOMEM;
        return (uint64_t)r;
    }
    int fd = task_fd_alloc(f);
    if (fd < 0) {
        vfs_close(f);
        r->rax = (uint64_t)-EMFILE;
        return (uint64_t)r;
    }
    r->rax = (uint64_t)fd;
    return (uint64_t)r;
}

static int sock_args(struct regs *r, int *fd, const void **buf,
                     uint32_t *len, struct sockaddr_in_k **sa) {
    *fd = (int)r->rdi;
    *buf = (const void *)r->rsi;
    *len = (uint32_t)r->rdx;
    *sa = (struct sockaddr_in_k *)r->r10;
    if (!*buf || !*len)
        return -1;
    if (!task_user_range_ok(current, (uint64_t)*buf, *len, 0))
        return -1;
    if (*sa && !task_user_range_ok(current, (uint64_t)*sa,
                                   sizeof(struct sockaddr_in_k), 0))
        return -1;
    return 0;
}

static uint64_t sys_sendto(struct regs *r) {
    int fd;
    const void *buf;
    uint32_t len;
    struct sockaddr_in_k *to;
    if (sock_args(r, &fd, &buf, &len, &to) < 0) {
        r->rax = (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    int sfd = sock_slot(fd, 0);
    if (sfd < 0) {
        r->rax = (uint64_t)-ENOTSOCK;
        return (uint64_t)r;
    }
    if (to && to->family != 2) {
        r->rax = (uint64_t)-EAFNOSUPPORT;
        return (uint64_t)r;
    }
    uint32_t ip = to ? __builtin_bswap32(to->addr) : 0;
    uint16_t port = to ? __builtin_bswap16(to->port) : 0;
    if (len > 0xffff)
        len = 0xffff;
    static uint8_t kbuf[1500];
    // streams take the buffer in chunks; datagrams go in one shot
    long sent = 0;
    while (sent < (long)len) {
        uint32_t n = len - (uint32_t)sent;
        if (n > sizeof(kbuf))
            n = sizeof(kbuf);
        memcpy(kbuf, (const uint8_t *)buf + sent, n);
        long rc = net_sendto(sfd, kbuf, (uint16_t)n, ip, port);
        if (rc < 0) {
            if (!sent)
                sent = len > sizeof(kbuf) ? -EMSGSIZE : rc;
            break;
        }
        if (rc == 0)
            break;               // stream buffer full / nothing queued
        sent += rc;
    }
    if (!sent && !to)
        sent = -EDESTADDRREQ;
    r->rax = (uint64_t)sent;
    return (uint64_t)r;
}

static uint64_t sys_recvfrom(struct regs *r) {
    int fd = (int)r->rdi;
    void *buf = (void *)r->rsi;
    uint16_t len = (uint16_t)r->rdx;
    struct sockaddr_in_k *from = (struct sockaddr_in_k *)r->r10;
    if (!buf || !len ||
        !task_user_range_ok(current, (uint64_t)buf, len, 1)) {
        r->rax = (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    int sfd = sock_slot(fd, 0);
    if (sfd < 0) {
        r->rax = (uint64_t)-ENOTSOCK;
        return (uint64_t)r;
    }
    uint32_t sip = 0;
    uint16_t sport = 0;
    static uint8_t kbuf[1500];
    uint16_t clen = len > sizeof(kbuf) ? (uint16_t)sizeof(kbuf) : len;
    long n = net_recvfrom(sfd, kbuf, clen, &sip, &sport);
    if (n > 0 && from) {
        if (!task_user_range_ok(current, (uint64_t)from,
                                sizeof(*from), 1)) {
            r->rax = (uint64_t)-EFAULT;
            return (uint64_t)r;
        }
        from->family = 2;
        from->port = __builtin_bswap16(sport);
        from->addr = __builtin_bswap32(sip);
    }
    if (n > 0)
        memcpy(buf, kbuf, (uint64_t)n);
    r->rax = (uint64_t)n;    // 0 = eof/nothing queued (non-blocking)
    return (uint64_t)r;
}

static uint64_t sys_bind(struct regs *r) {
    int fd = (int)r->rdi;
    struct sockaddr_in_k *sa = (struct sockaddr_in_k *)r->rsi;
    if (!sa || !task_user_range_ok(current, (uint64_t)sa, sizeof(*sa), 0)) {
        r->rax = (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    int sfd = sock_slot(fd, 0);
    if (sfd < 0) {
        r->rax = (uint64_t)-ENOTSOCK;
        return (uint64_t)r;
    }
    if (sa->family != 2) {
        r->rax = (uint64_t)-EAFNOSUPPORT;
        return (uint64_t)r;
    }
    uint32_t ip = __builtin_bswap32(sa->addr);
    if (ip && ip != net_local_ip && ip != 0x7f000001) {
        r->rax = (uint64_t)-EINVAL;
        return (uint64_t)r;
    }
    uint16_t port = __builtin_bswap16(sa->port);
    r->rax = (uint64_t)(net_bind(sfd, port) < 0 ? -EINVAL : 0);
    return (uint64_t)r;
}

static uint64_t sys_listen(struct regs *r) {
    int fd = (int)r->rdi;
    int backlog = (int)r->rsi;
    int sfd = sock_slot(fd, 0);
    if (sfd < 0) {
        r->rax = (uint64_t)-ENOTSOCK;
        return (uint64_t)r;
    }
    if (backlog < 0)
        backlog = 0;
    r->rax = (uint64_t)(net_listen(sfd, backlog) < 0 ? -EINVAL : 0);
    return (uint64_t)r;
}

static uint64_t sys_connect(struct regs *r) {
    int fd = (int)r->rdi;
    struct sockaddr_in_k *sa = (struct sockaddr_in_k *)r->rsi;
    if (!sa || !task_user_range_ok(current, (uint64_t)sa, sizeof(*sa), 0)) {
        r->rax = (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    int sfd = sock_slot(fd, 0);
    if (sfd < 0) {
        r->rax = (uint64_t)-ENOTSOCK;
        return (uint64_t)r;
    }
    if (sa->family != 2) {
        r->rax = (uint64_t)-EAFNOSUPPORT;
        return (uint64_t)r;
    }
    uint32_t ip = __builtin_bswap32(sa->addr);
    uint16_t port = __builtin_bswap16(sa->port);
    long rc = net_connect(sfd, ip, port);
    long out;
    switch (rc) {
    case 0:  out = 0; break;
    case -9: out = -EBADF; break;
    case -104: out = -ECONNRESET; break;
    case -106: out = -EISCONN; break;
    case -110: out = -ETIMEDOUT; break;
    case -114: out = -EALREADY; break;
    case -115: out = -EINPROGRESS; break;
    default: out = -EINVAL; break;
    }
    r->rax = (uint64_t)out;
    return (uint64_t)r;
}

static uint64_t sys_accept(struct regs *r) {
    int fd = (int)r->rdi;
    struct sockaddr_in_k *addr = (struct sockaddr_in_k *)r->rsi;
    uint32_t *alen = (uint32_t *)r->rdx;
    int lsock = sock_slot(fd, 0);
    if (lsock < 0) {
        r->rax = (uint64_t)-ENOTSOCK;
        return (uint64_t)r;
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    long slot = net_accept(lsock, &ip, &port);
    if (slot < 0) {
        r->rax = (uint64_t)-EAGAIN;
        return (uint64_t)r;
    }
    struct file *f = sock_file_alloc((int)slot);
    if (!f) {
        net_close((int)slot);
        r->rax = (uint64_t)-ENOMEM;
        return (uint64_t)r;
    }
    int nfd = task_fd_alloc(f);
    if (nfd < 0) {
        vfs_close(f);
        r->rax = (uint64_t)-EMFILE;
        return (uint64_t)r;
    }
    if (addr && task_user_range_ok(current, (uint64_t)addr,
                                   sizeof(*addr), 1)) {
        addr->family = 2;
        addr->port = __builtin_bswap16(port);
        addr->addr = __builtin_bswap32(ip);
        if (alen && task_user_range_ok(current, (uint64_t)alen, 4, 1))
            *alen = sizeof(*addr);
    }
    r->rax = (uint64_t)nfd;
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
    // ours. this is syscall ENTRY — the syscall has not run, so the
    // delivery decides its fate (SA_RESTART replays, otherwise the frame
    // reports -EINTR; see signal_deliver_entry)
    uint64_t fr;
    int act = signal_deliver_entry(r, &fr);
    if (act)
        return fr;   // 1: handler frame in place; 2: killed, already scheduled
    current->sig_woke_rewind = 0;   // stale only if no delivery consumed it

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
    case SYS_mremap:  fr = sys_mremap(r); break;
    case SYS_madvise: fr = sys_madvise(r); break;
    case SYS_brk:     fr = sys_brk(r); break;
    case SYS_futex:   fr = sys_futex(r); break;
    case SYS_getdents64: fr = sys_getdents64(r); break;
    case SYS_getcwd:  fr = sys_getcwd(r); break;
    case SYS_chdir:   fr = sys_chdir(r); break;
    case SYS_pipe:    fr = sys_pipe(r); break;
    case SYS_pipe2:   fr = sys_pipe2(r); break;
    case SYS_dup:     fr = sys_dup(r); break;
    case SYS_dup2:    fr = sys_dup2(r); break;
    case SYS_dup3:    fr = sys_dup3(r); break;
    case SYS_fcntl:   fr = sys_fcntl(r); break;
    case SYS_mkdir:   fr = sys_mkdir(r); break;
    case SYS_rmdir:   fr = sys_rmdir(r); break;
    case SYS_unlink:  fr = sys_unlink(r); break;
    case SYS_rename:  fr = sys_rename(r); break;
    case SYS_symlink: fr = sys_symlink(r); break;
    case SYS_readlink: fr = sys_readlink(r); break;
    case SYS_ftruncate: fr = sys_ftruncate(r); break;
    case SYS_chmod:   fr = sys_chmod(r); break;
    case SYS_fchmod:  fr = sys_fchmod(r); break;
    case SYS_fchmodat: fr = sys_fchmodat(r); break;
    case SYS_link:    fr = sys_link(r); break;
    case 162 /* SYS_sync */: r->rax = 0; fr = (uint64_t)r; break;
    case SYS_sysinfo: fr = sys_sysinfo(r); break;
    case SYS_times:   fr = sys_times(r); break;
    case SYS_getrusage: fr = sys_getrusage(r); break;
    case SYS_faccessat: fr = sys_faccessat(r); break;
    case 21 /* SYS_access */: fr = sys_access(r); break;
    case SYS_gettimeofday: fr = sys_gettimeofday(r); break;
    case SYS_setpgid: fr = sys_setpgid(r); break;
    case SYS_getpgid: fr = sys_getpgid(r); break;
    case SYS_sigaltstack: fr = sys_sigaltstack(r); break;
    case SYS_rt_sigpending: {
        // posix sigpending: pending, blocked or not
        uint64_t *u = (uint64_t *)r->rdi;
        if (!u || r->rsi != 8) {
            r->rax = -EINVAL;
        } else {
            *u = current->sig_pending;
            r->rax = 0;
        }
        fr = (uint64_t)r;
        break;
    }
    case SYS_utimensat: fr = sys_utimensat(r); break;
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
    case SYS_clock_nanosleep: fr = sys_clock_nanosleep(r); break;
    case SYS_pause:     fr = sys_pause(r); break;
    case SYS_rt_sigsuspend: fr = sys_sigsuspend(r); break;
    case SYS_rt_sigtimedwait: fr = sys_sigtimedwait(r); break;
    case SYS_setsid:    fr = sys_setsid(r); break;
    case SYS_clock_gettime: fr = sys_clock_gettime(r); break;
    case SYS_time:    fr = sys_time(r); break;
    case SYS_sched_getaffinity: fr = sys_sched_getaffinity(r); break;
    case SYS_nice:        fr = sys_nice(r); break;
    case SYS_getpriority: fr = sys_getpriority(r); break;
    case SYS_setpriority: fr = sys_setpriority(r); break;
    case SYS_prlimit64: fr = sys_prlimit64(r); break;
    case SYS_getrandom: fr = sys_getrandom(r); break;
    case 41 /* socket */:    fr = sys_socket(r); break;
    case 42 /* connect */:   fr = sys_connect(r); break;
    case 43 /* accept */:    fr = sys_accept(r); break;
    case 44 /* sendto */:    fr = sys_sendto(r); break;
    case 45 /* recvfrom */:  fr = sys_recvfrom(r); break;
    case 49 /* bind */:      fr = sys_bind(r); break;
    case 50 /* listen */:    fr = sys_listen(r); break;
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
