// minimal tcp: reliable byte streams over the net stack
// non-blocking api, tick-driven retransmit, loopback-aware
#pragma once
#include <stdint.h>

#define NET_TCP_LEN     20

// tcp header flags
#define TCPF_FIN        0x01
#define TCPF_SYN        0x02
#define TCPF_RST        0x04
#define TCPF_PSH        0x08
#define TCPF_ACK        0x10

// errno values the tcp api returns (linux numbers, negative)
#define TCP_EBADFD      -9
#define TCP_EAGAIN      -11
#define TCP_EPIPE       -32
#define TCP_ECONNRESET  -104
#define TCP_EISCONN     -106
#define TCP_ENOTCONN    -107
#define TCP_ETIMEDOUT   -110
#define TCP_EALREADY    -114
#define TCP_EINPROGRESS -115

// states
enum {
    TS_CLOSED = 0,
    TS_LISTEN,
    TS_SYN_SENT,
    TS_SYN_RX,
    TS_ESTABLISHED,
    TS_FIN_WAIT1,
    TS_FIN_WAIT2,
    TS_CLOSE_WAIT,
    TS_LAST_ACK,
    TS_CLOSING,
    TS_TIME_WAIT,
};

// getsockopt(TCP_INFO) subset (coonix flavor, not linux's big struct)
struct tcp_info_k {
    uint8_t state;               // enum above
    uint8_t pad[3];
    uint32_t cwnd;               // congestion window, bytes
    uint32_t ssthresh;           // slow-start threshold, bytes
    uint16_t mss;
    uint16_t rto;                // retransmit timeout, ticks
};

void tcp_init(void);
void *tcp_alloc(void);                 // fresh socket, 0 = table full
void tcp_destroy(void *p);             // close + release resources
int tcp_bind(void *p, uint16_t port);
int tcp_listen(void *p, int backlog);
int tcp_connect(void *p, uint32_t ip, uint16_t port);
int tcp_connect6(void *p, const uint8_t *ip6, uint16_t port);
// returns an established child, fills peer, 0 = none pending yet
void *tcp_accept(void *p, uint32_t *ip, uint16_t *port);
// returns bytes buffered (0 = full), < 0 = errno
long tcp_send(void *p, const void *buf, uint16_t len);
// returns bytes copied, 0 = eof, -EAGAIN = no data, < 0 = errno
long tcp_recv(void *p, void *buf, uint16_t len);
int tcp_peer(void *p, uint32_t *ip, uint16_t *port);
int tcp_peer6(void *p, uint8_t *ip6, uint16_t *port);
// pending error, read-and-clear (0 = none)
int tcp_so_error(void *p);
// snapshot for getsockopt(TCP_INFO), 0 ok
int tcp_info(void *p, struct tcp_info_k *out);

// ingress: one tcp segment (ip header stripped), host-order addrs
void tcp_input(const uint8_t *seg, uint16_t len,
               uint32_t src, uint32_t dst);
// ingress: one ipv6 tcp segment; src6/dst6 big endian as on the wire
void tcp_input6(const uint8_t *seg, uint16_t len,
                const uint8_t *src6, const uint8_t *dst6);
// tick hook: retransmits, handshake timers, time_wait expiry
void tcp_poll(void);
