// sigtest: glibc signals on Coonix — rt_sigaction, rt_sigprocmask,
// tgkill (raise), delivery on syscall boundary and timer tick, a SIGSEGV
// handler catching a real page fault, siglongjmp out of the handler.
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int got_usr1;
static volatile int got_tick;
static jmp_buf recover;

static void h_usr1(int sig) {
    (void)sig;
    got_usr1++;
}

static void h_usr2(int sig) {
    (void)sig;
    got_tick++;
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
    waitpid(pid, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) {
        printf("sigtest: wait status %#x, want SIGTERM death\n", st);
        return 1;
    }
    printf("sigtest: child death by signal reported correctly\n");

    printf("sigtest: OK\n");
    return 0;
}
