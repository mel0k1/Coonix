// user programs: shell, hello
// see also libc/ for the mini user library
#include "stdio.h"
#include "string.h"
#include "coonix.h"

int main(void) {
    char buf[128];
    int len;

    printf("Coonix shell. commands: help, echo, clear, hello, pid, exit\n");

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
            printf("commands: help, echo <txt>, clear, hello, pid, exit\n");
        } else if (!strncmp(buf, "echo", 4)) {
            printf("%s\n", buf[4] ? buf + 5 : "");
        } else if (!strcmp(buf, "clear")) {
            for (int i = 0; i < 60; i++)
                write(1, "\n", 1);
        } else if (!strcmp(buf, "pid")) {
            printf("pid: %d\n", (int)getpid());
        } else if (!strcmp(buf, "hello")) {
            long pid = fork();
            if (pid == 0) {
                if (execve("hello", 0, 0) < 0)
                    printf("exec failed\n");
                exit(1);
            } else {
                int st;
                wait(&st);
                printf("[hello exited with %d]\n", st);
            }
        } else if (!strcmp(buf, "exit")) {
            printf("bye\n");
            return 0;
        } else {
            printf("unknown: %s (try help)\n", buf);
        }
    }
}
