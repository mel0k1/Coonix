#include "tcp.h"
#include "net.h"
#include "pit.h"
#include "string.h"

// one tcp connection per slot, static buffers (no irq-context alloc).
// all api is non-blocking; the timer tick drives retransmits, handshake
// timers and time_wait expiry. loopback short-circuits synchronously
// through net_stack_output, so handshake + echo complete in one call.

#define TCP_MAX      8
#define TCP_SNDBUF   4096
#define TCP_RCVBUF   8192
#define TCP_MSS      1200
#define TCP_BACKLOG  4
#define TCP_RTO      30          // ticks
#define TCP_MAXRET   5
#define TCP_TW       200         // time_wait ticks (2s)

struct tcpsk {
    int used, owned, state;
    uint32_t peer_ip;
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, rcv_nxt;
    uint16_t snd_wnd, adv_wnd;
    uint16_t snd_len, rcv_len;
    uint8_t sndbuf[TCP_SNDBUF];
    uint8_t rcvbuf[TCP_RCVBUF];
    uint8_t fin_req, fin_sent, fin_acked, fin_rx, got_syn;
    uint64_t tx_tick, tw_tick;
    int retries;
    struct tcpsk *parent;
    struct tcpsk *queue[TCP_BACKLOG];
    int qlen;
};

static struct tcpsk tcp_tab[TCP_MAX];

// wrap-safe sequence compares (rfc 793)
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)

static uint32_t next_isn(void) {
    static uint32_t isn;
    isn = isn * 1103515245u + 12345u + (uint32_t)pit_ticks();
    return isn;
}

static int data_state(int st) {
    return st == TS_ESTABLISHED || st == TS_FIN_WAIT1 ||
           st == TS_FIN_WAIT2 || st == TS_CLOSE_WAIT || st == TS_LAST_ACK ||
           st == TS_CLOSING;
}

// -- tx ------------------------------------------------------------------

// build header + hand to ip; the ip layer fills the pseudo checksum
static void tx_seg(struct tcpsk *sk, uint32_t seq, uint8_t flags,
                   const void *payload, uint16_t len) {
    uint8_t pkt[NET_TCP_LEN + TCP_MSS];
    if (len > TCP_MSS)
        len = TCP_MSS;
    pkt[0] = (uint8_t)(sk->lport >> 8);
    pkt[1] = (uint8_t)sk->lport;
    pkt[2] = (uint8_t)(sk->rport >> 8);
    pkt[3] = (uint8_t)sk->rport;
    pkt[4] = (uint8_t)(seq >> 24);
    pkt[5] = (uint8_t)(seq >> 16);
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)seq;
    uint32_t ack = sk->got_syn ? sk->rcv_nxt : 0;
    pkt[8] = (uint8_t)(ack >> 24);
    pkt[9] = (uint8_t)(ack >> 16);
    pkt[10] = (uint8_t)(ack >> 8);
    pkt[11] = (uint8_t)ack;
    pkt[12] = 0x50;                  // doff 5
    pkt[13] = flags;
    uint16_t wnd = (uint16_t)(TCP_RCVBUF - sk->rcv_len);
    sk->adv_wnd = wnd;
    pkt[14] = (uint8_t)(wnd >> 8);
    pkt[15] = (uint8_t)wnd;
    pkt[16] = pkt[17] = 0;           // checksum (ip layer)
    pkt[18] = pkt[19] = 0;           // urgent
    if (len)
        memcpy(pkt + NET_TCP_LEN, payload, len);
    net_ip_output(pkt, (uint16_t)(NET_TCP_LEN + len), NET_PROTO_TCP,
                  sk->peer_ip);
}

