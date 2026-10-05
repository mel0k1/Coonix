// sigtest: glibc signals on Coonix — rt_sigaction, rt_sigprocmask,
// tgkill (raise), delivery on syscall boundary and timer tick, a SIGSEGV
// handler catching a real page fault, siglongjmp out of the handler,
// EINTR vs SA_RESTART at syscall entry, sigpending, sigaltstack,
// fpu state preserved across a handler (xmm round-trip).
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int got_usr1;
static volatile int got_tick;
static volatile int got_pend;
static volatile int got_eintr;
static volatile int got_restart;
static volatile int fp_hit;
static volatile int alt_ok;
static char altstk[16384] __attribute__((aligned(16)));
static jmp_buf recover;

static void h_usr1(int sig) {
    (void)sig;
    got_usr1++;
}

static void h_usr2(int sig) {
    (void)sig;
    got_tick++;
}

static void h_pend(int sig) {
    (void)sig;
    got_pend++;
}

static void h_eintr(int sig) {
    (void)sig;
    got_eintr++;
}

static void h_restart(int sig) {
    (void)sig;
    got_restart++;
}

static void h_alt(int sig) {
    stack_t cur;
    (void)sig;
    if (sigaltstack(0, &cur) == 0 && (cur.ss_flags & SS_ONSTACK)) {
        char probe;
        if (&probe >= altstk && &probe < altstk + sizeof(altstk))
            alt_ok = 1;
    }
}

static void h_fp(int sig) {
    static volatile double poison = 987.5;
    (void)sig;
    fp_hit = 1;
    __asm__ volatile ("movsd %0, %%xmm0" :: "m"(poison));
}

static void h_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    siglongjmp(recover, 1);        // bounce out of the fault
}

