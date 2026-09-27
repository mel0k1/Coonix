// exercises the memory syscalls: brk/sbrk, anon mmap, munmap
#include "stdio.h"
#include "coonix.h"

int main(void) {
    // --- program break ---
    char *base = sbrk(0);
    if (base == (void *)-1) {
        printf("brk: query failed\n");
        return 1;
    }
    char *h = sbrk(0x2000);
    if (h == (void *)-1) {
        printf("sbrk: grow failed\n");
        return 1;
    }
    for (int i = 0; i < 0x2000; i++)
        h[i] = (char)(i & 0xff);
    int ok = 1;
    for (int i = 0; i < 0x2000; i++)
        if (h[i] != (char)(i & 0xff))
            ok = 0;
    printf("brk grow + pattern: %s\n", ok ? "ok" : "corrupt");

    // shrink back
    if (sbrk(-0x2000) == (void *)-1)
        printf("brk shrink: fail\n");
    else
        printf("brk shrink: ok\n");

    // --- anonymous mmap ---
    char *m = mmap(0, 0x3000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS);
    if (m == MAP_FAILED) {
        printf("mmap: failed\n");
        return 1;
    }
    printf("mmap at 0x%x_%x\n",
           (unsigned)((unsigned long)m >> 32),
           (unsigned)((unsigned long)m & 0xffffffff));

    ok = 1;
    for (int i = 0; i < 0x3000; i++)
        if (m[i])
            ok = 0;
    printf("mmap zeroed: %s\n", ok ? "ok" : "no");

    for (int i = 0; i < 0x3000; i += 0x123)
        m[i] = 0x5a;
    ok = 1;
    for (int i = 0; i < 0x3000; i += 0x123)
        if (m[i] != 0x5a)
            ok = 0;
    printf("mmap write/read: %s\n", ok ? "ok" : "corrupt");

    if (munmap(m, 0x3000) == 0)
        printf("munmap: ok\n");
    else
        printf("munmap: fail\n");

    // --- mprotect: downgrade then upgrade back ---
    m = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (m == MAP_FAILED) {
        printf("mmap 2: failed\n");
        return 1;
    }
    m[0] = 0x11;
    if (mprotect(m, 0x1000, PROT_READ) == 0 && m[0] == 0x11)
        printf("mprotect ro: ok\n");
    else
        printf("mprotect ro: fail\n");
    if (mprotect(m, 0x1000, PROT_READ | PROT_WRITE) == 0) {
        m[0] = 0x22;
        printf("mprotect rw: ok\n");
    } else {
        printf("mprotect rw: fail\n");
    }
    munmap(m, 0x1000);
    return 0;
}
