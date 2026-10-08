#include "net.h"
#include "tcp.h"
#include "dhcp.h"
#include "sync.h"
#include "task.h"
#include "heap.h"
#include "string.h"
#include "pit.h"
#include "console.h"
#include "kernel.h"

// global stack lock: the rx demux runs from the timer tick (irq
// context) while sockets are driven from task context. one cpu +
// irqsave + a depth counter gives recursive protection with no
// deadlock window (the tick can never fire while a task holds it)
static spinlock_t net_lock = SPINLOCK_INIT;
static int net_depth;
static uint64_t net_flags;

static void net_lock_enter(void) {
    if (net_depth++ == 0)
        spin_lock_irqsave(&net_lock, &net_flags);
}

static void net_lock_leave(void) {
    if (--net_depth == 0)
        spin_unlock_irqrestore(&net_lock, net_flags);
}

// small host stack: arp cache, icmp echo, udp datagrams, loopback.
// sockets are static slots; rx queues are fixed rings (no irq-context
// allocation anywhere)

#define NSOCKS     8
#define NQUEUED    8
#define PAYLOAD    1200

struct netpkt {
    uint16_t len;
    uint16_t rsv;
    uint32_t src_ip;
    uint16_t src_port;      // udp src port; icmp: 0
    uint8_t data[PAYLOAD];
};

struct netsock {
    int used;
    int proto;
    uint16_t port;          // udp: local port; icmp: echo id
    uint16_t seq;
    void *tcp;              // proto tcp: struct tcpsk
    int rx_head, rx_count;
    struct netpkt ring[NQUEUED];
};

static struct netsock socks[NSOCKS];
static uint16_t next_port = 40000;

struct net_mac net_hwaddr = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
uint32_t net_local_ip = 0x0a00020f;   // 10.0.2.15 (qemu slirp default)
uint32_t net_gw_ip = 0x0a000202;      // 10.0.2.2
uint32_t net_netmask = 0xffffff00;
struct net_nic net_nic;

// -- arp -----------------------------------------------------------------

struct arp_ent {
    uint32_t ip;
    struct net_mac mac;
    uint64_t age;
    int valid;
};
static struct arp_ent arp_tab[8];

static rwlock_t arp_lock = RWLOCK_INIT;

static int arp_lookup(uint32_t ip, struct net_mac *out) {
    int found = 0;
    rw_read_lock(&arp_lock);
    for (int i = 0; i < 8; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip) {
            *out = arp_tab[i].mac;
            found = 1;
        }
    rw_read_unlock(&arp_lock);
    return found;
}

static void arp_learn(uint32_t ip, const struct net_mac *mac) {
    if (!ip)
        return;
    rw_write_lock(&arp_lock);
    struct arp_ent *slot = 0;
    for (int i = 0; i < 8 && !slot; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip)
            slot = &arp_tab[i];
    for (int i = 0; i < 8 && !slot; i++)
        if (!arp_tab[i].valid)
            slot = &arp_tab[i];
    if (!slot)
        slot = &arp_tab[0];
    slot->ip = ip;
    slot->mac = *mac;
    slot->age = pit_ticks();
    slot->valid = 1;
    rw_write_unlock(&arp_lock);
}

// -- wire helpers --------------------------------------------------------

