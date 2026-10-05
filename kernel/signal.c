#include "signal.h"
#include "task.h"
#include "string.h"
#include "serial.h"
#include "syscall.h"
#include "vmm.h"

// minimal errno set for signals
#define EPERM    1
#define EINTR    4
#define ESRCH    3
#define EINVAL  22
#define ENOMEM  12
#define EFAULT  14

#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048

// sigframe layout: the packed glibc-compatible frame, then a 512-byte
// fxsave area (16-aligned) the fpstate field points at
#define FXAREA_SIZE 528   // 512 + pad to keep the frame size 16-aligned

#define SIGBIT(sig) (1ULL << ((sig) - 1))

// glibc-compatible rt_sigframe. rt_sigreturn finds it at (user rsp - 8):
// after the handler returns, rsp points just past pretcode, i.e. at uc.
struct rt_sigframe_k {
    uint64_t pretcode;      // +0   user restorer (SA_RESTORER)
    uint64_t uc_flags;      // +8
    uint64_t uc_link;       // +16
    uint64_t uc_stack[3];   // +24  ss_sp, ss_flags, ss_size (we zero)
    uint64_t gregs[23];     // +48  mcontext.gregs
    uint64_t fpstate;       // +232 0 (no fp save: user is mno-sse-ish)
    uint64_t reserved[8];   // +240
    uint64_t info[16];      // +304 siginfo, zeroed
} __attribute__((packed));

_Static_assert(sizeof(struct rt_sigframe_k) == 432, "sigframe size");

// glibc gregs indices
enum {
    GR_R8 = 0, GR_R9, GR_R10, GR_R11, GR_R12, GR_R13, GR_R14, GR_R15,
    GR_RDI, GR_RSI, GR_RBP, GR_RBX, GR_RDX, GR_RAX, GR_RCX, GR_RSP,
    GR_RIP, GR_EFL, GR_CSGSFS, GR_ERR, GR_TRAPNO, GR_OLDMASK, GR_CR2
};

void signal_init(void) {
    for (int i = 0; i < TASK_MAX; i++)
        memset(task_table[i].sigact, 0, sizeof(task_table[i].sigact));
}

int signal_default_ignores(int sig) {
    return sig == SIGCHLD || sig == SIGWINCH || sig == SIGCONT ||
           sig == 23 || sig == 30;   // URG, PWR
}

long signal_sys_rt_sigaction(int sig, const struct k_sigaction *act,
                             struct k_sigaction *oact, uint64_t sigsetsize) {
    if (sig < 1 || sig > SIG_MAX || sigsetsize != 8)
        return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    if (oact &&
        !task_user_range_ok(current, (uint64_t)oact, sizeof(*oact), 1))
        return -EFAULT;
    if (act &&
        !task_user_range_ok(current, (uint64_t)act, sizeof(*act), 0))
        return -EFAULT;
    if (oact) {
        memcpy((void *)oact, &current->sigact[sig], sizeof(*oact));
    }
    if (act) {
        struct k_sigaction sa;
        memcpy(&sa, act, sizeof(sa));
        // like linux x86_64: a restorer is mandatory, handlers must be
        // in user space
        if (!(sa.flags & SA_RESTORER) || !sa.restorer)
            return -EINVAL;
        if (sa.handler && sa.handler != 1 &&
            (sa.handler < 0x1000 || sa.handler >= 0x800000000000ULL))
            return -EINVAL;
        sa.mask &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
        current->sigact[sig] = sa;
    }
    return 0;
}

long signal_sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                               uint64_t sigsetsize) {
    if (sigsetsize != 8)
        return -EINVAL;
    if (oldset &&
        !task_user_range_ok(current, (uint64_t)oldset, 8, 1))
        return -EFAULT;
    if (set && !task_user_range_ok(current, (uint64_t)set, 8, 0))
        return -EFAULT;
    if (oldset)
        *oldset = current->sig_mask;
    if (set) {
        uint64_t m = *set;
        m &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
        if (how == SIG_BLOCK)
            current->sig_mask |= m;
        else if (how == SIG_UNBLOCK)
            current->sig_mask &= ~m;
        else if (how == SIG_SETMASK)
            current->sig_mask = m;
        else
            return -EINVAL;
    }
    return 0;
}

