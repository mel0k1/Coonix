// sigwaittest: signal waits on Coonix — pause(34), rt_sigsuspend(130),
// rt_sigtimedwait(128). posix ordering: the handler runs BEFORE the wait
// returns; sigsuspend restores the pre-call mask; sigwaitinfo consumes the
// signal without running any handler; timeouts report EAGAIN.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int got_usr1;
static volatile int got_usr2;
static volatile int handler_ran;

static void h_usr1(int sig) {
    (void)sig;
    got_usr1++;
}

static void h_usr2(int sig) {
    (void)sig;
    got_usr2++;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// child: nap, signal the parent, die quietly
static void napper(double sec, int sig) {
    struct timespec ts = { (time_t)sec,
                           (long)((sec - (time_t)sec) * 1e9) };
    nanosleep(&ts, 0);
    kill(getppid(), sig);
    _exit(0);
}

// child: nap, send sig_a, nap again, send sig_b, die
static void double_napper(double sec, int sig_a, int sig_b) {
    struct timespec ts = { (time_t)sec,
                           (long)((sec - (time_t)sec) * 1e9) };
    nanosleep(&ts, 0);
    kill(getppid(), sig_a);
    nanosleep(&ts, 0);
    kill(getppid(), sig_b);
    _exit(0);
}

static int fails;

static void check(int ok, const char *what) {
    if (ok) {
        printf("ok  %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        fails++;
    }
}

int main(void) {
    struct sigaction sa;
    sigset_t block_usr1, empty, oldmask;

    sigemptyset(&empty);
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);          // no SA_RESTART on purpose

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr2;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, 0);

    // 1. pause: handler runs BEFORE pause returns -EINTR (posix ordering)
    got_usr1 = 0;
    if (fork() == 0)
        napper(0.15, SIGUSR1);
    long rc = pause();
    check(rc == -1 && errno == EINTR, "pause returns -1/EINTR");
    check(got_usr1 == 1, "pause: handler ran before return");

    // 2. pause loop idiom (dash/ash style): keep pausing until the flag
    got_usr1 = 0;
    if (fork() == 0)
        napper(0.15, SIGUSR1);
    while (!got_usr1)
        pause();
    check(got_usr1 == 1, "pause loop exits on handler flag");

    // 3. pause is not woken by a default-ignored signal: SIGCHLD (default
    // action = ignore) is sent first and must be discarded; USR1 wakes later
    got_usr1 = 0;
    if (fork() == 0)
        double_napper(0.10, SIGCHLD, SIGUSR1);
    rc = pause();
    check(rc == -1 && errno == EINTR && got_usr1 == 1,
          "pause ignores default-ignored signal");

    // 4. sigsuspend: USR1 blocked + pending, sigsuspend(&empty) delivers
    // it, restores the original mask and reports -EINTR
    got_usr1 = 0;
    sigprocmask(SIG_BLOCK, &block_usr1, &oldmask);
    raise(SIGUSR1);
    rc = sigsuspend(&empty);
    sigset_t after;
    sigprocmask(SIG_BLOCK, 0, &after);
    int restored = sigismember(&after, SIGUSR1);
    check(rc == -1 && errno == EINTR, "sigsuspend returns -1/EINTR");
    check(got_usr1 == 1, "sigsuspend: handler ran before return");
    check(restored == 1, "sigsuspend restores the original mask");
    sigprocmask(SIG_SETMASK, &oldmask, 0);

    // 5. sigsuspend is not woken by an ignored signal
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, 0);
    got_usr1 = 0;
    if (fork() == 0)
        double_napper(0.10, SIGUSR2, SIGUSR1);
    rc = sigsuspend(&empty);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr2;
    sigaction(SIGUSR2, &sa, 0);
    check(rc == -1 && errno == EINTR && got_usr1 == 1,
          "sigsuspend ignores SIG_IGN signal");

    // 6. sigwaitinfo consumes a pending signal; NO handler runs
    got_usr1 = 0;
    sigprocmask(SIG_BLOCK, &block_usr1, &oldmask);
    raise(SIGUSR1);
    int sig = sigwaitinfo(&block_usr1, 0);
    sigprocmask(SIG_SETMASK, &oldmask, 0);
    check(sig == SIGUSR1, "sigwaitinfo returns the signal number");
    check(got_usr1 == 0, "sigwaitinfo runs no handler");

    // 7. sigtimedwait success: child kills us mid-wait
    got_usr1 = 0;
    if (fork() == 0)
        napper(0.15, SIGUSR1);
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    double t0 = now_s();
    sig = sigwaitinfo(&block_usr1, &si);
    double dt = now_s() - t0;
    check(sig == SIGUSR1, "sigtimedwait returns the signal");
    check(si.si_signo == SIGUSR1, "sigtimedwait fills siginfo");
    check(dt >= 0.10, "sigtimedwait actually waited");
    check(got_usr1 == 0, "sigtimedwait runs no handler");

    // 8. sigtimedwait timeout: -1/EAGAIN after roughly the asked time
    sigset_t none;
    sigemptyset(&none);
    t0 = now_s();
    struct timespec wait200 = { 0, 200000000 };
    sig = sigtimedwait(&none, 0, &wait200);
    dt = now_s() - t0;
    check(sig == -1 && errno == EAGAIN, "sigtimedwait timeout -> EAGAIN");
    check(dt >= 0.15, "timeout waited the asked interval");

    // 9. sigtimedwait zero timeout: instant EAGAIN (poll semantics)
    t0 = now_s();
    struct timespec zero = { 0, 0 };
    sig = sigtimedwait(&none, 0, &zero);
    dt = now_s() - t0;
    check(sig == -1 && errno == EAGAIN, "zero timeout -> instant EAGAIN");
    check(dt < 0.05, "zero timeout did not sleep");

    // 10. a fully blocked signal does not wake pause; an unblocked one does
    sigprocmask(SIG_BLOCK, &block_usr1, &oldmask);
    got_usr1 = 0;
    if (fork() == 0)
        double_napper(0.10, SIGUSR1, SIGUSR2);
    rc = pause();
    check(rc == -1 && errno == EINTR, "pause wakes on unblocked signal");
    // queue state checked BEFORE unblocking: the restore below delivers
    // the still-pending USR1 right away (and that is posix-correct)
    check(got_usr2 == 1 && got_usr1 == 0,
          "blocked signal stayed queued during pause");
    sigprocmask(SIG_SETMASK, &oldmask, 0);

    printf(fails ? "sigwaittest: %d FAILURES\n" : "sigwaittest: ALL OK (%d fails)\n",
           fails);
    return fails ? 1 : 0;
}
