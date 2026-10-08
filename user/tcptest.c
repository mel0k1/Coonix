// tcptest: tcp echo over loopback through the kernel stack
// listener on port 7, client connects, 6k pattern echoed, fin teardown
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("[tcptest] %-24s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

static void msleep(long ms) {
    nanosleep(0, ms * 1000000);
}

static unsigned int ipaddr(unsigned a, unsigned b, unsigned c,
                           unsigned d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static void mkaddr(struct sockaddr_in *sa, unsigned int ip,
                   unsigned port) {
    unsigned be = __builtin_bswap32(ip);
    __builtin_memcpy(&sa->sin_addr, &be, 4);
    unsigned short pbe = (unsigned short)((port >> 8) | (port << 8));
    __builtin_memcpy(&sa->sin_port, &pbe, 2);
    sa->sin_family = AF_INET;
    __builtin_memset(sa->sin_zero, 0, sizeof(sa->sin_zero));
}

#define TPORT 7
#define TOTAL 6144

int main(void) {
    long lfd = socket(AF_INET, SOCK_STREAM, 0);
    check("tcp socket", lfd >= 0);

    struct sockaddr_in any;
    mkaddr(&any, ipaddr(0, 0, 0, 0), TPORT);
    check("tcp bind", bind((int)lfd, &any, sizeof(any)) == 0);
    check("tcp listen", listen((int)lfd, 4) == 0);

    // --- connect (handshake completes synchronously on loopback) ---
    long cfd = socket(AF_INET, SOCK_STREAM, 0);
    check("client socket", cfd >= 0);
    struct sockaddr_in dst;
    mkaddr(&dst, ipaddr(127, 0, 0, 1), TPORT);
    long rc = -1;
    for (int t = 0; t < 100 && rc != 0; t++) {
        rc = connect((int)cfd, &dst, sizeof(dst));
        if (rc != 0)
            msleep(20);
    }
    check("tcp connect", rc == 0);

    // --- accept ---
    long afd = -1;
    for (int t = 0; t < 100 && afd < 0; t++) {
        afd = accept((int)lfd, 0, 0);
        if (afd < 0)
            msleep(20);
    }
    check("tcp accept", afd >= 0);

    // --- echo: client sends, server reflects, client verifies ---
    // single process drives both ends; everything is non-blocking
    static unsigned char sbuf[TOTAL], rbuf[TOTAL], tmp[1400];
    for (int i = 0; i < TOTAL; i++)
        sbuf[i] = (unsigned char)(i * 7 + i / 251);

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
            send((int)afd, tmp, (unsigned long)n);   // reflect
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
    check("tcp roundtrip", sent == TOTAL && got == TOTAL);
    check("tcp payload", got == TOTAL &&
          !memcmp(sbuf, rbuf, (unsigned long)TOTAL));

    // --- congestion: slow start must have grown the window ---
    struct tcp_info_k ti;
    unsigned int ilen = sizeof(ti);
    check("tcp info", getsockopt((int)cfd, SOL_TCP, TCP_INFO, &ti,
                                 &ilen) == 0);
    check("tcp state est", ti.state == 4);   // TS_ESTABLISHED
    check("cwnd grew", ti.cwnd > 6000);      // IW was 2*MSS = 2400
    int soerr = -1;
    ilen = sizeof(soerr);
    check("so_error 0", getsockopt((int)cfd, SOL_SOCKET, SO_ERROR,
                                   &soerr, &ilen) == 0 && soerr == 0);

    // --- eof: server closes, client sees recv == 0 after draining ---
    close((int)afd);
    int eof = 0;
    for (int t = 0; t < 200 && !eof; t++) {
        long m = recv((int)cfd, tmp, sizeof(tmp));
        if (m == 0)
            eof = 1;
        else if (m < 0)
            msleep(10);
    }
    check("tcp eof after fin", eof);

    close((int)cfd);

    // --- second connection reuses the listener ---
    long c2 = socket(AF_INET, SOCK_STREAM, 0);
    rc = -1;
    for (int t = 0; t < 100 && rc != 0; t++) {
        rc = connect((int)c2, &dst, sizeof(dst));
        if (rc != 0)
            msleep(20);
    }
    check("tcp reconnect", rc == 0);
    const char *ping = "second";
    check("tcp send2", send((int)c2, ping, 6) == 6);
    long a2 = -1;
    for (int t = 0; t < 100 && a2 < 0; t++) {
        a2 = accept((int)lfd, 0, 0);
        if (a2 < 0)
            msleep(20);
    }
    check("tcp accept2", a2 >= 0);
    long n2 = -1;
    for (int t = 0; t < 100 && n2 < 0; t++) {
        n2 = recv((int)a2, tmp, sizeof(tmp));
        if (n2 < 0)
            msleep(20);
    }
    check("tcp recv2", n2 == 6 && !memcmp(tmp, ping, 6));
    close((int)a2);
    close((int)c2);
    close((int)lfd);

    if (!fail)
        printf("[tcptest] PASS\n");
    else
        printf("[tcptest] FAIL\n");
    return fail;
}