static uint16_t cksum(const void *buf, uint16_t len) {
    // big-endian words (rfc 1071): wire-compatible with real peers
    const uint8_t *p = buf;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += p[0] << 8;    // odd tail rides the high byte
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void *put_mac(void *p, const struct net_mac *m) {
    memcpy(p, m->b, 6);
    return (uint8_t *)p + 6;
}

uint16_t net_pseudo_cksum(uint32_t src, uint32_t dst, uint8_t proto,
                          const void *payload, uint16_t plen) {
    // big-endian words (rfc 1071); 0 on receive = valid checksum
    uint32_t sum = 0;
    sum += (src >> 16) & 0xffff;
    sum += src & 0xffff;
    sum += (dst >> 16) & 0xffff;
    sum += dst & 0xffff;
    sum += proto;
    sum += plen;
    const uint8_t *p = payload;
    uint16_t n = plen;
    while (n > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }
    if (n)
        sum += p[0] << 8;        // odd tail rides the high byte
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

// -- ip ------------------------------------------------------------------

static void ip_output(const uint8_t *payload, uint16_t plen,
                      uint8_t proto, uint32_t dst) {
    net_ip_output(payload, plen, proto, dst);
}

void net_ip_output(const void *payload, uint16_t plen,
                   uint8_t proto, uint32_t dst) {
    // one buffer: ip header + payload (the loopback hands the whole
    // packet straight back to net_input)
    uint8_t pkt[NET_IP_LEN + NET_MTU];
    if (plen > NET_MTU)
        return;
    // loopback traffic carries the loopback address as source, so a
    // socket talking to 127.0.0.1 sees replies from its own peer ip
    uint32_t src = (dst == 0x7f000001) ? 0x7f000001 : net_local_ip;
    uint8_t *ip = pkt;
    ip[0] = 0x45;                    // v4, ihl 5
    ip[1] = 0;                       // tos
    ip[2] = (uint8_t)((plen + NET_IP_LEN) >> 8);
    ip[3] = (uint8_t)(plen + NET_IP_LEN);
    ip[4] = ip[5] = 0;               // id
    ip[6] = ip[7] = 0;               // no fragmentation
    ip[8] = 64;                      // ttl
    ip[9] = proto;
    ip[10] = ip[11] = 0;             // checksum
    ip[12] = (uint8_t)(src >> 24);
    ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);
    ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst >> 24);
    ip[17] = (uint8_t)(dst >> 16);
    ip[18] = (uint8_t)(dst >> 8);
    ip[19] = (uint8_t)dst;
    memcpy(pkt + NET_IP_LEN, payload, plen);
    uint16_t sum = cksum(ip, NET_IP_LEN);
    ip[10] = (uint8_t)(sum >> 8);
    ip[11] = (uint8_t)sum;

    if (proto == NET_PROTO_UDP || proto == NET_PROTO_TCP) {
        // checksum over the pseudo header (patched into our copy);
        // udp keeps it at offset 6, tcp at 16
        uint8_t *u = pkt + NET_IP_LEN;
        uint16_t cks = (proto == NET_PROTO_TCP) ? 16 : 6;
        u[cks] = u[cks + 1] = 0;
        uint16_t c = net_pseudo_cksum(src, dst, proto, u, plen);
        if (!c)
            c = 0xffff;
        u[cks] = (uint8_t)(c >> 8);
        u[cks + 1] = (uint8_t)c;
    }

    net_stack_output(pkt, (uint16_t)(plen + NET_IP_LEN), dst);
}

// -- ethernet ------------------------------------------------------------

static void arp_request(uint32_t ip);

// loopback deliveries are deferred to the tick: a synchronous call
// back into net_input nests tx+rx frames ~12k deep on the 16k kstack
#define LO_Q 16
static uint8_t lo_ring[LO_Q][NET_IP_LEN + NET_MTU];
static uint16_t lo_len[LO_Q];
static int lo_head, lo_count;

static void net_lo_deliver(void) {
    while (lo_count) {
        uint8_t pkt[NET_IP_LEN + NET_MTU];
        uint16_t n = lo_len[lo_head];
        memcpy(pkt, lo_ring[lo_head], n);
        lo_head = (lo_head + 1) % LO_Q;
        lo_count--;
        net_input(pkt, n);       // recursive: depth counter handles it
    }
}