int signal_send_task(struct task *t, int sig) {
    if (sig < 1 || sig > SIG_MAX)
        return 1;
    if (sig == SIGKILL || sig == SIGSTOP) {
        // unblockable: force through
        t->sigact[sig].handler = 0;
    }
    uint64_t bit = SIGBIT(sig);
    uint64_t handler = t->sigact[sig].handler;
    // catchable = a delivery would do anything at all. ignored (SIG_IGN
    // or default-ignorable) sends are dropped — except a sigwait, which
    // accepts ignored signals (posix)
    int catchable = handler != 1 &&
                    !(handler == 0 && signal_default_ignores(sig));
    int wait_signal = t->state == T_BLOCKED &&
                      t->wait_reason == WAIT_SIGNAL;
    int sigwait_accept = wait_signal && t->sig_wait_kind == SW_SIGWAIT &&
                         (t->sig_wait_set & bit);
    if (handler == 1 && !sigwait_accept &&
        sig != SIGKILL && sig != SIGSTOP)
        return 0;   // dropped
    t->sig_pending |= bit;
    if (t->state != T_BLOCKED)
        return 0;
    if (t->wait_reason != WAIT_SIGNAL) {
        // generic replay wake: read/pipe/kbd/wait4 re-run their syscall
        t->state = T_READY;
        return 0;
    }
    if (sigwait_accept) {
        // accepted by the wait itself: consumed, no handler runs. the
        // signal number is parked on the task; the re-entered syscall
        // body reports it. rewind the frame so the body re-enters
        t->sig_pending &= ~bit;
        t->sig_wait_result = sig;
        t->sig_woke_rewind = 1;
        struct regs *fr = (struct regs *)t->rsp;
        if (fr->int_no == 128 || fr->int_no == 64)
            fr->rip -= 2;
        t->state = T_READY;
        return 0;
    }
    // deliverable under the CURRENT mask: for rt_sigsuspend that is the
    // temp mask from the argument (posix: sigsuspend wakes for signals
    // not in the arg; the handler then runs with the original restored
    // by rt_sigreturn, see deliver_one). ignored/dead signals never wake
    int deliverable = !(t->sig_mask & bit);
    if (catchable && deliverable)
        t->state = T_READY;   // rip already rewound: re-entry delivers
    return 0;
}

int signal_send_group(int tgid, int sig) {
    // to the group leader first (it owns the process semantics)
    for (int i = 0; i < TASK_MAX; i++)
        if (task_table[i].state != T_FREE &&
            task_table[i].tgid == tgid && task_table[i].pid == tgid)
            return signal_send_task(&task_table[i], sig);
    return 2;
}

int signal_send_pgid(int pgid, int sig) {
    int sent = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state != T_FREE && t->pgid == pgid) {
            signal_send_task(t, sig);
            sent++;
        }
    }
    return sent ? 0 : 2;
}

static void set_gregs(uint64_t *g, const struct regs *r) {
    g[GR_R8] = r->r8;    g[GR_R9] = r->r9;    g[GR_R10] = r->r10;
    g[GR_R11] = r->r11;  g[GR_R12] = r->r12;  g[GR_R13] = r->r13;
    g[GR_R14] = r->r14;  g[GR_R15] = r->r15;  g[GR_RDI] = r->rdi;
    g[GR_RSI] = r->rsi;  g[GR_RBP] = r->rbp;  g[GR_RBX] = r->rbx;
    g[GR_RDX] = r->rdx;  g[GR_RAX] = r->rax;  g[GR_RCX] = r->rcx;
    g[GR_RSP] = r->rsp;  g[GR_RIP] = r->rip;  g[GR_EFL] = r->rflags;
    g[GR_CSGSFS] = (r->cs & 0xffff) | (2u << 16);   // cs | gs=2 (ph)
    g[GR_ERR] = r->err;
    g[GR_TRAPNO] = r->int_no;
    g[GR_OLDMASK] = 0;   // filled by the caller
    g[GR_CR2] = 0;
}

// rewrite the user frame so the handler runs with a valid way back.
// returns the frame rsp to resume (or a schedule() result when the
// default action kills the task; *killed tells the caller which one).
// 0 = the signal needed no delivery (ignored/default-ignore): the frame
// is UNTOUCHED and the pending bit is already consumed — the interrupted
// syscall must run normally (returning the same frame as "handled" made
// the dispatcher skip it forever: user re-executed the insn, the same
// pending bit hit again, an endless re-entry loop for rt_sigreturn)
#define DELIVER_NOOP 0ULL