// rst with no socket context
static void tx_rst(uint16_t lport, uint16_t rport, uint32_t seq,
                   uint32_t ack, uint32_t dst) {
    uint8_t pkt[NET_TCP_LEN];
    pkt[0] = (uint8_t)(lport >> 8);
    pkt[1] = (uint8_t)lport;
    pkt[2] = (uint8_t)(rport >> 8);
    pkt[3] = (uint8_t)rport;
    pkt[4] = (uint8_t)(seq >> 24);
    pkt[5] = (uint8_t)(seq >> 16);
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)seq;
    pkt[8] = (uint8_t)(ack >> 24);
    pkt[9] = (uint8_t)(ack >> 16);
    pkt[10] = (uint8_t)(ack >> 8);
    pkt[11] = (uint8_t)ack;
    pkt[12] = 0x50;
    pkt[13] = TCPF_RST | TCPF_ACK;
    pkt[14] = pkt[15] = pkt[16] = pkt[17] = pkt[18] = pkt[19] = 0;
    net_ip_output(pkt, NET_TCP_LEN, NET_PROTO_TCP, dst);
}

static void send_ack(struct tcpsk *sk) {
    tx_seg(sk, sk->snd_nxt, sk->got_syn ? TCPF_ACK : 0, 0, 0);
}

// (re)transmit scheduler: rto backoff, new data within the peer window,
// then the pending fin
static void tcp_output(struct tcpsk *sk) {
    if (!sk->used || sk->state == TS_CLOSED || sk->state == TS_LISTEN)
        return;
    uint64_t now = pit_ticks();

    if (sk->state == TS_SYN_SENT || sk->state == TS_SYN_RX) {
        if (now - sk->tx_tick >= TCP_RTO) {
            if (++sk->retries > TCP_MAXRET) {
                sk->state = TS_CLOSED;
                return;
            }
            tx_seg(sk, sk->iss,
                   (uint8_t)(sk->state == TS_SYN_SENT ? TCPF_SYN
                                                      : TCPF_SYN | TCPF_ACK),
                   0, 0);
            sk->tx_tick = now;
        }
        return;
    }

    // retransmit the oldest unacked segment (go-back-n)
    if (sk->snd_una != sk->snd_nxt && now - sk->tx_tick >= TCP_RTO) {
        if (++sk->retries > TCP_MAXRET) {
            sk->state = TS_CLOSED;
            return;
        }
        if (sk->fin_sent && !sk->fin_acked && sk->snd_len == 0 &&
            sk->snd_nxt == sk->snd_una + 1) {
            // only the fin is unacked
            tx_seg(sk, sk->snd_una, (uint8_t)(TCPF_ACK | TCPF_FIN), 0, 0);
        } else {
            uint16_t n = (uint16_t)(sk->snd_nxt - sk->snd_una);
            if (n > TCP_MSS)
                n = TCP_MSS;
            if (n > sk->snd_len)
                n = sk->snd_len;
            if (n)
                tx_seg(sk, sk->snd_una, TCPF_ACK | TCPF_PSH, sk->sndbuf, n);
            else if (sk->fin_sent && !sk->fin_acked)
                tx_seg(sk, sk->snd_una, (uint8_t)(TCPF_ACK | TCPF_FIN), 0, 0);
        }
        sk->tx_tick = now;
    }

    // fresh data, bounded by the advertised peer window
    while (SEQ_LT(sk->snd_nxt, sk->snd_una + sk->snd_len) &&
           sk->snd_nxt - sk->snd_una < sk->snd_wnd) {
        uint32_t off = sk->snd_nxt - sk->snd_una;
        uint32_t inflight = sk->snd_nxt - sk->snd_una;
        uint32_t room = sk->snd_wnd - inflight;
        uint32_t n = sk->snd_len - off;
        if (n > TCP_MSS)
            n = TCP_MSS;
        if (n > room)
            n = room;
        if (!n)
            break;
        tx_seg(sk, sk->snd_nxt, TCPF_ACK | TCPF_PSH, sk->sndbuf + off,
               (uint16_t)n);
        sk->snd_nxt += n;
        sk->tx_tick = now;
        sk->retries = 0;
    }

    // fin after all data
    if (sk->fin_req && !sk->fin_sent &&
        sk->snd_nxt == sk->snd_una + sk->snd_len &&
        sk->snd_nxt - sk->snd_una < sk->snd_wnd) {
        tx_seg(sk, sk->snd_nxt, (uint8_t)(TCPF_ACK | TCPF_FIN), 0, 0);
        sk->fin_sent = 1;
        sk->snd_nxt++;
        sk->tx_tick = now;
        if (sk->state == TS_ESTABLISHED)
            sk->state = TS_FIN_WAIT1;
        else if (sk->state == TS_CLOSE_WAIT)
            sk->state = TS_LAST_ACK;
    }

    // fin acked: wait for the peer fin (bounded, or we leak the slot)
    if (sk->state == TS_FIN_WAIT1 && sk->fin_acked) {
        sk->state = TS_FIN_WAIT2;
        sk->tw_tick = now;
    }
}

