#include "dhcp.h"
#include "net.h"
#include "pit.h"
#include "string.h"
#include "console.h"

// minimal dhcp client over the udp socket layer. everything runs from
// the timer tick: 3 tries per stage, then fall back to the static ip
// so a headless boot never hangs on a missing dhcp server.

#define DHCP_PORT_SERVER 67
#define DHCP_PORT_CLIENT 68

#define DHCPO_SUBNET   1
#define DHCPO_ROUTER   3
#define DHCPO_REQIP    50
#define DHCPO_MSGTYPE  53
#define DHCPO_SERVERID 54
#define DHCPO_PARAMREQ 55
#define DHCPO_END      255

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_RTO    200      // ticks per try
#define DHCP_TRIES  3

enum { DS_OFF, DS_DISCOVER, DS_REQUEST, DS_BOUND, DS_FAILED };

struct dhcp_pkt {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
} __attribute__((packed));

static int state = DS_OFF;
static int dhcp_sock = -1;
static uint32_t xid;
static uint64_t stage_deadline;
static int tries;
static uint32_t offer_ip;        // host order
static uint32_t server_id;       // host order

static const uint8_t dhcp_magic[4] = {99, 130, 83, 99};

static const uint8_t *opt_find(const uint8_t *o, int len, int code) {
    while (len > 0 && *o != DHCPO_END) {
        if (*o == 0) {           // pad
            o++;
            len--;
            continue;
        }
        if (len < 2)
            return 0;
        if (*o == code)
            return o;
        int step = 2 + o[1];
        if (step > len)
            return 0;
        o += step;
        len -= step;
    }
    return 0;
}

// discover or request, built into one static buffer
static void send_msg(int type) {
    static uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    struct dhcp_pkt *d = (struct dhcp_pkt *)buf;
    d->op = 1;                   // bootrequest
    d->htype = 1;                // ethernet
    d->hlen = 6;
    d->xid = xid;
    d->flags = 0x8000;           // broadcast reply, we have no ip yet
    memcpy(d->chaddr, net_hwaddr.b, 6);
    memcpy(d->magic, dhcp_magic, 4);

    uint8_t *o = buf + sizeof(*d);
    *o++ = DHCPO_MSGTYPE;
    *o++ = 1;
    *o++ = (uint8_t)type;
    if (type == DHCP_REQUEST) {
        *o++ = DHCPO_REQIP;
        *o++ = 4;
        uint32_t be = __builtin_bswap32(offer_ip);
        memcpy(o, &be, 4);
        o += 4;
        *o++ = DHCPO_SERVERID;
        *o++ = 4;
        uint32_t sbe = __builtin_bswap32(server_id);
        memcpy(o, &sbe, 4);
        o += 4;
    }
    *o++ = DHCPO_PARAMREQ;
    *o++ = 3;
    *o++ = DHCPO_SUBNET;
    *o++ = DHCPO_ROUTER;
    *o++ = 6;                    // dns, ignored but polite
    *o++ = DHCPO_END;
    uint16_t len = (uint16_t)(o - buf);
    net_sendto(dhcp_sock, buf, len, 0xffffffff, DHCP_PORT_SERVER);
}

static uint32_t get_ip(const uint8_t *opt) {
    uint32_t be;
    memcpy(&be, opt + 2, 4);
    return __builtin_bswap32(be);
}

static int put_octet(char *p, unsigned v) {
    if (v >= 100) {
        p[0] = (char)('0' + v / 100);
        p[1] = (char)('0' + v / 10 % 10);
        p[2] = (char)('0' + v % 10);
        return 3;
    }
    if (v >= 10) {
        p[0] = (char)('0' + v / 10);
        p[1] = (char)('0' + v % 10);
        return 2;
    }
    p[0] = (char)('0' + v);
    return 1;
}

