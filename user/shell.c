// user programs: shell, hello, forktest
// see also libc/ for the mini user library
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static void cat(const char *path) {
    long fd = open(path, 0, 0);
    if (fd < 0) {
        printf("cat: no such file: %s\n", path);
        return;
    }
    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
}

static void ls(const char *path) {
    long fd = open(path, 0, 0);
    if (fd < 0) {
        printf("ls: no such dir: %s\n", path);
        return;
    }
    char buf[1024];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        printf("ls: read failed\n");
        return;
    }
    buf[n] = 0;
    // listing is name-per-line; print space separated
    for (long i = 0; i < n; i++)
        if (buf[i] == '\n')
            buf[i] = ' ';
    printf("%s\n", buf);
}

int main(void) {
    char buf[128];
    int len;

    // message of the day from the ramdisk
    cat("/etc/motd");
    printf("commands: help, echo, clear, ls [dir], cat <file>, pid, exit\n");

    for (;;) {
        printf("coonix> ");
        len = 0;
        while (len < (int)sizeof(buf) - 1) {
            char c;
            if (read(0, &c, 1) != 1)
                continue;
            if (c == '\n')
                break;
            if (c == '\b') {
                if (len) {
                    len--;
                    write(1, "\b \b", 3);
                }
                continue;
            }
            buf[len++] = c;
            write(1, &c, 1);
        }
        buf[len] = 0;
        write(1, "\n", 1);
        if (!len)
            continue;

        if (!strcmp(buf, "help")) {
            printf("commands: help, echo <txt>, clear, ls [dir], cat <file>,\n");
            printf("          <prog> (runs /bin/<prog>), pid, exit\n");
        } else if (!strncmp(buf, "echo", 4)) {
            printf("%s\n", buf[4] ? buf + 5 : "");
        } else if (!strcmp(buf, "clear")) {
            for (int i = 0; i < 60; i++)
                write(1, "\n", 1);
        } else if (!strcmp(buf, "pid")) {
            printf("pid: %d\n", (int)getpid());
        } else if (!strncmp(buf, "ls", 2)) {
            ls(buf[2] ? buf + 3 : "/");
        } else if (!strncmp(buf, "cat", 3)) {
            if (buf[3])
                cat(buf + 4);
            else
                printf("usage: cat <file>\n");
        } else if (!strcmp(buf, "exit")) {
            printf("bye\n");
            return 0;
        } else {
            // try to run it from /bin via fork+exec
            long pid = fork();
            if (pid == 0) {
                if (execve(buf, 0, 0) < 0) {
                    printf("unknown: %s (try help)\n", buf);
                    exit(1);
                }
            } else {
                int st;
                wait(&st);
            }
        }
    }
}