// -- rx ------------------------------------------------------------------

static void reset_sk(struct tcpsk *sk) {
    sk->state = TS_CLOSED;
    sk->fin_req = sk->fin_sent = sk->fin_acked = sk->fin_rx = 0;
    sk->snd_len = sk->rcv_len = 0;
    sk->parent = 0;
    sk->qlen = 0;
}

static void enqueue_child(struct tcpsk *parent, struct tcpsk *child) {
    if (parent->qlen >= TCP_BACKLOG) {
        tx_rst(child->lport, child->rport, child->snd_nxt, child->rcv_nxt,
               child->peer_ip);
        child->used = 0;
        return;
    }
    parent->queue[parent->qlen++] = child;
}

static void dequeue_child(struct tcpsk *parent, struct tcpsk *child) {
    for (int i = 0; i < parent->qlen; i++) {
        if (parent->queue[i] == child) {
            parent->queue[i] = parent->queue[--parent->qlen];
            return;
        }
    }
}

void tcp_input(const uint8_t *seg, uint16_t len,
               uint32_t src, uint32_t dst) {
    (void)dst;
    if (len < NET_TCP_LEN)
        return;
    if (net_pseudo_cksum(src, dst, NET_PROTO_TCP, seg, len) != 0)
        return;                      // bad checksum
    uint16_t sport = (uint16_t)((seg[0] << 8) | seg[1]);
    uint16_t dport = (uint16_t)((seg[2] << 8) | seg[3]);
    uint32_t seq = ((uint32_t)seg[4] << 24) | ((uint32_t)seg[5] << 16) |
                   ((uint32_t)seg[6] << 8) | seg[7];
    uint32_t ackn = ((uint32_t)seg[8] << 24) | ((uint32_t)seg[9] << 16) |
                    ((uint32_t)seg[10] << 8) | seg[11];
    uint8_t doff = (uint8_t)(seg[12] >> 4);
    uint8_t flags = (uint8_t)(seg[13] & 0x3f);
    uint16_t wnd = (uint16_t)((seg[14] << 8) | seg[15]);
    if (doff < 5 || (uint16_t)(doff * 4) > len)
        return;
    const uint8_t *payload = seg + doff * 4;
    uint16_t plen = (uint16_t)(len - doff * 4);

    // exact match first, then a listener for fresh syns
    struct tcpsk *sk = 0;
    for (int i = 0; i < TCP_MAX && !sk; i++) {
        struct tcpsk *t = &tcp_tab[i];
        if (t->used && t->state != TS_LISTEN && t->lport == dport &&
            t->rport == sport && t->peer_ip == src)
            sk = t;
    }
    if (!sk && (flags & TCPF_SYN) && !(flags & TCPF_ACK)) {
        for (int i = 0; i < TCP_MAX && !sk; i++) {
            struct tcpsk *t = &tcp_tab[i];
            if (t->used && t->state == TS_LISTEN && t->lport == dport)
                sk = t;
        }
        if (sk) {
            // spawn the child socket
            struct tcpsk *ch = tcp_alloc();
            if (!ch)
                return;
            ch->peer_ip = src;
            ch->rport = sport;
            ch->lport = dport;
            ch->parent = sk;
            ch->got_syn = 1;
            ch->rcv_nxt = seq + 1;
            ch->iss = next_isn();
            ch->snd_una = ch->iss;
            ch->snd_nxt = ch->iss;
            ch->snd_wnd = wnd;
            tx_seg(ch, ch->iss, (uint8_t)(TCPF_SYN | TCPF_ACK), 0, 0);
            ch->snd_nxt = ch->iss + 1;
            ch->tx_tick = pit_ticks();
            ch->state = TS_SYN_RX;
            enqueue_child(sk, ch);
            if (!ch->used)           // backlog overflow: we rst'd it
                return;
            return;
        }
    }
    if (!sk) {
        // no socket: rst unless the peer is rsting already
        if (!(flags & TCPF_RST))
            tx_rst(dport, sport,
                   (flags & TCPF_ACK) ? ackn : 0, seq + plen + 1, dst);
        return;
    }
    if (flags & TCPF_RST) {
        reset_sk(sk);
        return;
    }

    // -- handshake --
    if (sk->state == TS_SYN_SENT) {
        if ((flags & TCPF_SYN) && (flags & TCPF_ACK) && ackn == sk->iss + 1) {
            sk->rcv_nxt = seq + 1;
            sk->got_syn = 1;
            sk->snd_una = ackn;
            sk->snd_wnd = wnd;
            sk->state = TS_ESTABLISHED;
            send_ack(sk);
        }
        return;
    }

    // -- data + ack (established-ish states) --
    int need_ack = 0;
    if (plen) {
        if (seq == sk->rcv_nxt) {
            uint16_t space = (uint16_t)(TCP_RCVBUF - sk->rcv_len);
            uint16_t n = plen < space ? plen : space;
            if (n) {
                memcpy(sk->rcvbuf + sk->rcv_len, payload, n);
                sk->rcv_len += n;
                sk->rcv_nxt += n;
                need_ack = 1;
            }
            if ((flags & TCPF_FIN) && n == plen &&
                seq + plen == sk->rcv_nxt) {
                sk->fin_rx = 1;
                sk->rcv_nxt++;
                need_ack = 1;
                if (sk->state == TS_ESTABLISHED)
                    sk->state = TS_CLOSE_WAIT;
                else if (sk->state == TS_FIN_WAIT2) {
                    sk->state = TS_TIME_WAIT;
                    sk->tw_tick = pit_ticks();
                } else if (sk->state == TS_FIN_WAIT1)
                    sk->state = TS_CLOSING;
            }
        } else {
            // old or future data: re-ack with the current rcv_nxt
            need_ack = 1;
        }
    } else if (flags & TCPF_FIN) {
        if (seq == sk->rcv_nxt) {
            sk->fin_rx = 1;
            sk->rcv_nxt++;
            need_ack = 1;
            if (sk->state == TS_ESTABLISHED)
                sk->state = TS_CLOSE_WAIT;
            else if (sk->state == TS_FIN_WAIT2) {
                sk->state = TS_TIME_WAIT;
                sk->tw_tick = pit_ticks();
            }
        } else
            need_ack = 1;
    }

    if (flags & TCPF_ACK) {
        if (SEQ_LT(sk->snd_una, ackn) && SEQ_LEQ(ackn, sk->snd_nxt)) {
            uint32_t adv = ackn - sk->snd_una;
            if (adv > sk->snd_len)
                adv = sk->snd_len;
            if (adv && sk->snd_len - adv)
                memmove(sk->sndbuf, sk->sndbuf + adv,
                        (uint16_t)(sk->snd_len - adv));
            sk->snd_len = (uint16_t)(sk->snd_len - adv);
            sk->snd_una = ackn;
            sk->retries = 0;
            sk->tx_tick = pit_ticks();
            if (sk->fin_sent && ackn == sk->snd_nxt)
                sk->fin_acked = 1;
            if (sk->state == TS_SYN_RX)
                sk->state = TS_ESTABLISHED;
            else if (sk->state == TS_FIN_WAIT1 && sk->fin_acked) {
                sk->state = TS_FIN_WAIT2;
                sk->tw_tick = pit_ticks();
            } else if (sk->state == TS_CLOSING && sk->fin_acked) {
                sk->state = TS_TIME_WAIT;
                sk->tw_tick = pit_ticks();
            } else if (sk->state == TS_LAST_ACK && sk->fin_acked)
                sk->state = TS_CLOSED;
            need_ack = 0;            // acked by the ack itself
        }
        sk->snd_wnd = wnd;
    }

    if (need_ack && data_state(sk->state))
        send_ack(sk);

    // data may have freed window space or an ack unlocked the queue
    if (data_state(sk->state))
        tcp_output(sk);
}

