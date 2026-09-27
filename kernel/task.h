// tasks, round-robin scheduler, ring 3
#pragma once
#include <stdint.h>
#include "idt.h"
#include "vfs.h"

#define TASK_MAX 16
#define KSTACK_PAGES 4
#define KSTACK_SIZE (KSTACK_PAGES * 4096)
#define KSTACK_VA_BASE 0xffffffff90000000ULL  // per-pid slot, 64k each
#define USER_STACK_TOP 0x7ffffffff000ULL
#define USER_STACK_PAGES 16

enum { T_FREE, T_READY, T_RUNNING, T_BLOCKED, T_ZOMBIE };
enum { WAIT_NONE = 0, WAIT_KBD = 1, WAIT_CHILD = 2 };

struct task {
    int pid;
    int state;
    int exit_code;
    int wait_reason;      // 0 none, 1 kbd
    struct task *parent;
    uint64_t rsp;         // kernel rsp (top: struct regs)
    uint64_t kstack_top;  // virtual
    uint64_t pml4;        // phys
    uint64_t wake_tick;
    struct file *fds[FILE_MAX];
};

extern struct task task_table[TASK_MAX];
extern struct task *current;

void task_init(void);
struct task *task_spawn_kernel(void (*entry)(void));
struct task *task_spawn_user(const char *path, struct task *parent);
void task_yield(void);           // called from irq context
uint64_t task_schedule(uint64_t old_rsp);
struct task *task_fork(struct regs *frame);
uint64_t task_exit_current(int code);
void task_unmap_user(struct task *t);
struct task *task_find_free(void);
void task_wake_kbd(void);
// exec current task with a new image from a vnode; returns new frame rsp, 0 on fail
uint64_t task_exec_current(struct vnode *vn);
// lowest free fd >= 0, or -1
int task_fd_alloc(struct file *f);
void task_close_fds(struct task *t, int keep_console);
