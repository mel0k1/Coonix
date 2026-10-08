// ip6test: ipv6 over loopback — icmpv6 echo, udp6 echo, tcp6 stream
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("[ip6test] %-24s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

static void msleep(long ms) {
    nanosleep(0, ms * 1000000);
}

static const unsigned char lo1[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 1};
static const unsigned char any6[16] = {0};

static void mkaddr6(struct sockaddr_in6 *sa, const unsigned char *ip6,
                    unsigned port) {
    __builtin_memset(sa, 0, sizeof(*sa));
    sa->sin6_family = AF_INET6;
    unsigned short pbe = (unsigned short)((port >> 8) | (port << 8));
    __builtin_memcpy(&sa->sin6_port, &pbe, 2);
    __builtin_memcpy(sa->sin6_addr, ip6, 16);
}

// --- 1. icmpv6 echo over ::1 ---

static void ping6_loopback(void) {
    long s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    check("ping6 socket", s >= 0);
    const char *msg = "ipv6-ping-payload";
    struct sockaddr_in6 dst;
    mkaddr6(&dst, lo1, 0);
    long n = -1;
    for (int t = 0; t < 50 && n <= 0; t++) {
        n = sendto6((int)s, msg, 18, &dst);
        if (n <= 0)
            msleep(20);
    }
    check("ping6 send", n == 18);
    char rbuf[64];
    struct sockaddr_in6 from;
    long m = -1;
    for (int t = 0; t < 100 && m <= 0; t++) {
        m = recvfrom6((int)s, rbuf, sizeof(rbuf), &from);
        if (m <= 0)
            msleep(20);
    }
    check("ping6 reply", m == 18 && !memcmp(rbuf, msg, 18));
    check("ping6 src ::1", from.sin6_family == AF_INET6 &&
          !memcmp(from.sin6_addr, lo1, 16));
    close((int)s);
}

// --- 2. udp6 echo over ::1 ---

#define UPORT 5322

static void udp6_child(void) {
    long s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        exit(1);
    struct sockaddr_in6 a;
    mkaddr6(&a, any6, UPORT);
    if (bind6((int)s, &a, sizeof(a)) < 0)
        exit(1);
    for (int i = 0; i < 1; i++) {   // parent sends exactly one
        char b[300];
        struct sockaddr_in6 from;
        long n;
        do {                     // non-blocking: poll until one lands
            n = recvfrom6((int)s, b, sizeof(b), &from);
            if (n <= 0)
                nanosleep(0, 5 * 1000000);
        } while (n <= 0);
        sendto6((int)s, b, (unsigned long)n, &from);
    }
    exit(0);
}

static void udp6_loopback(void) {
    long pid = fork();
    if (pid == 0)
        udp6_child();
    msleep(300);
    long s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    check("udp6 socket", s >= 0);
    struct sockaddr_in6 dst;
    mkaddr6(&dst, lo1, UPORT);
    const char *msg = "udp6 hello";
    check("udp6 send", sendto6((int)s, msg, 11, &dst) == 11);
    char rb[64];
    struct sockaddr_in6 from;
    long n = -1;
    for (int t = 0; t < 100 && n <= 0; t++) {
        n = recvfrom6((int)s, rb, sizeof(rb), &from);
        if (n <= 0)
            msleep(20);
    }
    check("udp6 echo", n == 11 && !memcmp(rb, msg, 11));
    check("udp6 src ::1", n > 0 &&
          !memcmp(from.sin6_addr, lo1, 16));
    close((int)s);
    int st = -1;
    wait(&st);
    check("udp6 child", st == 0);
}

// --- 3. tcp6 stream over ::1 ---

#define TPORT 8
#define TOTAL 4096

static void tcp6_loopback(void) {
    long lfd = socket(AF_INET6, SOCK_STREAM, 0);
    check("tcp6 socket", lfd >= 0);
    struct sockaddr_in6 a;
    mkaddr6(&a, any6, TPORT);
    check("tcp6 bind", bind6((int)lfd, &a, sizeof(a)) == 0);
    check("tcp6 listen", listen((int)lfd, 4) == 0);

    long cfd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 dst;
    mkaddr6(&dst, lo1, TPORT);
    long rc = -1;
    for (int t = 0; t < 100 && rc != 0; t++) {
        rc = connect6((int)cfd, &dst, sizeof(dst));
        if (rc != 0)
            msleep(20);
    }
    check("tcp6 connect", rc == 0);

    long afd = -1;
    for (int t = 0; t < 100 && afd < 0; t++) {
        afd = accept((int)lfd, 0, 0);
        if (afd < 0)
            msleep(20);
    }
    check("tcp6 accept", afd >= 0);

    static unsigned char sbuf[TOTAL], rbuf[TOTAL], tmp[1400];
    for (int i = 0; i < TOTAL; i++)
        sbuf[i] = (unsigned char)(i * 13 + i / 97);
    long sent = 0, got = 0;
    int stalled = 0;
    while (got < TOTAL && stalled < 3000) {
        int progress = 0;
        if (sent < TOTAL) {
            long n = send((int)cfd, sbuf + sent,
                          (unsigned long)(TOTAL - sent));
            if (n > 0) {
                sent += n;
                progress = 1;
            }
        }
        long n = recv((int)afd, tmp, sizeof(tmp));
        if (n > 0) {
            send((int)afd, tmp, (unsigned long)n);
            progress = 1;
        }
        if (got < TOTAL) {
            long m = recv((int)cfd, rbuf + got,
                          (unsigned long)(TOTAL - got));
            if (m > 0) {
                got += m;
                progress = 1;
            }
        }
        if (!progress) {
            msleep(10);
            stalled++;
        }
    }
    check("tcp6 roundtrip", sent == TOTAL && got == TOTAL);
    check("tcp6 payload", got == TOTAL &&
          !memcmp(sbuf, rbuf, (unsigned long)TOTAL));

    close((int)afd);
    int eof = 0;
    for (int t = 0; t < 200 && !eof; t++) {
        long m = recv((int)cfd, tmp, sizeof(tmp));
        if (m == 0)
            eof = 1;
        else if (m < 0)
            msleep(10);
    }
    check("tcp6 eof", eof);
    close((int)cfd);
    close((int)lfd);
}

// --- 4. link-local slirp peer (non-fatal: needs a v6-aware host) ---

static void ping6_fe80(void) {
    // libslirp's host side answers on fe80::2 for 10.0.2.0/24 nets
    unsigned char fe80_2[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 2};
    long s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    if (s < 0)
        return;
    struct sockaddr_in6 dst;
    mkaddr6(&dst, fe80_2, 0);
    for (int t = 0; t < 3; t++)
        sendto6((int)s, "ping6-fe80", 10, &dst);
    char rb[32];
    struct sockaddr_in6 from;
    long n = -1;
    for (int t = 0; t < 50 && n <= 0; t++) {
        n = recvfrom6((int)s, rb, sizeof(rb), &from);
        if (n <= 0)
            msleep(20);
    }
    printf("[ip6test] %-24s %s\n", "slirp fe80::2 ping",
           n == 10 ? "ok" : "skip");
    close((int)s);
}

int main(void) {
    ping6_loopback();
    udp6_loopback();
    tcp6_loopback();
    ping6_fe80();
    puts(fail ? "[ip6test] FAIL\n" : "[ip6test] all ok\n");
    return fail;
}