void net_stack_output(const void *buf, uint16_t len, uint32_t dst_ip) {
    net_lock_enter();
    // loopback: queue for the tick (net_lo_deliver drains it)
    if (dst_ip == net_local_ip || dst_ip == 0x7f000001) {
        if (len <= NET_IP_LEN + NET_MTU && lo_count < LO_Q) {
            int t = (lo_head + lo_count) % LO_Q;
            memcpy(lo_ring[t], buf, len);
            lo_len[t] = (uint16_t)len;
            lo_count++;
        }
        net_lock_leave();
        return;
    }
    if (!net_nic.send) {
        net_lock_leave();
        return;
    }

    uint8_t frame[NET_ETH_LEN + NET_MTU];
    struct net_mac dst_mac;
    static const struct net_mac bcast =
        {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
    if (dst_ip == 0xffffffff) {
        // ip broadcast needs no arp
        dst_mac = bcast;
    } else if (!arp_lookup(dst_ip, &dst_mac) &&
               !arp_lookup(net_gw_ip, &dst_mac)) {
        arp_request(dst_ip);   // ask; the caller retries later
        net_lock_leave();
        return;
    }
    uint8_t *p = put_mac(frame, &dst_mac);
    p = put_mac(p, &net_hwaddr);
    uint16_t et = 0x0800;            // ipv4
    p[0] = (uint8_t)(et >> 8);
    p[1] = (uint8_t)et;
    if ((uint32_t)NET_ETH_LEN + len > sizeof(frame)) {
        net_lock_leave();
        return;
    }
    memcpy(p + 2, buf, len);
    net_nic.send(frame, (uint16_t)(NET_ETH_LEN + len));
    net_lock_leave();
}

// -- icmp ----------------------------------------------------------------

// src_ip = the ip-layer source address (the icmp packet itself does
// not carry it)
static void icmp_input(const uint8_t *ip, uint16_t iplen, uint32_t src_ip) {
    if (iplen < NET_ICMP_LEN)
        return;
    uint8_t type = ip[0];
    if (type == NET_ICMP_ECHO_REQUEST) {
        // echo reply: swap addresses, retype, rechecksum
        uint8_t reply[NET_MTU];
        if ((uint32_t)iplen > sizeof(reply))
            return;
        memcpy(reply, ip, iplen);
        reply[0] = NET_ICMP_ECHO_REPLY;
        reply[2] = reply[3] = 0;
        uint16_t sum = cksum(reply, iplen);
        reply[2] = (uint8_t)(sum >> 8);
        reply[3] = (uint8_t)sum;
        // rebuild the ip header around it: copy dst from the ip src
        ip_output(reply, iplen, NET_PROTO_ICMP, src_ip);
        return;
    }
    if (type == NET_ICMP_ECHO_REPLY) {
        // deliver to the ping socket whose id matches
        uint16_t id = ((uint16_t)ip[4] << 8) | ip[5];
        uint16_t seq = ((uint16_t)ip[6] << 8) | ip[7];
        for (int i = 0; i < NSOCKS; i++) {
            struct netsock *sk = &socks[i];
            if (!sk->used || sk->proto != NET_PROTO_ICMP || sk->port != id)
                continue;
            if (sk->rx_count < NQUEUED && iplen >= 8) {
                struct netpkt *pk = &sk->ring[(sk->rx_head + sk->rx_count) % NQUEUED];
                uint16_t pl = iplen - NET_ICMP_LEN;
                if (pl > PAYLOAD)
                    pl = PAYLOAD;
                memcpy(pk->data, ip + NET_ICMP_LEN, pl);
                pk->len = pl;
                pk->src_ip = src_ip;
                pk->src_port = seq;
                sk->rx_count++;
            }
        }
    }
}

// -- udp -----------------------------------------------------------------

static void udp_input(const uint8_t *udp, uint16_t iplen, uint32_t src) {
    if (iplen < NET_UDP_LEN)
        return;
    uint16_t dport = ((uint16_t)udp[2] << 8) | udp[3];
    uint16_t sport = ((uint16_t)udp[0] << 8) | udp[1];
    uint16_t ulen = ((uint16_t)udp[4] << 8) | udp[5];
    if (ulen < NET_UDP_LEN || iplen < ulen)
        return;
    uint16_t plen = ulen - NET_UDP_LEN;
    const uint8_t *payload = udp + NET_UDP_LEN;
    for (int i = 0; i < NSOCKS; i++) {
        struct netsock *sk = &socks[i];
        if (!sk->used || sk->proto != NET_PROTO_UDP || sk->port != dport)
            continue;
        if (sk->rx_count >= NQUEUED)
            continue;
        struct netpkt *pk = &sk->ring[(sk->rx_head + sk->rx_count) % NQUEUED];
    if (plen > (uint16_t)PAYLOAD)
            plen = (uint16_t)PAYLOAD;
        memcpy(pk->data, payload, plen);
        pk->len = plen;
        pk->src_ip = src;
        pk->src_port = sport;
        sk->rx_count++;
    }
}

// -- arp frames ----------------------------------------------------------

static void arp_input(const uint8_t *pkt, uint16_t len) {
    if (len < NET_ARP_LEN)
        return;
    uint16_t op = ((uint16_t)pkt[6] << 8) | pkt[7];
    uint32_t spa = ((uint32_t)pkt[14] << 24) | ((uint32_t)pkt[15] << 16) |
                   ((uint32_t)pkt[16] << 8) | pkt[17];
    struct net_mac sha;
    memcpy(sha.b, pkt + 8, 6);
    arp_learn(spa, &sha);
    if (op != 1)                     // not a request
        return;
    uint32_t tpa = ((uint32_t)pkt[24] << 24) | ((uint32_t)pkt[25] << 16) |
                   ((uint32_t)pkt[26] << 8) | pkt[27];
    if (tpa != net_local_ip || !net_nic.send) {
        net_lock_leave();
        return;
    }
    // reply
    uint8_t fr[NET_ETH_LEN + NET_ARP_LEN];
    uint8_t *p = put_mac(fr, &sha);
    p = put_mac(p, &net_hwaddr);
    *p++ = 0x08; *p++ = 0x06;        // arp
    p[0] = 0x00; p[1] = 0x01;        // ethernet
    p[2] = 0x08; p[3] = 0x00;        // ipv4
    p[4] = 6; p[5] = 4;              // sizes
    p[6] = 0x00; p[7] = 0x02;        // reply
    p = put_mac(p + 8, &net_hwaddr);
    *p++ = (uint8_t)(net_local_ip >> 24);
    *p++ = (uint8_t)(net_local_ip >> 16);
    *p++ = (uint8_t)(net_local_ip >> 8);
    *p++ = (uint8_t)net_local_ip;
    p = put_mac(p, &sha);
    *p++ = (uint8_t)(spa >> 24);
    *p++ = (uint8_t)(spa >> 16);
    *p++ = (uint8_t)(spa >> 8);
    *p++ = (uint8_t)spa;
    net_nic.send(fr, sizeof(fr));
    net_lock_leave();
}

void net_input(const uint8_t *pkt, uint16_t len) {
    net_lock_enter();
    if (len < NET_IP_LEN || (pkt[0] >> 4) != 4) {
        net_lock_leave();
        return;
    }
    // ip version 4 only; arp arrives via net_arp_input (the driver
    // strips the ethernet header and routes by ethertype)
    uint8_t proto = pkt[9];
    uint16_t tot = ((uint16_t)pkt[2] << 8) | pkt[3];
    if (tot < NET_IP_LEN || tot > len) {
        net_lock_leave();
        return;
    }
    uint16_t plen = tot - NET_IP_LEN;
    const uint8_t *payload = pkt + NET_IP_LEN;
    if (proto == NET_PROTO_ICMP) {
        uint32_t src = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                       ((uint32_t)pkt[14] << 8) | pkt[15];
        icmp_input(payload, plen, src);
    }
    else if (proto == NET_PROTO_UDP) {
        uint32_t src = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                       ((uint32_t)pkt[14] << 8) | pkt[15];
        udp_input(payload, plen, src);
    }
    else if (proto == NET_PROTO_TCP) {
        uint32_t src = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                       ((uint32_t)pkt[14] << 8) | pkt[15];
        uint32_t dst = ((uint32_t)pkt[16] << 24) | ((uint32_t)pkt[17] << 16) |
                       ((uint32_t)pkt[18] << 8) | pkt[19];
        tcp_input(payload, plen, src, dst);
    }
    net_lock_leave();
}

// raw arp frame from the nic (ethernet header stripped by the driver)
void net_arp_input(const uint8_t *pkt, uint16_t len) {
    net_lock_enter();
    arp_input(pkt, len);
}

// -- public api ----------------------------------------------------------

void net_init(void) {
    memset(socks, 0, sizeof(socks));
    net_nic.send = 0;
    tcp_init();
}

uint16_t net_next_port(void) {
    return next_port++;
}

int net_socket(int proto) {
    net_lock_enter();
    if (proto != NET_PROTO_UDP && proto != NET_PROTO_ICMP &&
        proto != NET_PROTO_TCP) {
        net_lock_leave();
        return -1;
    }
    for (int i = 0; i < NSOCKS; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = 1;
            socks[i].proto = proto;
            if (proto == NET_PROTO_TCP) {
                socks[i].tcp = tcp_alloc();
                if (!socks[i].tcp) {
                    socks[i].used = 0;
                    net_lock_leave();
                    return -1;
                }
            } else {
                socks[i].port = next_port++;
            }
            net_lock_leave();
            return i;
        }
    }
    net_lock_leave();
    return -1;
}

