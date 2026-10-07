// dpagetest: anonymous mmap is demand-paged
// 1. a freshly mapped region costs no physical memory
// 2. touching each page faults in exactly one zero frame per page
// 3. the pages read back with the written pattern
// 4. munmap hands the memory back to the pmm
#include "stdio.h"
#include "string.h"
#include "coonix.h"

// matches the kernel's glibc-style sysinfo (112 bytes)
struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
};

static long freeram(void) {
    static struct sysinfo si;
    long ret = _sys(99, (long)&si, 0, 0);
    if (ret < 0)
        return -1;
    return (long)si.freeram;
}

static int fail;

static void check(const char *what, int ok) {
    printf("[dpagetest] %-26s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

#define MB (1024UL * 1024)
#define SIZE (4UL * MB)

int main(void) {
    long base = freeram();

    char *m = mmap(0, SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap 4M", m != MAP_FAILED);

    // untouched: the pmm must not have paid for the region
    long after_map = freeram();
    long drop = base - after_map;
    check("untouched costs nothing", drop < (long)MB);
    printf("[dpagetest] map drop %d kb\n", (int)(drop >> 10));

    // touch one byte per page: 1024 faults for 4 MB
    for (unsigned long i = 0; i < SIZE; i += 4096)
        m[i] = (char)(i >> 12);
    long after_touch = freeram();
    long used = after_map - after_touch;
    check("touch allocates 4M", used > 3 * (long)MB && used < 6 * (long)MB);
    printf("[dpagetest] touch used %d kb\n", (int)(used >> 10));

    // pattern survives
    int ok = 1;
    for (unsigned long i = 0; i < SIZE; i += 4096)
        if (m[i] != (char)(i >> 12))
            ok = 0;
    check("pattern readback", ok);

    // untouched pages inside the region stay zero
    ok = 1;
    for (unsigned long i = 0; i < SIZE; i += 4096)
        if (m[i + 100])
            ok = 0;
    check("pages start zeroed", ok);

    // release: the pmm gets its frames back
    check("munmap", munmap(m, SIZE) == 0);
    long after_unmap = freeram();
    long got_back = after_unmap - after_touch;
    check("munmap frees 4M", got_back > 3 * (long)MB && got_back < 6 * (long)MB);
    printf("[dpagetest] munmap got back %d kb\n", (int)(got_back >> 10));

    if (!fail) {
        printf("[dpagetest] PASS\n");
        return 0;
    }
    printf("[dpagetest] FAIL\n");
    return 1;
}