// -- api (all called under the net lock) ---------------------------------

void tcp_init(void) {
    memset(tcp_tab, 0, sizeof(tcp_tab));
}

void *tcp_alloc(void) {
    struct tcpsk *sk = 0;
    for (int i = 0; i < TCP_MAX && !sk; i++)
        if (!tcp_tab[i].used)
            sk = &tcp_tab[i];
    // reap dead sockets whose owner never closed the fd
    for (int i = 0; i < TCP_MAX && !sk; i++)
        if (tcp_tab[i].used && !tcp_tab[i].owned &&
            tcp_tab[i].state == TS_CLOSED)
            sk = &tcp_tab[i];
    if (!sk)
        return 0;
    memset(sk, 0, sizeof(*sk));
    sk->used = 1;
    sk->owned = 1;
    sk->state = TS_CLOSED;
    sk->lport = net_next_port();
    sk->snd_wnd = TCP_SNDBUF;        // optimistic until the peer answers
    return sk;
}

// fd close: start teardown if live; buffers stay until the handshake
// drains (tcp_poll reaps when done)
void tcp_destroy(void *p) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used)
        return;
    sk->owned = 0;
    if (sk->state == TS_LISTEN) {
        // drop queued children the app never accepted
        for (int i = 0; i < sk->qlen; i++) {
            struct tcpsk *ch = sk->queue[i];
            if (ch && !ch->owned)
                ch->used = 0;
        }
        sk->qlen = 0;
        sk->used = 0;
        return;
    }
    if (sk->state == TS_ESTABLISHED || sk->state == TS_SYN_SENT ||
        sk->state == TS_CLOSE_WAIT) {
        sk->fin_req = 1;
        if (sk->state == TS_CLOSE_WAIT)
            sk->state = TS_LAST_ACK;
        else if (sk->state == TS_SYN_SENT) {
            sk->used = 0;
            return;
        } else
            sk->state = TS_FIN_WAIT1;
        tcp_output(sk);
        return;
    }
    if (sk->state == TS_CLOSED || sk->state == TS_TIME_WAIT)
        sk->used = 0;
}

