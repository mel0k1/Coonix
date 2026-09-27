// exercises the memory syscalls: brk/sbrk, anon mmap, munmap, mprotect,
// file-backed mmap (private + shared with writeback)
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("%-30s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

int main(void) {
    // --- program break ---
    char *base = sbrk(0);
    if (base == (void *)-1) {
        printf("brk: query failed\n");
        return 1;
    }
    char *h = sbrk(0x2000);
    check("sbrk grow", h != (void *)-1);
    for (int i = 0; i < 0x2000; i++)
        h[i] = (char)(i & 0xff);
    int ok = 1;
    for (int i = 0; i < 0x2000; i++)
        if (h[i] != (char)(i & 0xff))
            ok = 0;
    check("brk pattern", ok);
    if (sbrk(-0x2000) == (void *)-1)
        check("brk shrink", 0);
    else
        check("brk shrink", 1);

    // --- anonymous mmap ---
    char *m = mmap(0, 0x3000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("anon mmap", m != MAP_FAILED);
    ok = 1;
    for (int i = 0; i < 0x3000; i++)
        if (m[i])
            ok = 0;
    check("mmap zeroed", ok);
    for (int i = 0; i < 0x3000; i += 0x123)
        m[i] = 0x5a;
    ok = 1;
    for (int i = 0; i < 0x3000; i += 0x123)
        if (m[i] != 0x5a)
            ok = 0;
    check("mmap write/read", ok);
    check("munmap", munmap(m, 0x3000) == 0);

    // --- mprotect ---
    m = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
             -1, 0);
    check("anon mmap 2", m != MAP_FAILED);
    m[0] = 0x11;
    ok = mprotect(m, 0x1000, PROT_READ) == 0 && m[0] == 0x11;
    check("mprotect ro", ok);
    ok = mprotect(m, 0x1000, PROT_READ | PROT_WRITE) == 0;
    if (ok) {
        m[0] = 0x22;
        ok = (m[0] == 0x22);
    }
    check("mprotect rw", ok);
    munmap(m, 0x1000);

    // --- file-backed, MAP_PRIVATE: reads file, writes stay private ---
    long fd = open("/etc/motd", O_RDONLY, 0);
    check("open /etc/motd", fd >= 0);
    char *fm = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check("private file mmap", fm != MAP_FAILED);
    check("file content read", fm[0] != 0);
    fm[0] = 'X';   // must not touch the file
    munmap(fm, 0x1000);
    close(fd);
    fd = open("/etc/motd", O_RDONLY, 0);
    char buf[8];
    long n = read(fd, buf, 1);
    close(fd);
    check("private write discarded", n == 1 && buf[0] != 'X');

    // --- file-backed, MAP_SHARED: writes reach the file on munmap ---
    fd = open("/shared.txt", O_WRONLY | O_CREAT | O_TRUNC, 0);
    check("create /shared.txt", fd >= 0);
    const char *msg = "original shared content";
    write(fd, msg, strlen(msg));
    close(fd);

    fd = open("/shared.txt", O_RDWR, 0);
    fm = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("shared file mmap", fm != MAP_FAILED);
    check("shared content read", !memcmp(fm, msg, strlen(msg)));
    const char *upd = "shared mmap rewrote this";
    memcpy(fm, upd, strlen(upd));
    // page is dirty in the pte now; munmap triggers writeback
    munmap(fm, 0x1000);
    close(fd);

    fd = open("/shared.txt", O_RDONLY, 0);
    n = read(fd, buf, 4);
    close(fd);
    buf[4] = 0;
    check("shared writeback", n == 4 && !memcmp(buf, upd, 4));

    if (fail) {
        printf("mtest: FAILED\n");
        return 1;
    }
    printf("mtest: all ok\n");
    return 0;
}
