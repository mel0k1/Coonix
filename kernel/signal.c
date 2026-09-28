#include "signal.h"
#include "task.h"
#include "string.h"
#include "serial.h"

// EBADF..: minimal errno set for signals
#define ESRCH    3
#define EINVAL  22

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
    if (t->sigact[sig].handler == 1 /* SIG_IGN */ &&
        sig != SIGKILL && sig != SIGSTOP)
        return 0;   // dropped
    t->sig_pending |= SIGBIT(sig);
    if (t->state == T_BLOCKED)
        t->state = T_READY;   // replay path will deliver
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
// default action kills the task; *killed tells the caller which one)
static uint64_t deliver_one(struct regs *r, int sig, int *killed) {
    *killed = 0;
    struct k_sigaction *sa = &current->sigact[sig];
    uint64_t handler = sa->handler;

    // default action
    if (handler == 0) {
        if (signal_default_ignores(sig)) {
            current->sigact[sig].handler = 1;   // ignore from now on
            return (uint64_t)r;
        }
        *killed = 1;
        return task_exit_current_sig(sig);
    }

    // user handler: build the frame below the interrupted rsp
    uint64_t sp = (r->rsp - 128) & ~0xfULL;
    sp -= sizeof(struct rt_sigframe_k);
    struct rt_sigframe_k *f = (struct rt_sigframe_k *)sp;
    memset(f, 0, sizeof(*f));
    set_gregs(f->gregs, r);
    f->gregs[GR_OLDMASK] = current->sig_mask;
    f->pretcode = sa->restorer;
    // siginfo pointer: process only reads si_signo/si_code realistically
    f->info[0] = (uint64_t)sig;    // si_signo
    f->info[2] = -6;               // si_code = SI_TKILL

    uint64_t old_mask = current->sig_mask;
    current->sig_mask |= sa->mask | SIGBIT(sig);

    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)(void *)f + __builtin_offsetof(struct rt_sigframe_k, info);
    r->rdx = (uint64_t)(void *)f + __builtin_offsetof(struct rt_sigframe_k, uc_flags);
    r->rax = 0;
    r->rip = handler;
    r->rsp = (uint64_t)&f->pretcode;
    (void)old_mask;
    return (uint64_t)r;
}

int signal_deliver(struct regs *r, uint64_t *out) {
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
            *out = deliver_one(r, sig, &killed);
            return killed ? 2 : 1;
        }
    }
    return 0;
}

uint64_t signal_sigreturn(struct regs *r) {
    // user rsp at syscall time = just past pretcode = &uc_flags
    struct rt_sigframe_k *f = (struct rt_sigframe_k *)(r->rsp - 8);
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
