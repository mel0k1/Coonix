// user programs: shell, hello, forktest
// see also libc/ for the mini user library
#include "stdio.h"
#include "string.h"
#include "coonix.h"

// ctrl-C lands here while the shell sits in read(); print a fresh prompt
// and let the sigreturn replay the read
static void on_sigint(int sig) {
    (void)sig;
    printf("\ncoonix> ");
}

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

    sigaction(SIGINT, on_sigint, 0);

    // message of the day from the ramdisk
    cat("/etc/motd");
    printf("commands: help, echo, clear, ls [dir], cat <file>, pid, exit\n");

    for (;;) {
        printf("coonix> ");
        // canonical mode: the kernel echoes and hands us the whole line
        len = read(0, buf, (int)sizeof(buf) - 1);
        if (len <= 0)
            continue;                  // eof (ctrl-D) or restart after signal
        if (buf[len - 1] == '\n')
            len--;
        buf[len] = 0;
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
            // try to run it from /bin via fork+exec; quote-aware tokenizer
            // (strips ' and " spans) so argv reaches the child intact —
            // busybox dispatches on argv[0], sh -c needs one fat argument
            char *argv[16];
            int argc = 0;
            char *p = buf;      // read cursor
            char *w = buf;      // write cursor (quotes stripped in place)
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                if (argc < 15)
                    argv[argc++] = w;
                while (*p && *p != ' ') {
                    if (*p == '\'' || *p == '"') {
                        char q = *p++;
                        while (*p && *p != q)
                            *w++ = *p++;
                        if (*p == q)
                            p++;
                    } else {
                        *w++ = *p++;
                    }
                }
                if (*p)
                    p++;        // the space
                *w++ = 0;
            }
            if (!argc)
                continue;
            argv[argc] = 0;
            long pid = fork();
            if (pid == 0) {
                if (execve(argv[0], argv, 0) < 0) {
                    printf("unknown: %s (try help)\n", argv[0]);
                    exit(1);
                }
            } else {
                int st;
                wait(&st);
            }
        }
    }
}
