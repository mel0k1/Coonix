// iotest: terminal ioctls + line discipline through real glibc.
// canonical reads give whole lines with echo from the kernel; raw mode
// gives per-character reads. isatty/tcgetattr/winsize covered too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void) {
    struct termios t;
    struct winsize ws;

    // 1. isatty via TCGETS on console fds, ENOTTY on files
    if (!isatty(0) || !isatty(1)) {
        printf("iotest: console is not a tty\n");
        return 1;
    }

    // 2. tcgetattr roundtrip
    if (tcgetattr(0, &t) < 0) {
        printf("iotest: tcgetattr failed\n");
        return 1;
    }
    if (!(t.c_lflag & ICANON) || !(t.c_lflag & ECHO)) {
        printf("iotest: expected canonical+echo defaults, lflag=%#lx\n",
               (unsigned long)t.c_lflag);
        return 1;
    }

    // 3. winsize
    if (ioctl(0, TIOCGWINSZ, &ws) < 0 || ws.ws_row != 25 || ws.ws_col != 80) {
        printf("iotest: winsize %dx%d\n", ws.ws_row, ws.ws_col);
        return 1;
    }

    // 4. canonical read: a whole typed line arrives in one read()
    char line[64];
    ssize_t n = read(0, line, sizeof(line) - 1);
    if (n < 2) {
        printf("iotest: canonical read got %zd bytes\n", n);
        return 1;
    }
    line[n] = 0;
    if (line[n - 1] != '\n') {
        printf("iotest: canonical read missing newline\n");
        return 1;
    }
    printf("iotest: canonical line (%zd bytes): %.*s", n, (int)n, line);

    // 5. raw mode: per-character read without enter
    struct termios raw = t;
    cfmakeraw(&raw);
    if (tcsetattr(0, TCSANOW, &raw) < 0) {
        printf("iotest: tcsetattr raw failed\n");
        return 1;
    }
    char c;
    printf("iotest: raw now, type one key\n");
    n = read(0, &c, 1);
    if (n != 1) {
        printf("iotest: raw read failed\n");
        return 1;
    }
    printf("iotest: raw key %c (%#x)\n", c == '\r' ? '.' : c, c);

    // 6. restore canonical
    if (tcsetattr(0, TCSANOW, &t) < 0) {
        printf("iotest: restore failed\n");
        return 1;
    }
    printf("iotest: OK\n");
    return 0;
}
