// posix-ish signals: rt_sigaction/procmask/return, kill, delivery.
// delivery replaces the user frame in place: handler runs, and the user
// restorer calls rt_sigreturn which restores the saved frame — so blocked
// syscalls replay naturally (SA_RESTART-style) after the handler.
#pragma once
#include <stdint.h>
#include "task.h"

// x86_64 signal numbers
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGWINCH  28
#define SIGCANCEL 32    // glibc nptl internals
#define SIGSETXID 33

// rt_sigaction SA_* flags we know
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

// sigprocmask hows
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

void signal_init(void);

// set/clear a pending bit; wakes a blocked target. returns 0 ok,
// -ESRCH-ish error codes: 1 bad sig, 2 no task
int signal_send_task(struct task *t, int sig);
int signal_send_group(int tgid, int sig);
int signal_send_pgid(int pgid, int sig);

// rt_sigaction(2)/rt_sigprocmask(2) syscall bodies; return 0 or -errno
long signal_sys_rt_sigaction(int sig, const struct k_sigaction *act,
                             struct k_sigaction *oact, uint64_t sigsetsize);
long signal_sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                               uint64_t sigsetsize);
// sigaltstack(2): uss/ouss are glibc stack_t {sp, flags, size} in user
// memory; user_rsp is the interrupted user rsp (on-altstack detection)
long signal_sys_sigaltstack(const uint64_t *uss, uint64_t *ouss,
                            uint64_t user_rsp);

// deliver pending signals by rewriting the user frame.
// return 0 = nothing was pending, frame untouched
// return 1 = handler frame built in r (caller must return *out right away:
//            the syscall must NOT run; rt_sigreturn will replay it)
// return 2 = the signal killed the task; *out is the next task's frame rsp
int signal_deliver(struct regs *r, uint64_t *out);
// variant for syscall ENTRY: the interrupted syscall has not run yet, so
// its fate is decided here — SA_RESTART replays it (rip rewound onto the
// instruction), otherwise the frame returns -EINTR; rt_sigreturn and the
// exit family always replay
int signal_deliver_entry(struct regs *r, uint64_t *out);

// rt_sigreturn(2): restore the frame saved at signal time
uint64_t signal_sigreturn(struct regs *r);

// default action ignores these signals
int signal_default_ignores(int sig);