int tcp_bind(void *p, uint16_t port) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used || sk->state != TS_CLOSED)
        return -1;
    sk->lport = port;
    return 0;
}

int tcp_listen(void *p, int backlog) {
    struct tcpsk *sk = p;
    (void)backlog;                   // fixed TCP_BACKLOG
    if (!sk || !sk->used || sk->state != TS_CLOSED)
        return -1;
    sk->state = TS_LISTEN;
    return 0;
}

int tcp_connect(void *p, uint32_t ip, uint16_t port) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used)
        return TCP_EBADFD;
    if (sk->state == TS_LISTEN)
        return TCP_EISCONN;
    if (sk->state == TS_ESTABLISHED)
        return 0;                    // handshake done since last call
    if (sk->state == TS_SYN_SENT)
        return TCP_EALREADY;
    if (sk->state == TS_CLOSED && sk->peer_ip)
        return TCP_ETIMEDOUT;        // previous attempt failed
    sk->peer_ip = ip;
    sk->rport = port;
    sk->iss = next_isn();
    sk->snd_una = sk->iss;
    sk->snd_nxt = sk->iss;
    tx_seg(sk, sk->iss, TCPF_SYN, 0, 0);
    sk->snd_nxt = sk->iss + 1;
    sk->tx_tick = pit_ticks();
    sk->retries = 0;
    sk->state = TS_SYN_SENT;
    return TCP_EINPROGRESS;
}