// spin without syscalls so delivery must come from the timer tick
static void busy_wait_for(int want) {
    for (int i = 0; i < 200000000L && got_tick < want; i++)
        __asm__ volatile("nop");
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// child helper: nap, then signal the parent, then die quietly
static void napper(double sec, int sig) {
    struct timespec ts = { (time_t)sec,
                           (long)((sec - (time_t)sec) * 1e9) };
    nanosleep(&ts, 0);
    kill(getppid(), sig);
    _exit(0);
}

int main(void) {
    struct sigaction sa;

    // 1. raise -> handler on the syscall return path
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, 0) < 0) {
        printf("sigtest: sigaction failed\n");
        return 1;
    }
    raise(SIGUSR1);
    if (got_usr1 != 1) {
        printf("sigtest: usr1 not delivered on syscall boundary (%d)\n", got_usr1);
        return 1;
    }

    // 2. block, raise (stays pending), unblock -> delivered
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blk, 0);
    raise(SIGUSR1);
    if (got_usr1 != 1) {
        printf("sigtest: delivered while blocked!\n");
        return 1;
    }
    sigprocmask(SIG_UNBLOCK, &blk, 0);
    if (got_usr1 != 2) {
        printf("sigtest: pending not delivered after unblock\n");
        return 1;
    }

    // 3. delivery from the timer tick (no syscalls in the spin loop)
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr2;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, 0);
    kill(getpid(), SIGUSR2);
    busy_wait_for(1);
    if (got_tick != 1) {
        printf("sigtest: usr2 not delivered from tick\n");
        return 1;
    }

    // 4. SIGSEGV handler + siglongjmp out
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = h_segv;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);
    if (sigsetjmp(recover, 1) == 0) {
        *(volatile int *)0 = 42;    // boom
        printf("sigtest: segfault did not fault?!\n");
        return 1;
    }
    printf("sigtest: survived SIGSEGV via siglongjmp\n");

    // 5. default action still terminates (child killed by SIGTERM)
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        raise(SIGTERM);
        _exit(0);
    }
    int st = 0;
    pid_t wrc = waitpid(pid, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) {
        printf("sigtest: rc=%d status %#x, want SIGTERM death\n", wrc, st);
        return 1;
    }
    printf("sigtest: child death by signal reported correctly\n");

    // 6. EINTR: a signal arriving while blocked in nanosleep, handler
    //    WITHOUT SA_RESTART -> the syscall must report -1/EINTR
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_eintr;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    pid = fork();
    if (pid == 0)
        napper(0.3, SIGUSR1);
    double t0 = now_s();
    struct timespec two = { 2, 0 };
    long rc = nanosleep(&two, 0);
    double dt = now_s() - t0;
    waitpid(pid, &st, 0);
    if (rc != -1 || errno != EINTR || got_eintr != 1 || dt >= 1.9) {
        printf("sigtest: want -1/EINTR fast, got rc=%ld errno=%d hits=%d "
               "dt=%.2f\n", rc, errno, got_eintr, dt);
        return 1;
    }
    printf("sigtest: interrupted nanosleep reports EINTR\n");

    // 7. SA_RESTART: same punch, handler WITH SA_RESTART -> nanosleep
    //    replays and completes normally
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_restart;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    pid = fork();
    if (pid == 0)
        napper(0.3, SIGUSR1);
    rc = nanosleep(&two, 0);
    waitpid(pid, &st, 0);
    if (rc != 0 || got_restart != 1) {
        printf("sigtest: SA_RESTART nanosleep rc=%ld errno=%d hits=%d\n",
               rc, errno, got_restart);
        return 1;
    }
    printf("sigtest: SA_RESTART replays the interrupted syscall\n");

    // 8. sigpending: blocked + raised is visible, survives syscalls,
    //    delivered on unblock
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_pend;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, 0);
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, 0);
    raise(SIGUSR2);
    sigset_t pend;
    sigpending(&pend);
    if (!sigismember(&pend, SIGUSR2)) {
        printf("sigtest: raised signal not in sigpending\n");
        return 1;
    }
    for (int i = 0; i < 5; i++)
        (void)getpid();          // pending must survive syscalls
    sigpending(&pend);
    if (!sigismember(&pend, SIGUSR2)) {
        printf("sigtest: pending set lost across syscalls\n");
        return 1;
    }
    sigprocmask(SIG_UNBLOCK, &blk, 0);
    if (got_pend != 1) {
        printf("sigtest: pending not delivered on unblock\n");
        return 1;
    }
    printf("sigtest: sigpending tracks blocked signals\n");

    // 9. sigaltstack + SA_ONSTACK: the handler really runs on the
    //    alternate stack and reports SS_ONSTACK
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstk;
    ss.ss_size = sizeof(altstk);
    if (sigaltstack(&ss, 0) < 0) {
        printf("sigtest: sigaltstack setup failed errno=%d\n", errno);
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_alt;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, 0);
    raise(SIGUSR2);
    if (!alt_ok) {
        printf("sigtest: handler did not run on the alt stack\n");
        return 1;
    }
    memset(&ss, 0, sizeof(ss));
    ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, 0) < 0) {
        printf("sigtest: sigaltstack disable failed\n");
        return 1;
    }
    printf("sigtest: SA_ONSTACK delivers on the alternate stack\n");

    // 10. fpu in the signal frame: xmm0 holds a marker across a syscall
    //     whose entry delivers a child-sent signal; the handler poisons
    //     xmm0; sigreturn must restore the interrupted value
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_fp;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    pid = fork();
    if (pid == 0)
        napper(0.3, SIGUSR1);
    double one = 1.0, got = 0.0;
    for (long i = 0; i < 5000000L && !fp_hit; i++) {
        __asm__ volatile (
            "movsd %1, %%xmm0\n\t"
            "movl $39, %%eax\n\t"        // SYS_getpid: the trap that delivers
            "int $0x80\n\t"
            "movsd %%xmm0, %0\n\t"
            : "=m"(got)
            : "m"(one)
            : "eax"
        );
    }
    waitpid(pid, &st, 0);
    if (!fp_hit) {
        printf("sigtest: fp handler never ran (loop raced out)\n");
        return 1;
    }
    if (got != 1.0) {
        printf("sigtest: xmm0 = %.1f after handler, want 1.0 "
               "(fpu frame broken)\n", got);
        return 1;
    }
    printf("sigtest: xmm state survives the handler\n");

    printf("sigtest: OK\n");
    return 0;
}
