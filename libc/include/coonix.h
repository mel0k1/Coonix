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