void *tcp_accept(void *p, uint32_t *ip, uint16_t *port) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used || sk->state != TS_LISTEN)
        return 0;
    for (int i = 0; i < sk->qlen; i++) {
        struct tcpsk *ch = sk->queue[i];
        if (ch && ch->state == TS_ESTABLISHED) {
            dequeue_child(sk, ch);
            if (ip)
                *ip = ch->peer_ip;
            if (port)
                *port = ch->rport;
            return ch;
        }
    }
    return 0;
}

long tcp_send(void *p, const void *buf, uint16_t len) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used)
        return TCP_EBADFD;
    if (sk->state == TS_CLOSED)
        return TCP_ECONNRESET;
    if (sk->state == TS_LISTEN || sk->state == TS_SYN_RX ||
        sk->state == TS_SYN_SENT || !data_state(sk->state))
        return TCP_ENOTCONN;
    if (sk->fin_req)
        return TCP_EPIPE;
    uint16_t space = (uint16_t)(TCP_SNDBUF - sk->snd_len);
    uint16_t n = len < space ? len : space;
    if (n) {
        memcpy(sk->sndbuf + sk->snd_len, buf, n);
        sk->snd_len += n;
        tcp_output(sk);
    }
    return n;                        // 0 = full, poll again
}

long tcp_recv(void *p, void *buf, uint16_t len) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used)
        return TCP_EBADFD;
    if (sk->state == TS_CLOSED)
        return TCP_ECONNRESET;
    if (sk->rcv_len) {
        uint16_t n = len < sk->rcv_len ? len : sk->rcv_len;
        memcpy(buf, sk->rcvbuf, n);
        memmove(sk->rcvbuf, sk->rcvbuf + n, (uint16_t)(sk->rcv_len - n));
        sk->rcv_len -= n;
        // reopen a previously closed window
        if (sk->adv_wnd == 0)
            send_ack(sk);
        return n;
    }
    if (sk->fin_rx)
        return 0;                    // eof
    return TCP_EAGAIN;
}

int tcp_peer(void *p, uint32_t *ip, uint16_t *port) {
    struct tcpsk *sk = p;
    if (!sk || !sk->used)
        return -1;
    if (ip)
        *ip = sk->peer_ip;
    if (port)
        *port = sk->rport;
    return 0;
}

void tcp_poll(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcpsk *sk = &tcp_tab[i];
        if (!sk->used)
            continue;
        if (sk->state == TS_TIME_WAIT &&
            now - sk->tw_tick >= TCP_TW) {
            sk->state = TS_CLOSED;
            if (!sk->owned)
                sk->used = 0;
            continue;
        }
        // fin_wait2 without a peer fin: bounded wait, then close
        if (sk->state == TS_FIN_WAIT2 &&
            now - sk->tw_tick >= 3 * TCP_TW) {
            sk->state = TS_CLOSED;
            if (!sk->owned)
                sk->used = 0;
            continue;
        }
        if (sk->state == TS_CLOSED) {
            if (!sk->owned)
                sk->used = 0;
            continue;
        }
        tcp_output(sk);
    }
}
