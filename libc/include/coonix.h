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

// 6-arg syscall: kernel reads args 4/5/6 from r10/r8/r9 like linux
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

static inline long _sys7(long n, long a, long b, long c, long d, long e, long f) {
    long ret;
    register long _d __asm__("r10") = d;
    register long _e __asm__("r8") = e;
    register long _f __asm__("r9") = f;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(_d), "r"(_e), "r"(_f)
                     : "memory");
    return ret;
}

// --- memory: brk/sbrk + anonymous mmap ---

#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_SHARED    0x01
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

// linux-style mmap: fd ignored for anonymous, off must be page aligned
static inline void *mmap(void *addr, unsigned long len, int prot, int flags,
                         int fd, unsigned long off) {
    long ret = _sys7(9, (long)addr, (long)len, prot, flags, fd, (long)off);
    return ret < 0 ? MAP_FAILED : (void *)ret;
}

static inline long munmap(void *addr, unsigned long len) {
    return _sys(11, (long)addr, (long)len, 0);
}

static inline long mprotect(void *addr, unsigned long len, int prot) {
    return _sys(10, (long)addr, (long)len, prot);
}

// --- threads / signals / tty: matching the kernel's linux-ish abi ---

#define SIGINT   2
#define SIGILL   4
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGTERM 15

#define SA_RESTORER 0x04000000UL

// what the kernel's rt_sigaction expects (kernel_sigaction)
struct ksigaction {
    void *handler;
    unsigned long flags;
    void *restorer;
    unsigned long mask;
};

// rt_sigreturn trampoline: the kernel restores the interrupted frame
// from the sigframe our rsp points into; no stack traffic allowed here
__attribute__((naked)) static long _sigreturn_thunk(void) {
    __asm__ volatile("mov $15, %eax\n\tint $0x80");
}

// simplified sigaction: glibc-style wrapper that fills in the restorer
static inline long sigaction(int sig, void *handler, unsigned long flags) {
    struct ksigaction sa;
    sa.handler = handler;
    sa.flags = flags | SA_RESTORER;
    sa.restorer = (void *)_sigreturn_thunk;
    sa.mask = 0;
    return _sys7(13, sig, (long)&sa, 0, 8, 0, 0);
}

static inline long kill(long pid, int sig) {
    return _sys(62, pid, sig, 0);
}

static inline long gettid(void) {
    return _sys(186, 0, 0, 0);
}

// --- ioctl: terminal ---

#define TCGETS      0x5401UL
#define TCSETS      0x5402UL
#define TIOCGWINSZ  0x5413UL

struct coonix_termios {
    unsigned int iflag, oflag, cflag, lflag;
    unsigned char line;
    unsigned char cc[19];
} __attribute__((packed));

static inline long ioctl(int fd, unsigned long req, void *arg) {
    return _sys(16, fd, (long)req, (long)arg);
}
