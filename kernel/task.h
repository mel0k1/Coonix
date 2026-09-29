// tasks, round-robin scheduler, ring 3
#pragma once
#include <stdint.h>
#include "idt.h"
#include "vfs.h"

#define TASK_MAX 32
#define KSTACK_PAGES 4
#define KSTACK_SIZE (KSTACK_PAGES * 4096)
#define KSTACK_VA_BASE 0xffffffff90000000ULL  // per-pid slot, 64k each
#define USER_STACK_TOP 0x7ffffffff000ULL
#define USER_STACK_PAGES 16
#define USER_MMAP_BASE 0x600000000000ULL      // mmap region grows up

// signals 1..64 fit one u64 mask; bit(sig-1)
#define SIG_MAX 64
#define SIG_DFL 0
#define SIG_IGN 1

// what glibc passes to rt_sigaction (kernel_sigaction)
struct k_sigaction {
    uint64_t handler;     // SIG_DFL / SIG_IGN / user fn
    uint64_t flags;       // SA_*; SA_RESTORER must be set (like linux x86_64)
    uint64_t restorer;    // user trampoline that calls rt_sigreturn
    uint64_t mask;        // signals to block while handling
};

// mmap reservation (syscall 9/11); file = 0 for anonymous
struct mmap_region {
    uint64_t start, end;
    struct file *file;    // backing file, refcounted; 0 = anon
    uint64_t off;         // file offset of region start
    uint64_t prot;        // PROT_*
    uint64_t flags;       // MAP_* (we care about MAP_SHARED)
    struct mmap_region *next;
};

enum { T_FREE, T_READY, T_RUNNING, T_BLOCKED, T_ZOMBIE };
enum { WAIT_NONE = 0, WAIT_KBD = 1, WAIT_CHILD = 2, WAIT_SLEEP = 3,
       WAIT_PIPE = 4 };

struct task {
    int pid;              // thread id (tid)
    int tgid;             // thread group: the process id
    int pgid;             // console foreground group
    int state;
    int exit_code;        // wait4 sees: signal death -> the signal, else code<<8
    int sig_death;        // exit_code was a killing signal
    int wait_reason;      // WAIT_*
    void *wait_pipe;      // WAIT_PIPE: which pipe we sit on
    struct task *parent;
    uint64_t rsp;         // kernel rsp (top: struct regs)
    uint64_t kstack_top;  // virtual
    uint64_t pml4;        // phys; shared between threads of a group
    uint64_t wake_tick;   // nanosleep deadline (tick units)
    // cpu accounting (ticks, 100 Hz): utime/stime are this thread's
    // runtime, cutime/cstime sum the reaped children's totals
    uint64_t utime, stime;
    uint64_t cutime, cstime;
    uint64_t start_tick;  // spawn tick: /proc stat starttime, times()
    struct file *fds[FILE_MAX];
    uint8_t fd_flags[FILE_MAX];     // FD_CLOEXEC per fd
    char cwd[192];        // current directory (absolute, kernel-side)
    uint64_t brk_base;    // past the last elf segment
    uint64_t brk_cur;     // current program break
    struct mmap_region *mmaps;  // sorted by start
    uint64_t fs_base;     // user tls (arch_prctl)
    uint64_t gs_base;
    uint64_t clear_tid;   // set_tid_address
    uint64_t child_tid;   // clone CLONE_CHILD_CLEARTID: cleared + futex-woken on exit
    uint64_t sig_pending; // awaiting delivery
    uint64_t sig_mask;    // blocked
    struct k_sigaction sigact[SIG_MAX + 1];
    // syscall-entry scratch the gs base points at: {kstack_top, user_rsp}.
    // lives INSIDE the task slot: a heap-allocated scratch would dangle
    // after kfree (poisoned/reused) and a stale gs would send the syscall
    // entry onto another task's kernel stack
    uint64_t kgs_area[2];
    uint64_t kgs;         // = &kgs_area[0], loaded into MSR_GS_BASE
    // elf image info for auxv
    uint64_t phdr_va, phent, phnum, entry_va;
    // /proc metadata: comm = basename(argv[0]); cmdline = the argv block
    // adopted from execve (NUL-separated strings, kernel heap)
    char comm[16];
    char *cmdline;
    int cmdline_len;
};

extern struct task task_table[TASK_MAX];
extern struct task *current;

void task_init(void);
struct task *task_spawn_kernel(void (*entry)(void));
struct task *task_spawn_user(const char *path, struct task *parent);
void task_yield(void);           // called from irq context
uint64_t task_schedule(uint64_t old_rsp);
// summed utime/stime over every thread of the group (getrusage SELF, times)
void task_cpu_group(int tgid, uint64_t *utime, uint64_t *stime);
// direct switch to a specific task (clone returns straight into the child);
// caller must have stored current->rsp already
uint64_t task_switch_to(struct task *next);
struct task *task_fork(struct regs *frame);
// CLONE_THREAD clone: shared address space, new tid; 0 on failure
struct task *task_clone_thread(struct regs *frame, uint64_t flags,
                               uint64_t newsp, uint64_t parent_tid,
                               uint64_t child_tid, uint64_t tls);
uint64_t task_exit_current(int code);
uint64_t task_exit_current_group(int code);   // SYS_exit_group: kill threads
// killed by a signal: wait4 reports the signal number
uint64_t task_exit_current_sig(int sig);
void task_unmap_user(struct task *t);
void task_mmap_teardown(struct task *t);   // writeback shared, drop regions
void task_mmap_clone(struct task *dst, const struct task *src);
// lazy fill of a file-backed mapping on #PF; 1 = handled
int task_mmap_fault(struct regs *r, uint64_t cr2);
struct task *task_find_free(void);
void task_wake_kbd(void);
void task_wake_pipe(struct pipe *p);   // unblock WAIT_PIPE tasks on p
void task_tick_wake(void);      // unblock nanosleep deadlines
void task_reap(void);           // idle loop: free parked kstacks
int task_frame_owned(struct regs *fr);   // forensics
int task_count_group(int tgid, struct task *except);
// exec current task with a new image from a vnode; returns new frame rsp, 0 on fail
uint64_t task_exec_current(struct vnode *vn);
// same, but name becomes argv[0] and auxv gets a fresh elf image info
uint64_t task_exec_current_named(struct vnode *vn, const char *name);
// full execve: caller-captured argv/envp (kernel-side copies, each a
// single kmalloc block of NUL-separated strings). both may be 0
typedef struct exec_args {
    int argc, envc;
    char *argv;         // block: str\0str\0...\0
    char *envp;
} exec_args_t;
uint64_t task_execve(struct vnode *vn, const char *name,
                     const exec_args_t *ea);
// /proc comm: basename of name into t->comm
void task_set_comm(struct task *t, const char *name);
// lowest free fd >= 0, or -1
int task_fd_alloc(struct file *f);
void task_close_fds(struct task *t, int keep_console);
