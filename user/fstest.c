// exercises ext2 write support: create, write, read back, truncate,
// multi-block file with indirect blocks
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("%-28s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

int main(void) {
    const char *msg = "coonix ext2 write support works!\n";

    // create + write + read back
    long fd = open("/test.txt", O_WRONLY | O_CREAT, 0);
    check("create /test.txt", fd >= 0);
    if (fd < 0)
        return 1;
    check("write msg", write(fd, msg, strlen(msg)) == (long)strlen(msg));
    close(fd);

    fd = open("/test.txt", O_RDONLY, 0);
    char buf[64];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0)
        buf[n] = 0;
    check("read back", n == (long)strlen(msg) && !strcmp(buf, msg));

    // truncate + rewrite
    fd = open("/test.txt", O_WRONLY | O_TRUNC, 0);
    check("open O_TRUNC", fd >= 0);
    const char *msg2 = "truncated, rewritten";
    write(fd, msg2, strlen(msg2));
    close(fd);

    fd = open("/test.txt", O_RDONLY, 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0)
        buf[n] = 0;
    check("after truncate", n == (long)strlen(msg2) && !strcmp(buf, msg2));

    // multi-block: 40 KiB pattern -> 40 data blocks, 1 indirect table
    #define BIG (40 * 1024)
    static char big[BIG];
    for (int i = 0; i < BIG; i++)
        big[i] = (char)(i * 7 + 3);
    fd = open("/big.bin", O_WRONLY | O_CREAT | O_TRUNC, 0);
    check("create /big.bin", fd >= 0);
    long w = write(fd, big, BIG);
    close(fd);
    check("write 40k", w == BIG);

    static char rb[BIG];
    fd = open("/big.bin", O_RDONLY, 0);
    long r = read(fd, rb, BIG);
    close(fd);
    check("read 40k size", r == BIG);
    check("read 40k data", !memcmp(big, rb, BIG));

    // ls the root: our new files must be listed
    fd = open("/", O_RDONLY, 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    int found_t = 0, found_b = 0;
    for (long i = 0; i < n; i++) {
        if (i + 9 <= n && !memcmp(buf + i, "\ntest.txt", 9))
            found_t = 1;
        if (i + 8 <= n && !memcmp(buf + i, "\nbig.bin", 8))
            found_b = 1;
    }
    check("ls sees test.txt", found_t);
    check("ls sees big.bin", found_b);

    if (fail) {
        printf("fstest: FAILED\n");
        return 1;
    }
    printf("fstest: all ok\n");
    return 0;
}
