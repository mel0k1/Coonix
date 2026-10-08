#include "dns.h"
#include "net.h"
#include "string.h"

// stateless non-blocking dns step. the caller (a libc retry loop that
// sleeps between calls) drives the timeout; nothing here blocks, so a
// slow server can never stall the tick or another task. replies are
// matched by question name instead of the tx id: concurrent resolvers
// could cross-match in theory, which a single-user hobby stack accepts.

#define DNS_PORT      53
#define DNS_QTYPE_A   1
#define DNS_QCLASS_IN 1
#define DNS_RCODE_NXDOMAIN 3

// persistent udp socket so replies survive between resolver calls
static int dns_sock = -1;

void dns_init(void) {
    if (dns_sock < 0)
        dns_sock = net_socket(NET_PROTO_UDP);
}

// encode "a.b.c" as "\x03a\x01b\x01c"; returns bytes written
static int put_name(uint8_t *q, const char *name, int cap) {
    int o = 0;
    const char *p = name;
    while (*p) {
        const char *lab = p;
        while (*p && *p != '.')
            p++;
        int len = (int)(p - lab);
        if (len <= 0 || len > 63 || o + 1 + len + 1 > cap)
            return -1;
        q[o++] = (uint8_t)len;
        memcpy(q + o, lab, (uint64_t)len);
        o += len;
        if (*p == '.')
            p++;
    }
    if (!o)
        return -1;
    q[o++] = 0;                  // root label
    return o;
}

// read a name (no compression) into dotted form; returns the offset
// past it or -1
static int read_name(const uint8_t *msg, int len, int off,
                     char *out, int cap) {
    int w = 0;
    for (int guard = 0; guard < 128; guard++) {
        if (off >= len)
            return -1;
        uint8_t c = msg[off];
        if (!c) {
            off++;
            if (!w)
                return -1;
            out[w] = 0;
            return off;
        }
        if (c & 0xc0)
            return -1;
        if (off + 1 + c > len || w + c + 2 > cap)
            return -1;
        if (w)
            out[w++] = '.';
        memcpy(out + w, msg + off + 1, c);
        w += c;
        off += 1 + c;
    }
    return -1;
}

// parse one reply for the wanted name. 0 = resolved, 1 = not ours/no
// answer yet, -2 = nxdomain, -3 = malformed
static int parse_reply(const uint8_t *rb, int n, const char *want,
                       uint32_t *out_ip) {
    if (n < 12)
        return -3;
    uint16_t flags = (uint16_t)((rb[2] << 8) | rb[3]);
    if (!(flags & 0x8000))
        return 1;                // not a response
    int rcode = (int)(flags & 0x000f);
    char qname[256];
    int off = read_name(rb, n, 12, qname, (int)sizeof(qname));
    if (off < 0 || off + 4 > n)
        return -3;
    if (strcmp(qname, want) != 0)
        return 1;                // someone else's reply
    if (rcode == DNS_RCODE_NXDOMAIN)
        return -2;
    if (rcode)
        return 1;
    int an = (int)((rb[6] << 8) | rb[7]);
    off += 4;                    // qtype + qclass
    for (int i = 0; i < an && off < n; i++) {
        // owner name: usually a compression pointer; skip it either way
        if (off < n && (rb[off] & 0xc0) == 0xc0)
            off += 2;
        else {
            int no = read_name(rb, n, off, qname, (int)sizeof(qname));
            if (no < 0)
                return -3;
            off = no;
        }
        if (off + 10 > n)
            return -3;
        uint16_t type = (uint16_t)((rb[off] << 8) | rb[off + 1]);
        uint16_t rdlen = (uint16_t)((rb[off + 8] << 8) | rb[off + 9]);
        off += 10;
        if (off + rdlen > n)
            return -3;
        if (type == DNS_QTYPE_A && rdlen == 4) {
            memcpy(out_ip, rb + off, 4);
            *out_ip = __builtin_bswap32(*out_ip);
            return 0;
        }
        off += rdlen;
    }
    return 1;                    // no A record in this reply
}

int dns_resolve(const char *name, uint32_t server, uint32_t *out_ip) {
    if (!name || !name[0] || !out_ip || dns_sock < 0)
        return -1;
    if (!server)
        server = net_dns_ip;
    if (!server)
        return -1;

    // 1. drain pending replies first: a query sent by an earlier call
    // may have been answered in between
    uint8_t rb[600];
    uint32_t sip;
    uint16_t sport;
    for (int i = 0; i < 4; i++) {
        long n = net_recvfrom(dns_sock, rb, sizeof(rb), &sip, &sport);
        if (n <= 0)
            break;
        int rc = parse_reply(rb, (int)n, name, out_ip);
        if (rc == 0)
            return 0;
        if (rc == -2)
            return -2;           // nxdomain: no point re-asking
    }

    // 2. (re)send the query
    uint8_t q[300];
    memset(q, 0, 12);
    q[2] = 0x01;                 // rd=1: please recurse
    q[5] = 1;                    // qdcount
    int qlen = put_name(q + 12, name, (int)sizeof(q) - 12 - 4);
    if (qlen < 0)
        return -1;
    qlen += 12;
    q[qlen++] = 0;
    q[qlen++] = DNS_QTYPE_A;
    q[qlen++] = 0;
    q[qlen++] = DNS_QCLASS_IN;
    if (net_sendto(dns_sock, q, (uint16_t)qlen, server, DNS_PORT) < 0)
        return -1;
    return 1;
}
