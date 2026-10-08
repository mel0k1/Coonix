// msgtest: dup + sendmsg/recvmsg scatter-gather over udp loopback
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("[msgtest] %-22s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

static void msleep(long ms) {
    nanosleep(0, ms * 1000000);
}

static void mkaddr(struct sockaddr_in *sa, unsigned port) {
    unsigned be = __builtin_bswap32(0x7f000001);
    __builtin_memcpy(&sa->sin_addr, &be, 4);
    unsigned short pbe = (unsigned short)((port >> 8) | (port << 8));
    __builtin_memcpy(&sa->sin_port, &pbe, 2);
    sa->sin_family = AF_INET;
    __builtin_memset(sa->sin_zero, 0, sizeof(sa->sin_zero));
}

#define PORT 5312

// child: udp echo server that uppercases, so scatter checks bind
static void echo_child(void) {
    long s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        exit(1);
    struct sockaddr_in a;
    mkaddr(&a, PORT);
    if (bind((int)s, &a, sizeof(a)) < 0)
        exit(1);
    for (int i = 0; i < 3; i++) {
        char b[256];
        struct sockaddr_in from;
        struct iovec iov = { b, sizeof(b) };
        struct msghdr m;
        __builtin_memset(&m, 0, sizeof(m));
        m.msg_name = &from;
        m.msg_namelen = sizeof(from);
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        long n;
        do {                     // non-blocking: poll until one lands
            n = recvmsg((int)s, &m, 0);
            if (n <= 0)
                nanosleep(0, 5 * 1000000);
        } while (n <= 0);
        for (long j = 0; j < n; j++)
            if (b[j] >= 'a' && b[j] <= 'z')
                b[j] = (char)(b[j] - 'a' + 'A');
        struct sockaddr_in to;
        mkaddr(&to, PORT);
        to.sin_port = from.sin_port;
        struct iovec o = { b, (unsigned long)n };
        struct msghdr om;
        __builtin_memset(&om, 0, sizeof(om));
        om.msg_name = &to;
        om.msg_namelen = sizeof(to);
        om.msg_iov = &o;
        om.msg_iovlen = 1;
        sendmsg((int)s, &om, 0);
    }
    exit(0);
}

int main(void) {
    long pid = fork();
    if (pid == 0)
        echo_child();
    msleep(300);                     // let the child bind

    long s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check("socket", s >= 0);

    // 1. sendmsg gathers two iovecs into one datagram
    char p0[6] = "hello ", p1[6] = "world";
    struct iovec sv[2] = { {p0, 6}, {p1, 5} };
    struct sockaddr_in dst;
    mkaddr(&dst, PORT);
    struct msghdr sm;
    __builtin_memset(&sm, 0, sizeof(sm));
    sm.msg_name = &dst;
    sm.msg_namelen = sizeof(dst);
    sm.msg_iov = sv;
    sm.msg_iovlen = 2;
    check("sendmsg 2 iovs", sendmsg((int)s, &sm, 0) == 11);

    // 2. recvmsg scatters the reply across two iovecs
    char r0[8], r1[8];
    struct iovec rv[2] = { {r0, 6}, {r1, 5} };
    struct sockaddr_in from;
    struct msghdr rm;
    __builtin_memset(&rm, 0, sizeof(rm));
    rm.msg_name = &from;
    rm.msg_namelen = sizeof(from);
    rm.msg_iov = rv;
    rm.msg_iovlen = 2;
    long n = -1;
    for (int t = 0; t < 50 && n <= 0; t++) {
        n = recvmsg((int)s, &rm, 0);
        if (n <= 0)
            msleep(20);
    }
    check("recvmsg scatter", n == 11);
    check("iov0 bound", n > 0 && !memcmp(r0, "HELLO ", 6));
    check("iov1 bound", n > 0 && !memcmp(r1, "WORLD", 5));
    check("msg_name src", from.sin_family == AF_INET &&
          __builtin_bswap32(from.sin_addr) == 0x7f000001);

    // 3. dup: the same socket through another fd
    long d = dup((int)s);
    check("dup", d >= 0);
    check("sendmsg via dup", d >= 0 && sendmsg((int)d, &sm, 0) == 11);

    // 4. dup2 onto another fd: send and receive through it
    check("dup2", dup2((int)s, 13) == 13);
    check("sendmsg via dup2", sendmsg(13, &sm, 0) == 11);
    n = -1;
    for (int t = 0; t < 50 && n <= 0; t++) {
        n = recvmsg(13, &rm, 0);
        if (n <= 0)
            msleep(20);
    }
    check("recvmsg via dup2", n == 11 && !memcmp(r0, "HELLO ", 6));

    int st = -1;
    wait(&st);
    check("child exit", st == 0);
    puts(fail ? "[msgtest] FAIL\n" : "[msgtest] all ok\n");
    return fail;
}
