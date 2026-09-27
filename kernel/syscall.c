#include "syscall.h"
#include "task.h"
#include "kbd.h"
#include "console.h"
#include "string.h"
#include "vmm.h"
#include "gdt.h"
#include "vfs.h"

// every handler sets r->rax and returns current frame rsp;
// blocking ones return a switched rsp instead

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
    struct vnode *vn = vfs_resolve(path);
    if (!vn) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
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
