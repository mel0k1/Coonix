// dnstest: kernel resolver against an in-guest udp dns server
// (hermetic), then against the slirp resolver (non-fatal)
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fail;

static void check(const char *what, int ok) {
    printf("[dnstest] %-24s %s\n", what, ok ? "ok" : "FAIL");
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

static unsigned int be(unsigned int host) {
    return __builtin_bswap32(host);
}

// decode the question name of a query into dotted form
static int get_qname(const unsigned char *q, int qlen, char *out,
                     int cap, int *name_end) {
    int o = 12, w = 0;
    while (o < qlen) {
        int l = q[o];
        if (!l) {
            o++;
            break;
        }
        if (l & 0xc0)
            return -1;
        if (o + 1 + l > qlen || w + l + 2 > cap)
            return -1;
        if (w)
            out[w++] = '.';
        __builtin_memcpy(out + w, q + o + 1, (unsigned long)l);
        w += l;
        o += 1 + l;
    }
    out[w] = 0;
    *name_end = o;
    return 0;
}

// child: a tiny authoritative udp server on :53 for two names
static void dns_child(void) {
    long s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        exit(1);
    struct sockaddr_in a;
    __builtin_memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    unsigned short pbe = (unsigned short)((53 >> 8) | (53 << 8));
    __builtin_memcpy(&a.sin_port, &pbe, 2);
    if (bind((int)s, &a, sizeof(a)) < 0)
        exit(1);
    for (int i = 0; i < 12; i++) {
        unsigned char q[512], r[512];
        struct sockaddr_in from;
        long n;
        do {                     // non-blocking: poll until one lands
            n = recvfrom((int)s, q, sizeof(q), &from);
            if (n <= 0)
                nanosleep(0, 5 * 1000000);
        } while (n <= 0);
        char name[256];
        int ne = 0;
        if (get_qname(q, (int)n, name, sizeof(name), &ne) < 0)
            break;
        int nxdomain = !strcmp(name, "none.internal");
        int done = !strcmp(name, "done.internal");
        __builtin_memcpy(r, q, 12);          // echo id
        r[2] = 0x81;                         // qr + rd
        r[3] = nxdomain ? 0x83 : 0x80;       // ra + rcode
        r[4] = 0;
        r[5] = 1;                            // qdcount
        r[6] = 0;
        r[7] = nxdomain ? 0 : 1;             // ancount
        r[8] = r[9] = r[10] = r[11] = 0;
        int o = 12;
        __builtin_memcpy(r + o, q + 12, (unsigned long)(ne - 12) + 4);
        o += (ne - 12) + 4;                  // question + qtype/qclass
        if (!nxdomain) {
            r[o++] = 0xc0;                   // pointer to the qname
            r[o++] = 0x0c;
            r[o++] = 0;
            r[o++] = 1;                      // type A
            r[o++] = 0;
            r[o++] = 1;                      // class IN
            r[o++] = 0;
            r[o++] = 0;
            r[o++] = 0;
            r[o++] = 60;                     // ttl
            r[o++] = 0;
            r[o++] = 4;                      // rdlength
            unsigned int loop = be(ipaddr(127, 0, 0, 1));
            __builtin_memcpy(r + o, &loop, 4);
            o += 4;
        }
        sendto((int)s, r, (unsigned long)o, &from);
        if (done)
            exit(0);             // sentinel answered: server is done
    }
    exit(0);
}

int main(void) {
    // resolver -> our own server
    unsigned int old = (unsigned int)setdnsserver(be(ipaddr(127, 0, 0, 1)));
    check("setdnsserver", old == be(ipaddr(10, 0, 2, 3)));

    long pid = fork();
    if (pid == 0)
        dns_child();
    msleep(300);                     // let the child bind

    unsigned int ip = 0;
    check("resolve test.internal",
          gethostbyname("test.internal", &ip) == 0 &&
          ip == be(ipaddr(127, 0, 0, 1)));

    ip = 0xdeadbeef;
    check("nxdomain fails",
          gethostbyname("none.internal", &ip) < 0 && ip == 0xdeadbeef);

    // tell the server to quit: it answers this, then exits
    ip = 0;
    check("resolve done.internal",
          gethostbyname("done.internal", &ip) == 0 &&
          ip == be(ipaddr(127, 0, 0, 1)));

    // reap the echo server
    int st = -1;
    wait(&st);
    check("child exit", st == 0);
    puts(fail ? "[dnstest] FAIL\n" : "[dnstest] all ok\n");
    return fail;
}
