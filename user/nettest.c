// nettest: loopback udp + loopback icmp ping through the kernel stack
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("[nettest] %-24s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fail = 1;
}

static void msleep(long ms) {
    nanosleep(0, ms * 1000000);
}

static unsigned int ipaddr(unsigned a, unsigned b, unsigned c, unsigned d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// sockaddr_in keeps network byte order; build it from host parts
static void mkaddr(struct sockaddr_in *sa, unsigned int ip, unsigned port) {
    unsigned be = __builtin_bswap32(ip);
    __builtin_memcpy(&sa->sin_addr, &be, 4);
    unsigned short pbe = (unsigned short)((port >> 8) | (port << 8));
    __builtin_memcpy(&sa->sin_port, &pbe, 2);
    sa->sin_family = AF_INET;
    __builtin_memset(sa->sin_zero, 0, sizeof(sa->sin_zero));
}

int main(void) {
    long fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check("udp socket", fd >= 0);

    // --- udp loopback ---
    // bind a known port; loopback delivers dport matches
    struct sockaddr_in self;
    mkaddr(&self, ipaddr(0, 0, 0, 0), 40123);
    check("udp bind", bind((int)fd, &self, sizeof(self)) == 0);
    const char *msg = "enot rules the wire";
    struct sockaddr_in dst;
    mkaddr(&dst, ipaddr(127, 0, 0, 1), 40123);
    check("udp sendto lo", sendto((int)fd, msg, strlen(msg), &dst) ==
                             (long)strlen(msg));
    char buf[128];
    struct sockaddr_in from;
    long n = -1;
    for (int t = 0; t < 40 && n <= 0; t++) {
        msleep(25);
        n = recvfrom((int)fd, buf, sizeof(buf), &from);
    }
    check("udp recvfrom lo", n == (long)strlen(msg));
    check("udp payload", n > 0 && !memcmp(buf, msg, (unsigned long)n));

    // --- icmp ping over loopback ---
    long pf = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    check("icmp socket", pf >= 0);
    const char *ping = "ping-payload-0123";
    mkaddr(&dst, ipaddr(127, 0, 0, 1), 0);
    check("icmp sendto lo", sendto((int)pf, ping, 17, &dst) == 17);
    n = -1;
    for (int t = 0; t < 40 && n <= 0; t++) {
        msleep(25);
        n = recvfrom((int)pf, buf, sizeof(buf), &from);
    }
    check("icmp echo reply", n == 17 && !memcmp(buf, ping, 17));

    // --- nic ping (needs working hardware rx; non-fatal) ---
    pf = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    mkaddr(&dst, ipaddr(10, 0, 2, 2), 0);
    n = -1;
    for (int t = 0; t < 60 && n <= 0; t++) {
        msleep(25);
        // resend: the first frames race the arp resolution
        if (t % 10 == 0)
            sendto((int)pf, ping, 17, &dst);
        n = recvfrom((int)pf, buf, sizeof(buf), &from);
    }
    printf("[nettest] nic ping %s\n",
           n == 17 ? "ok" : "no reply (nic rx not verified)");

    if (!fail) {
        printf("[nettest] PASS\n");
        return 0;
    }
    printf("[nettest] FAIL\n");
    return 1;
}
