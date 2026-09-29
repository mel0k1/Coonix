// wxtest: W^X enforcement in userspace.
//  - RW anon pages must NOT execute (kernel maps them NX)
//  - mprotect RX promotes, mprotect RW demotes
//  - the binary's own .text must NOT be writable
//  - the binary's own .bss must NOT be executable
// every fault is caught with a SIGSEGV handler + siglongjmp.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>

static sigjmp_buf jb;
static volatile int expect_fault = 0;
static volatile int faults = 0;
static volatile int surprises = 0;

static void segv_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    (void)si;
    (void)uc;
    if (expect_fault) {
        expect_fault = 0;
        faults++;
        siglongjmp(jb, 1);
    }
    surprises++;
    _exit(7);   // an unplanned fault: bail out loudly
}

// run fn(); return 1 if it faulted, 0 if it came back clean
static int attempt(void (*fn)(void)) {
    if (sigsetjmp(jb, 1) != 0)
        return 1;             // came here from the handler
    expect_fault = 1;
    fn();
    expect_fault = 0;
    return 0;
}

static void text_write(void) {
    *(volatile char *)&text_write = 0x90;   // store into own .text
}

static void data_exec(void) {
    char gbuf[64];
    memset(gbuf, 0xC3, sizeof(gbuf));   // ret sled: would return if executable
    void (*fp)(void) = (void (*)(void))(void *)gbuf;
    fp();
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, 0) < 0) {
        printf("wxtest: sigaction failed\n");
        return 1;
    }

    // 1. fresh RW anon page filled with `ret`: calling it must fault (NX)
    char *pg = mmap(0, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pg == (char *)-1) {
        printf("wxtest: mmap failed\n");
        return 1;
    }
    memset(pg, 0xC3, 4096);
    if (attempt((void (*)(void))(void *)pg) != 1) {
        printf("wxtest: FAIL rw anon page is executable\n");
        return 1;
    }
    printf("wxtest: ok 1 rw anon page is NX\n");

    // 2. promote to RX: the ret sled now runs and returns cleanly
    if (mprotect(pg, 4096, PROT_READ | PROT_EXEC) != 0) {
        printf("wxtest: mprotect rx failed\n");
        return 1;
    }
    if (attempt((void (*)(void))(void *)pg) != 0) {
        printf("wxtest: FAIL rx page did not execute\n");
        return 1;
    }
    printf("wxtest: ok 2 mprotect rx allows exec\n");

    // 3. demote back to RW: exec must fault again
    if (mprotect(pg, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("wxtest: mprotect rw failed\n");
        return 1;
    }
    if (attempt((void (*)(void))(void *)pg) != 1) {
        printf("wxtest: FAIL demoted page still executes\n");
        return 1;
    }
    printf("wxtest: ok 3 mprotect rw drops exec\n");
    munmap(pg, 4096);

    // 4. own .text is X but not W: a store must fault
    if (attempt(text_write) != 1) {
        printf("wxtest: FAIL .text is writable\n");
        return 1;
    }
    printf("wxtest: ok 4 .text is not writable\n");

    // 5. own stack buffer is W but not X: a call must fault
    if (attempt(data_exec) != 1) {
        printf("wxtest: FAIL stack page is executable\n");
        return 1;
    }
    printf("wxtest: ok 5 stack is not executable\n");

    if (surprises) {
        printf("wxtest: FAIL %d unplanned faults\n", surprises);
        return 1;
    }
    printf("wxtest: ALL OK (%d faults caught)\n", faults);
    return 0;
}