void net_close(int fd) {
    net_lock_enter();
    if (fd >= 0 && fd < NSOCKS && socks[fd].used) {
        if (socks[fd].proto == NET_PROTO_TCP && socks[fd].tcp)
            tcp_destroy(socks[fd].tcp);
        socks[fd].tcp = 0;
        socks[fd].used = 0;
    }
    net_lock_leave();
}

int net_bind(int fd, uint16_t port) {
    net_lock_enter();
    int rc = -1;
    if (fd >= 0 && fd < NSOCKS && socks[fd].used) {
        if (socks[fd].proto == NET_PROTO_TCP && socks[fd].tcp)
            rc = tcp_bind(socks[fd].tcp, port);
        else if (socks[fd].proto == NET_PROTO_UDP) {
            socks[fd].port = port;
            rc = 0;
        }
    }
    net_lock_leave();
    return rc;
}

int net_listen(int fd, int backlog) {
    net_lock_enter();
    int rc = -1;
    if (fd >= 0 && fd < NSOCKS && socks[fd].used &&
        socks[fd].proto == NET_PROTO_TCP && socks[fd].tcp)
        rc = tcp_listen(socks[fd].tcp, backlog);
    net_lock_leave();
    return rc;
}

int net_connect(int fd, uint32_t ip, uint16_t port) {
    net_lock_enter();
    int rc = -1;
    if (fd >= 0 && fd < NSOCKS && socks[fd].used &&
        socks[fd].proto == NET_PROTO_TCP && socks[fd].tcp)
        rc = tcp_connect(socks[fd].tcp, ip, port);
    net_lock_leave();
    return rc;
}

