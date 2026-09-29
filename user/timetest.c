// timetest: cpu accounting through the real glibc.
//  - getrusage(RUSAGE_SELF): utime grows after a user busy loop
//  - fork + child burn + wait: getrusage(RUSAGE_CHILDREN) reports the child
//  - times(): tms_cutime agrees, uptime ticks come back as the return value
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>

static volatile unsigned long sink;

static void burn(void) {
    for (volatile unsigned long i = 0; i < 150000000UL; i++)
        sink += i;
}

static long usec_of(struct timeval *tv) {
    return (long)tv->tv_sec * 1000000L + tv->tv_usec;
}

int main(void) {
    struct rusage ru0, ru1, rc;
    memset(&ru0, 0, sizeof(ru0));
    memset(&ru1, 0, sizeof(ru1));
    memset(&rc, 0, sizeof(rc));

    // 1. self user time moves after a busy loop
    if (getrusage(RUSAGE_SELF, &ru0) != 0) {
        printf("timetest: getrusage self failed\n");
        return 1;
    }
    burn();
    if (getrusage(RUSAGE_SELF, &ru1) != 0) {
        printf("timetest: getrusage self 2 failed\n");
        return 1;
    }
    long du = usec_of(&ru1.ru_utime) - usec_of(&ru0.ru_utime);
    if (du <= 0) {
        printf("timetest: FAIL self utime did not move (%ld us)\n", du);
        return 1;
    }
    printf("timetest: ok 1 self utime %ld.%06ld s\n",
           (long)ru1.ru_utime.tv_sec, (long)ru1.ru_utime.tv_usec);

    // 2. a burned child lands in RUSAGE_CHILDREN
    pid_t pid = fork();
    if (pid < 0) {
        printf("timetest: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        burn();
        _exit(0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        printf("timetest: waitpid failed\n");
        return 1;
    }
    if (getrusage(RUSAGE_CHILDREN, &rc) != 0) {
        printf("timetest: getrusage children failed\n");
        return 1;
    }
    if (usec_of(&rc.ru_utime) <= 0) {
        printf("timetest: FAIL children utime is zero\n");
        return 1;
    }
    printf("timetest: ok 2 children utime %ld.%06ld s\n",
           (long)rc.ru_utime.tv_sec, (long)rc.ru_utime.tv_usec);

    // 3. times() sees the same totals + uptime in the return value
    struct tms tm;
    memset(&tm, 0, sizeof(tm));
    clock_t now = times(&tm);
    if (now <= 0) {
        printf("timetest: FAIL times() uptime is %ld\n", (long)now);
        return 1;
    }
    if ((long)tm.tms_cutime <= 0) {
        printf("timetest: FAIL tms_cutime is %ld\n", (long)tm.tms_cutime);
        return 1;
    }
    if ((long)tm.tms_utime <= 0) {
        printf("timetest: FAIL tms_utime is %ld\n", (long)tm.tms_utime);
        return 1;
    }
    printf("timetest: ok 3 times(): up %ld ticks, self %ld, child %ld\n",
           (long)now, (long)tm.tms_utime, (long)tm.tms_cutime);

    printf("timetest: ALL OK\n");
    return 0;
}
