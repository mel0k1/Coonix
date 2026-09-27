// syscall wrappers: same numbers as the kernel expects
#pragma once
#include <stdint.h>

static inline long _sys(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return ret;
}

static inline long write(int fd, const void *buf, unsigned long len) {
    return _sys(1, fd, (long)buf, (long)len);
}

static inline long read(int fd, void *buf, unsigned long len) {
    return _sys(0, fd, (long)buf, (long)len);
}

static inline long open(const char *path, int flags, int mode) {
    (void)flags; (void)mode;
    return _sys(2, (long)path, flags, mode);
}

static inline long close(int fd) {
    return _sys(3, fd, 0, 0);
}

static inline long getpid(void) {
    return _sys(39, 0, 0, 0);
}

static inline long fork(void) {
    return _sys(57, 0, 0, 0);
}

static inline long execve(const char *name, char *const argv[], char *const envp[]) {
    (void)argv; (void)envp;
    return _sys(59, (long)name, 0, 0);
}

static inline void exit(int code) {
    _sys(60, code, 0, 0);
    for (;;);
}

static inline long wait(int *status) {
    return _sys(61, 0, (long)status, 0);
}

// 5-arg syscall: kernel reads args 4/5 from r10/r8 like linux
static inline long _sys6(long n, long a, long b, long c, long d, long e) {
    long ret;
    register long _d __asm__("r10") = d;
    register long _e __asm__("r8") = e;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(_d), "r"(_e)
                     : "memory");
    return ret;
}

// --- memory: brk/sbrk + anonymous mmap ---

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

static inline long brk(void *addr) {
    return _sys(12, (long)addr, 0, 0);
}

static inline void *sbrk(long delta) {
    long cur = _sys(12, 0, 0, 0);
    if (cur < 0)
        return (void *)-1;
    if (!delta)
        return (void *)cur;
    long nw = _sys(12, cur + delta, 0, 0);
    return nw == cur + delta ? (void *)cur : (void *)-1;
}

static inline void *mmap(void *addr, unsigned long len, int prot, int flags) {
    long ret = _sys6(9, (long)addr, (long)len, prot, flags, -1);
    return ret < 0 ? MAP_FAILED : (void *)ret;
}

static inline long munmap(void *addr, unsigned long len) {
    return _sys(11, (long)addr, (long)len, 0);
}

static inline long mprotect(void *addr, unsigned long len, int prot) {
    return _sys(10, (long)addr, (long)len, prot);
}
