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
    return _sys(59, (long)name, (long)argv, (long)envp);
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

// --- fs ops: cwd, directories, namei (kernel numbers, linux order) ---

#define DT_BLK  6
#define DT_CHR  2
#define DT_FIFO 1
#define DT_LNK  10
#define DT_DIR  4
#define DT_REG  8

struct coonix_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static inline long getdents64(int fd, void *buf, unsigned long len) {
    return _sys(217, fd, (long)buf, (long)len);
}

static inline long chdir(const char *path) {
    return _sys(80, (long)path, 0, 0);
}

static inline long getcwd(char *buf, unsigned long size) {
    return _sys(79, (long)buf, (long)size, 0);
}

static inline long pipe(int *fds) {
    return _sys(22, (long)fds, 0, 0);
}

static inline long dup(int fd) {
    return _sys(32, fd, 0, 0);
}

static inline long dup2(int oldfd, int newfd) {
    return _sys(33, oldfd, newfd, 0);
}

static inline long fcntl(int fd, int cmd, long arg) {
    return _sys(72, fd, cmd, arg);
}

static inline long mkdir(const char *path, unsigned mode) {
    (void)mode;
    return _sys(83, (long)path, 0, 0);
}

static inline long rmdir(const char *path) {
    return _sys(84, (long)path, 0, 0);
}

static inline long unlink(const char *path) {
    return _sys(87, (long)path, 0, 0);
}

static inline long symlink(const char *target, const char *linkpath) {
    return _sys(88, (long)linkpath, (long)target, 0);
}

static inline long rename(const char *oldp, const char *newp) {
    return _sys(82, (long)oldp, (long)newp, 0);
}

static inline long ftruncate(int fd, unsigned long len) {
    return _sys(77, fd, (long)len, 0);
}

static inline long access(const char *path, int mode) {
    return _sys(21, (long)path, mode, 0);
}

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline long lseek(int fd, long off, int whence) {
    return _sys(8, fd, off, whence);
}

#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD 0
#define FD_CLOEXEC 1

// --- scheduler priorities ---

#define PRIO_PROCESS 0

// linux convention: getpriority returns 20 - nice
static inline long nice(long inc) {
    return _sys(154, inc, 0, 0);
}

static inline long getpriority(int which, long who) {
    return _sys(140, which, who, 0);
}

static inline long setpriority(int which, long who, long prio) {
    return _sys(141, which, who, prio);
}

struct timespec_k {
    long tv_sec;
    long tv_nsec;
};

static inline long clock_gettime(int clk, struct timespec_k *ts) {
    return _sys(228, clk, (long)ts, 0);
}


// --- sockets (AF_INET: udp/icmp datagrams + tcp streams) ---

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

// errno flavors the kernel returns for sockets
#define EAGAIN       11
#define EPIPE        32
#define EMSGSIZE     90
#define ECONNRESET   104
#define EISCONN      106
#define ENOTCONN     107
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EALREADY     114
#define EINPROGRESS  115

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;      // network byte order
    unsigned int   sin_addr;      // network byte order
    unsigned char  sin_zero[8];
};

static inline long socket(long domain, long type, long proto) {
    return _sys(41, domain, type, proto);
}

static inline long sendto(int fd, const void *buf, unsigned long len,
                          const struct sockaddr_in *to) {
    register long _d __asm__("r10") = (long)to;
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((long)44), "D"((long)fd), "S"((long)buf),
                       "d"((long)len), "r"(_d)
                     : "memory");
    return ret;
}

static inline long recvfrom(int fd, void *buf, unsigned long len,
                            struct sockaddr_in *from) {
    register long _d __asm__("r10") = (long)from;
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((long)45), "D"((long)fd), "S"((long)buf),
                       "d"((long)len), "r"(_d)
                     : "memory");
    return ret;
}

// --- tcp stream extensions ---

static inline long connect(int fd, const struct sockaddr_in *sa,
                           unsigned long alen) {
    (void)alen;
    return _sys(42, fd, (long)sa, 0);
}

static inline long bind(int fd, const struct sockaddr_in *sa,
                        unsigned long alen) {
    (void)alen;
    return _sys(49, fd, (long)sa, 0);
}

static inline long listen(int fd, int backlog) {
    return _sys(50, fd, backlog, 0);
}

static inline long accept(int fd, struct sockaddr_in *sa,
                          unsigned int *alen) {
    return _sys(43, fd, (long)sa, (long)alen);
}

static inline long send(int fd, const void *buf, unsigned long len) {
    return sendto(fd, buf, len, 0);
}

static inline long recv(int fd, void *buf, unsigned long len) {
    return recvfrom(fd, buf, len, 0);
}

// --- scatter-gather socket i/o (linux layout) ---

#define MSG_TRUNC 0x20

struct iovec {
    void *iov_base;
    unsigned long iov_len;
};

struct msghdr {
    void *msg_name;               // optional sockaddr
    unsigned int msg_namelen;
    struct iovec *msg_iov;
    unsigned long msg_iovlen;
    void *msg_control;
    unsigned long msg_controllen;
    int msg_flags;
};

static inline long sendmsg(int fd, const struct msghdr *m, int flags) {
    (void)flags;
    return _sys(46, fd, (long)m, 0);
}

static inline long recvmsg(int fd, struct msghdr *m, int flags) {
    (void)flags;
    return _sys(47, fd, (long)m, 0);
}

// --- getsockopt: SO_ERROR/SO_TYPE + coonix TCP_INFO subset ---

#define SOL_SOCKET  1
#define SO_TYPE     3
#define SO_ERROR    4
#define SOL_TCP     6
#define TCP_INFO    11

struct tcp_info_k {
    unsigned char state;          // kernel tcp state enum
    unsigned char pad[3];
    unsigned int cwnd;            // congestion window, bytes
    unsigned int ssthresh;
    unsigned short mss;
    unsigned short rto;           // ticks
};

static inline long getsockopt(int fd, int level, int opt, void *val,
                              unsigned int *len) {
    return _sys6(55, fd, level, opt, (long)val, (long)len);
}

struct timespec_k_dup { long tv_sec; long tv_nsec; };

static inline long nanosleep(long sec, long nsec) {
    struct timespec_k_dup ts = { sec, nsec };
    return _sys(35, (long)&ts, 0, 0);
}
