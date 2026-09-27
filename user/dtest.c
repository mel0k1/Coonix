// dtest: replay ld.so's exact syscall sequence on a real lib file
#include <coonix.h>

#define O_CLOEXEC 0x80000
#define AT_FDCWD  (-100)

struct stat_k {
    unsigned long dev, ino, nlink;
    unsigned int mode, uid, gid, pad0;
    unsigned long rdev, size, blksize, blocks;
    long atim[2], mtim[2], ctim[2];
    long reserved[3];
};

static void hex(unsigned long v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (v >> ((15 - i) * 4)) & 0xf;
        buf[2 + i] = d < 10 ? '0' + d : 'a' + d - 10;
    }
    buf[18] = '\n';
    write(1, buf, 19);
}

static void msg(const char *s) {
    long n = 0;
    while (s[n])
        n++;
    write(1, s, n);
}

int main(void) {
    // 1. openat like ld.so: AT_FDCWD, O_RDONLY|O_CLOEXEC
    long fd = _sys6(257, AT_FDCWD, (long)"/lib/x86_64-linux-gnu/libc.so.6",
                    O_RDONLY | O_CLOEXEC, 0, 0);
    msg("openat fd="); hex((unsigned long)fd);
    if (fd < 0) {
        // try plain open as fallback diagnostic
        fd = open("/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY, 0);
        msg("plain open fd="); hex((unsigned long)fd);
        if (fd < 0)
            exit(1);
    }

    // 2. fstat
    struct stat_k st;
    long r = _sys(5, fd, (long)&st, 0);
    msg("fstat ret="); hex((unsigned long)r);
    msg("st_size="); hex(st.size);
    msg("st_mode="); hex(st.mode);

    // 3. pread64 header (what ld.so does before mmap)
    char hdr[64];
    r = _sys6(17, fd, (long)hdr, 64, 0, 0);
    msg("pread64 ret="); hex((unsigned long)r);
    msg("magic="); hex(*(unsigned long *)hdr & 0xffffffffUL);

    // 4. mmap PROT_NONE anon reservation (like _dl_map_segments)
    unsigned long len = 0x1d2000;
    unsigned long res = (unsigned long)mmap(0, len, 0,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    msg("reserve="); hex(res);
    if (res == (unsigned long)-1)
        exit(1);

    // 5. MAP_FIXED file mapping over the reservation start (PT_LOAD style)
    unsigned long m = (unsigned long)mmap((void *)res, 0x20000,
                                          PROT_READ | PROT_EXEC,
                                          MAP_PRIVATE | MAP_FIXED, (int)fd, 0);
    msg("fixed map="); hex(m);

    // 6. read a byte from the mapping (should be 0x7f ELF magic)
    if (m != (unsigned long)-1)
        msg("first byte: "), hex(*(unsigned char *)m);

    // 7. MAP_FIXED anon over a middle chunk (bss-style zero fill)
    unsigned long b = (unsigned long)mmap((void *)(res + 0x40000), 0x10000,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
                                          -1, 0);
    msg("fixed anon="); hex(b);

    // 8. mprotect subrange back to read-only
    r = mprotect((void *)res, 0x1000, PROT_READ);
    msg("mprotect="); hex((unsigned long)r);

    msg("dtest done\n");
    return 0;
}