long net_accept(int fd, uint32_t *ip, uint16_t *port) {
    net_lock_enter();
    long nfd = -1;
    if (fd >= 0 && fd < NSOCKS && socks[fd].used &&
        socks[fd].proto == NET_PROTO_TCP && socks[fd].tcp) {
        void *ch = tcp_accept(socks[fd].tcp, ip, port);
        if (ch) {
            // wrap the child in its own socket slot
            for (int i = 0; i < NSOCKS && nfd < 0; i++) {
                if (!socks[i].used) {
                    memset(&socks[i], 0, sizeof(socks[i]));
                    socks[i].used = 1;
                    socks[i].proto = NET_PROTO_TCP;
                    socks[i].tcp = ch;
                    nfd = i;
                }
            }
            if (nfd < 0)
                tcp_destroy(ch);   // no slot: drop the child
        }
    }
    net_lock_leave();
    return nfd;
}

static void arp_request(uint32_t ip) {
    if (!net_nic.send)
        return;
    uint8_t fr[NET_ETH_LEN + NET_ARP_LEN];
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t *p = put_mac(fr, (struct net_mac *)bcast);
    p = put_mac(p, &net_hwaddr);
    *p++ = 0x08; *p++ = 0x06;
    p[0] = 0x00; p[1] = 0x01;
    p[2] = 0x08; p[3] = 0x00;
    p[4] = 6; p[5] = 4;
    p[6] = 0x00; p[7] = 0x01;        // request
    p = put_mac(p + 8, &net_hwaddr);
    *p++ = (uint8_t)(net_local_ip >> 24);
    *p++ = (uint8_t)(net_local_ip >> 16);
    *p++ = (uint8_t)(net_local_ip >> 8);
    *p++ = (uint8_t)net_local_ip;
    memset(p, 0, 6);                 // target mac unknown
    p += 6;
    *p++ = (uint8_t)(ip >> 24);
    *p++ = (uint8_t)(ip >> 16);
    *p++ = (uint8_t)(ip >> 8);
    *p++ = (uint8_t)ip;
    net_nic.send(fr, sizeof(fr));
}