static uint64_t deliver_one(struct regs *r, int sig, int *killed,
                            int at_syscall_entry) {
    *killed = 0;
    struct k_sigaction *sa = &current->sigact[sig];
    uint64_t handler = sa->handler;

    // default action
    if (handler == 0) {
        if (signal_default_ignores(sig)) {
            current->sigact[sig].handler = 1;   // ignore from now on
            return DELIVER_NOOP;
        }
        *killed = 1;
        return task_exit_current_sig(sig);
    }

    // SIG_IGN while pending (installed after the signal was queued):
    // discard quietly. falling through would build a handler frame with
    // rip = 1 and jump into the void
    if (handler == 1)
        return DELIVER_NOOP;

    // user handler: build the frame below the interrupted rsp (or on the
    // alternate signal stack when SA_ONSTACK asks for it)
    uint64_t base = r->rsp;
    int on_alt = 0;
    if ((sa->flags & SA_ONSTACK) && current->alt_size &&
        !(base >= current->alt_sp &&
          base < current->alt_sp + current->alt_size)) {
        base = current->alt_sp + current->alt_size;
        on_alt = 1;
    }
    // the frame write targets user memory picked from user state: a tiny
    // rsp would wrap below 0 into the kernel half. broken stack = the
    // handler cannot run: fall back from altstack, else kill (posix)
#define SIGFRAME_BYTES (sizeof(struct rt_sigframe_k) + FXAREA_SIZE + 16)
    uint64_t sp = (base - 128) & ~0xfULL;
    sp -= SIGFRAME_BYTES;
    if (!task_user_range_ok(current, sp, SIGFRAME_BYTES, 1) && on_alt) {
        base = r->rsp;
        sp = (base - 128) & ~0xfULL;
        sp -= SIGFRAME_BYTES;
    }
    if (!task_user_range_ok(current, sp, SIGFRAME_BYTES, 1)) {
        *killed = 1;
        return task_exit_current_sig(SIGSEGV);
    }
    struct rt_sigframe_k *f = (struct rt_sigframe_k *)sp;
    memset(f, 0, sizeof(*f));
    set_gregs(f->gregs, r);
    f->pretcode = sa->restorer;
    // siginfo pointer: process only reads si_signo/si_code realistically
    f->info[0] = (uint64_t)sig;    // si_signo
    f->info[2] = -6;               // si_code = SI_TKILL
    // ucontext.uc_stack: reports the altstack (glibc makecontext users)
    f->uc_stack[0] = current->alt_sp;
    f->uc_stack[2] = current->alt_size;

    // old mask for rt_sigreturn: normally the mask at delivery time, but
    // a rt_sigsuspend waiter delivers under its TEMP mask and must resume
    // with the ORIGINAL one (posix: sigsuspend restores the old mask)
    uint64_t oldmask = current->sig_mask;
    if (current->sig_wait_kind == SW_SUSPEND) {
        oldmask = current->sig_wait_saved_mask;
        current->sig_wait_kind = 0;   // consumed
    }
    f->gregs[GR_OLDMASK] = oldmask;

    // fpu: snapshot the interrupted state into the frame. the handler
    // runs with live xmm/x87 registers and clobbers them freely; without
    // the frame copy the interrupted code resumes on the handler's trash
    uint64_t fxva = ((uint64_t)f + sizeof(*f) + 15) & ~15ULL;
    if (current->fpu_ready) {
        __asm__ volatile("fxsave (%0)" :: "r"(fxva) : "memory");
        f->fpstate = fxva;
    }

    // a syscall interrupted BEFORE it ran: SA_RESTART replays it (rip
    // rewound onto the instruction, rax keeps the number — the same
    // rewind blocking syscalls already carry); otherwise the frame
    // returns -EINTR. sigreturn/exit replay unconditionally: faking
    // their result would skip a frame restore or let exit()ed code run
    if (at_syscall_entry && (r->int_no == 128 || r->int_no == 64)) {
        // sig_woke_rewind: the frame was already rewound when the task
        // blocked (replay_fixup), so the rewind must not repeat (a double
        // rewind would re-execute the middle of the mov before the int)
        int woken = current->sig_woke_rewind;
        current->sig_woke_rewind = 0;
        // pause/sigsuspend/sigtimedwait never restart, even with
        // SA_RESTART set (posix: they report -EINTR after the handler)
        int force_eintr = !(sa->flags & SA_RESTART) ||
            r->rax == SYS_rt_sigreturn || r->rax == SYS_exit ||
            r->rax == SYS_exit_group || r->rax == SYS_execve ||
            r->rax == SYS_pause || r->rax == SYS_rt_sigsuspend ||
            r->rax == SYS_rt_sigtimedwait;
        if (force_eintr) {
            f->gregs[GR_RAX] = (uint64_t)-EINTR;
            if (woken)
                f->gregs[GR_RIP] += 2;   // unwind the block-time rewind
        } else if (!woken) {
            f->gregs[GR_RIP] -= 2;      // fresh entry: re-execute
        }
        // woken + SA_RESTART: rip already sits on the insn, keep it
    }

    current->sig_mask |= sa->mask;
    if (!(sa->flags & SA_NODEFER))
        current->sig_mask |= SIGBIT(sig);
    if (sa->flags & SA_RESETHAND)
        current->sigact[sig].handler = 0;   // SIG_DFL on re-entry

    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)(void *)f + __builtin_offsetof(struct rt_sigframe_k, info);
    r->rdx = (uint64_t)(void *)f + __builtin_offsetof(struct rt_sigframe_k, uc_flags);
    r->rax = 0;
    r->rip = handler;
    r->rsp = (uint64_t)&f->pretcode;
    return (uint64_t)r;
}

