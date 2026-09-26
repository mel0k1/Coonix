#include "syscall.h"
#include "task.h"
#include "kbd.h"
#include "console.h"
#include "string.h"
#include "elf.h"
#include "vmm.h"
#include "gdt.h"

// user programs for execve (objcopy'ed, see Makefile)
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

// every handler sets r->rax and returns current frame rsp;
// blocking ones return a switched rsp instead

static uint64_t sys_write(struct regs *r) {
    const char *buf = (const char *)r->rsi;
    uint64_t len = r->rdx;
    for (uint64_t i = 0; i < len; i++)
        console_putc(buf[i]);
    r->rax = len;
    return (uint64_t)r;
}

static uint64_t sys_read(struct regs *r) {
    char *buf = (char *)r->rsi;
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

static uint64_t sys_execve(struct regs *r) {
    const char *name = (const char *)r->rdi;
    uint64_t size;
    const uint8_t *image = prog_find(name, &size);
    if (!image) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    uint64_t entry = elf_load_user(current->pml4, image, size);
    if (!entry) {
        r->rax = -1ULL;
        return (uint64_t)r;
    }
    // fresh user image: rebuild iret frame on our kernel stack
    struct regs *fr = (struct regs *)current->rsp;
    memset(fr, 0, sizeof(*fr));
    fr->rip = entry;
    fr->cs = SEL_UCODE | 3;
    fr->rflags = 0x202;
    fr->rsp = USER_STACK_TOP - 16;
    fr->ss = SEL_UDATA | 3;
    r->rax = 0;
    return (uint64_t)fr;
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