int net_sendto(int fd, const void *buf, uint16_t len,
               uint32_t ip, uint16_t port) {
    net_lock_enter();
    if (fd < 0 || fd >= NSOCKS || !socks[fd].used) {
        net_lock_leave();
        return -1;
    }
    struct netsock *sk = &socks[fd];
    uint8_t pkt[NET_MTU];
    uint16_t plen;
    if (sk->proto == NET_PROTO_TCP) {
        // ip/port args are ignored: the stream uses the connected peer
        long rc = sk->tcp ? tcp_send(sk->tcp, buf, len) : -1;
        net_lock_leave();
        return (int)rc;
    }
    if (sk->proto == NET_PROTO_UDP) {
        if (len > NET_MTU - NET_UDP_LEN) {
            net_lock_leave();
            return -1;
        }
        pkt[0] = (uint8_t)(sk->port >> 8);    // src port
        pkt[1] = (uint8_t)sk->port;
        pkt[2] = (uint8_t)(port >> 8);        // dst port
        pkt[3] = (uint8_t)port;
        pkt[4] = (uint8_t)((len + NET_UDP_LEN) >> 8);
        pkt[5] = (uint8_t)(len + NET_UDP_LEN);
        pkt[6] = pkt[7] = 0;                  // checksum filled later
        memcpy(pkt + NET_UDP_LEN, buf, len);
        plen = (uint16_t)(len + NET_UDP_LEN);
        ip_output(pkt, plen, NET_PROTO_UDP, ip);
        net_lock_leave();
        return len;
    }
    // icmp ping: echo request, id = port, seq increments
    if (len > NET_MTU - NET_ICMP_LEN) {
        net_lock_leave();
        return -1;
    }
    pkt[0] = NET_ICMP_ECHO_REQUEST;
    pkt[1] = 0;
    pkt[2] = pkt[3] = 0;              // checksum (filled below)
    pkt[4] = (uint8_t)(sk->port >> 8);   // identifier
    pkt[5] = (uint8_t)sk->port;
    pkt[6] = (uint8_t)(sk->seq >> 8);    // sequence
    pkt[7] = (uint8_t)sk->seq;
    memcpy(pkt + NET_ICMP_LEN, buf, len);
    uint16_t icmplen = (uint16_t)(len + NET_ICMP_LEN);
    uint16_t sum = cksum(pkt, icmplen);
    pkt[2] = (uint8_t)(sum >> 8);
    pkt[3] = (uint8_t)sum;
    sk->seq++;
    plen = icmplen;
    ip_output(pkt, plen, NET_PROTO_ICMP, ip);
    net_lock_leave();
    return len;
}

long net_recvfrom(int fd, void *buf, uint16_t len,
                  uint32_t *src_ip, uint16_t *src_port) {
    net_lock_enter();
    if (fd < 0 || fd >= NSOCKS || !socks[fd].used) {
        net_lock_leave();
        return -1;
    }
    struct netsock *sk = &socks[fd];
    if (sk->proto == NET_PROTO_TCP) {
        long rc = sk->tcp ? tcp_recv(sk->tcp, buf, len) : -1;
        if (rc >= 0 && sk->tcp)
            tcp_peer(sk->tcp, src_ip, src_port);
        net_lock_leave();
        return rc;
    }
    if (!sk->rx_count) {
        net_lock_leave();
        return 0;
    }
    struct netpkt *pk = &sk->ring[sk->rx_head % NQUEUED];
    uint16_t n = pk->len < len ? pk->len : len;
    memcpy(buf, pk->data, n);
    if (src_ip)
        *src_ip = pk->src_ip;
    if (src_port)
        *src_port = pk->src_port;
    sk->rx_head++;
    sk->rx_count--;
    net_lock_leave();
    return n;
}

void net_poll(void) {
    // the nic driver drains its rx ring here (timer-tick context)
    extern void virtio_net_poll(void);
    virtio_net_poll();
    net_lock_enter();
    net_lo_deliver();
    tcp_poll();
    dhcp_poll();
    net_lock_leave();
}