static int signal_deliver_inner(struct regs *r, uint64_t *out, int at_entry) {
    *out = (uint64_t)r;
    if (!current || !(r->cs & 3))
        return 0;
    uint64_t deliverable = current->sig_pending & ~current->sig_mask;
    if (!deliverable)
        return 0;
    for (int sig = 1; sig <= SIG_MAX; sig++) {
        if (deliverable & SIGBIT(sig)) {
            current->sig_pending &= ~SIGBIT(sig);
            int killed = 0;
            uint64_t fr = deliver_one(r, sig, &killed, at_entry);
            if (fr == DELIVER_NOOP) {
                *out = (uint64_t)r;   // frame untouched
                return 0;             // the syscall proceeds normally
            }
            *out = fr;
            return killed ? 2 : 1;
        }
    }
    return 0;
}

int signal_deliver(struct regs *r, uint64_t *out) {
    return signal_deliver_inner(r, out, 0);
}

int signal_deliver_entry(struct regs *r, uint64_t *out) {
    return signal_deliver_inner(r, out, 1);
}

// sigaltstack(2). glibc stack_t is {void *ss_sp; int ss_flags; size_t
// ss_size}: sp at 0, flags as a 32-BIT int at 8 (the upper half is
// uninitialized padding — reading it as u64 broke the flag check),
// size at 16
long signal_sys_sigaltstack(const uint64_t *uss, uint64_t *ouss,
                            uint64_t user_rsp) {
    int on_alt = current->alt_size && user_rsp >= current->alt_sp &&
                 user_rsp < current->alt_sp + current->alt_size;
    if (ouss) {
        if (!task_user_range_ok(current, (uint64_t)ouss, 24, 1))
            return -EFAULT;
        ouss[0] = current->alt_sp;
        *((uint32_t *)ouss + 2) =
            !current->alt_size ? SS_DISABLE : (on_alt ? SS_ONSTACK : 0);
        ouss[2] = current->alt_size;
    }
    if (!uss)
        return 0;
    if (!task_user_range_ok(current, (uint64_t)uss, 24, 0))
        return -EFAULT;
    if (on_alt)
        return -EPERM;              // cannot swap stacks from itself
    uint32_t fl = *((const uint32_t *)uss + 2);
    if (fl & ~(uint32_t)SS_DISABLE)
        return -EINVAL;
    if (fl & SS_DISABLE) {
        current->alt_sp = 0;
        current->alt_size = 0;
        return 0;
    }
    if (uss[2] < MINSIGSTKSZ)
        return -ENOMEM;
    current->alt_sp = uss[0];
    current->alt_size = uss[2];
    return 0;
}

uint64_t signal_sigreturn(struct regs *r) {
    // user rsp at syscall time = just past pretcode = &uc_flags
    struct rt_sigframe_k *f = (struct rt_sigframe_k *)(r->rsp - 8);
    // the restore reads user-picked memory in kernel mode: a bad rsp
    // would page-fault inside the kernel; demand a mapped frame instead
    if (!task_user_range_ok(current, (uint64_t)f,
                           sizeof(struct rt_sigframe_k), 0))
        return task_exit_current_sig(SIGSEGV);
    // restore the interrupted fpu state saved at delivery time. only the
    // fxsave area of THIS frame is accepted: a crafted fpstate pointer
    // would feed fxrstor arbitrary memory
    if (f->fpstate) {
        uint64_t fxva = ((uint64_t)f + sizeof(*f) + 15) & ~15ULL;
        if (f->fpstate == fxva)
            __asm__ volatile("fxrstor (%0)" :: "r"(fxva) : "memory");
    }
    current->sig_mask = f->gregs[GR_OLDMASK];
    r->r8 = f->gregs[GR_R8];    r->r9 = f->gregs[GR_R9];
    r->r10 = f->gregs[GR_R10];  r->r11 = f->gregs[GR_R11];
    r->r12 = f->gregs[GR_R12];  r->r13 = f->gregs[GR_R13];
    r->r14 = f->gregs[GR_R14];  r->r15 = f->gregs[GR_R15];
    r->rdi = f->gregs[GR_RDI];  r->rsi = f->gregs[GR_RSI];
    r->rbp = f->gregs[GR_RBP];  r->rbx = f->gregs[GR_RBX];
    r->rdx = f->gregs[GR_RDX];  r->rax = f->gregs[GR_RAX];
    r->rcx = f->gregs[GR_RCX];
    r->rsp = f->gregs[GR_RSP];
    r->rip = f->gregs[GR_RIP];
    r->rflags = f->gregs[GR_EFL];
    return (uint64_t)r;
}