static void print_ip(const char *pre, uint32_t ip, const char *post) {
    char line[64];
    int n = 0;
    while (*pre && n < (int)sizeof(line) - 1)
        line[n++] = *pre++;
    n += put_octet(line + n, ip >> 24);
    line[n++] = '.';
    n += put_octet(line + n, (ip >> 16) & 0xff);
    line[n++] = '.';
    n += put_octet(line + n, (ip >> 8) & 0xff);
    line[n++] = '.';
    n += put_octet(line + n, ip & 0xff);
    while (*post && n < (int)sizeof(line) - 1)
        line[n++] = *post++;
    line[n] = 0;
    console_puts(line);
}

void dhcp_start(void) {
    if (state != DS_OFF)
        return;
    dhcp_sock = net_socket(NET_PROTO_UDP);
    if (dhcp_sock < 0)
        return;
    if (net_bind(dhcp_sock, DHCP_PORT_CLIENT) < 0) {
        net_close(dhcp_sock);
        dhcp_sock = -1;
        return;
    }
    xid = (uint32_t)pit_ticks() * 2654435761u + 0x9e3779b9u;
    tries = 0;
    state = DS_DISCOVER;
    send_msg(DHCP_DISCOVER);
    stage_deadline = pit_ticks() + DHCP_RTO;
}

void dhcp_poll(void) {
    if (state != DS_DISCOVER && state != DS_REQUEST)
        return;

    // drain replies queued by the udp demux
    static uint8_t rb[600];
    uint32_t sip;
    uint16_t sport;
    for (;;) {
        long n = net_recvfrom(dhcp_sock, rb, sizeof(rb), &sip, &sport);
        if (n <= 0)
            break;               // ring drained
        if (n <= (long)sizeof(struct dhcp_pkt))
            continue;            // too short to be dhcp, keep draining
        struct dhcp_pkt *d = (struct dhcp_pkt *)rb;
        if (d->op != 2 || d->xid != xid)
            continue;
        if (memcmp(d->magic, dhcp_magic, 4) != 0)
            continue;
        const uint8_t *opts = rb + sizeof(struct dhcp_pkt);
        int olen = (int)n - (int)sizeof(struct dhcp_pkt);
        const uint8_t *mt = opt_find(opts, olen, DHCPO_MSGTYPE);
        if (!mt)
            continue;
        if (state == DS_DISCOVER && mt[2] == DHCP_OFFER) {
            const uint8_t *sid = opt_find(opts, olen, DHCPO_SERVERID);
            if (!sid)
                continue;
            offer_ip = __builtin_bswap32(d->yiaddr);
            server_id = get_ip(sid);
            tries = 0;
            state = DS_REQUEST;
            send_msg(DHCP_REQUEST);
            stage_deadline = pit_ticks() + DHCP_RTO;
        } else if (state == DS_REQUEST && mt[2] == DHCP_ACK) {
            net_local_ip = __builtin_bswap32(d->yiaddr);
            const uint8_t *sn = opt_find(opts, olen, DHCPO_SUBNET);
            const uint8_t *gw = opt_find(opts, olen, DHCPO_ROUTER);
            if (sn)
                net_netmask = get_ip(sn);
            if (gw)
                net_gw_ip = get_ip(gw);
            state = DS_BOUND;
            print_ip("[dhcp] lease ", net_local_ip, "\n");
            if (sn)
                print_ip("[dhcp] mask ", net_netmask, "\n");
            if (gw)
                print_ip("[dhcp] gw ", net_gw_ip, "\n");
        } else if (mt[2] == DHCP_NAK) {
            tries = DHCP_TRIES;  // force the timeout path below
        }
    }

    if (state != DS_DISCOVER && state != DS_REQUEST)
        return;

    // stage timeout: resend or give up (static ip stays)
    if (pit_ticks() >= stage_deadline) {
        if (++tries > DHCP_TRIES) {
            state = DS_FAILED;
            console_puts("[dhcp] no lease (timeout), keeping static ip\n");
            return;
        }
        send_msg(state == DS_DISCOVER ? DHCP_DISCOVER : DHCP_REQUEST);
        stage_deadline = pit_ticks() + DHCP_RTO;
    }
}
