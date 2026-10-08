// minimal network stack: arp, ipv4, ipv6, icmp, icmpv6+nd, udp, tcp
#pragma once
#include <stdint.h>

#define NET_ETH_LEN     14
#define NET_IP_LEN      20
#define NET_IP6_LEN     40
#define NET_ICMP6_LEN   8
#define NET_UDP_LEN     8
#define NET_ICMP_LEN    8
#define NET_ARP_LEN     28
#define NET_MTU         1500

// protocols (netinet values)
#define NET_PROTO_ICMP  1
#define NET_PROTO_TCP   6
#define NET_PROTO_UDP   17

// ipv6 next-header values
#define NET_NH_TCP      6
#define NET_NH_UDP      17
#define NET_NH_ICMP6    58

// icmp types
#define NET_ICMP_ECHO_REPLY   0
#define NET_ICMP_ECHO_REQUEST 8
#define NET_ICMP6_ECHO_REQUEST 128
#define NET_ICMP6_ECHO_REPLY   129
#define NET_ICMP6_NS    135
#define NET_ICMP6_NA    136

// address families
#define NET_AF_INET4    0
#define NET_AF_INET6    1

struct net_mac { uint8_t b[6]; };

// our identity (set by the nic driver / defaults)
extern struct net_mac net_hwaddr;
extern uint32_t net_local_ip;    // host order
extern uint32_t net_gw_ip;       // host order
extern uint32_t net_netmask;     // host order
extern uint32_t net_dns_ip;      // host order (dhcp option 6 / slirp default)
extern uint8_t net_local_ip6[16];        // link-local fe80::/64 eui-64
extern const uint8_t net_ip6_loopback[16];   // ::1
extern const uint8_t net_ip6_any[16];        // :: (unspecified)

int net_ip6_eq(const uint8_t *a, const uint8_t *b);
int net_ip6_is_loopback(const uint8_t *a);   // ::1

void net_init(void);
// nic driver hooks (virtio_net.c registers these)
struct net_nic {
    int (*send)(const void *buf, uint16_t len);   // 0 ok
    void *ctx;
};
extern struct net_nic net_nic;

// outgoing packet from the stack (ethernet-framed if a nic is present)
void net_stack_output(const void *buf, uint16_t len, uint32_t dst_ip);

// outgoing ip packet (ethernet-framed if a nic is present; loopback
// short-circuits back into net_input). fills the udp/tcp pseudo checksum
void net_ip_output(const void *payload, uint16_t plen, uint8_t proto,
                   uint32_t dst);

// outgoing ipv6 packet; same contract as net_ip_output, checksums
// patched for nh = tcp/udp/icmpv6
void net_ip6_output(const void *payload, uint16_t plen, uint8_t nh,
                    const uint8_t *dst6);

// one's complement over the pseudo header + payload; 0 = valid checksum
// on receive, the checksum itself when built with a zeroed field
uint16_t net_pseudo_cksum(uint32_t src, uint32_t dst, uint8_t proto,
                          const void *payload, uint16_t plen);
// ipv6 pseudo header variant (32 bytes of addresses)
uint16_t net_pseudo6_cksum(const uint8_t *src6, const uint8_t *dst6,
                           uint8_t nh, const void *payload, uint16_t plen);

// fresh ephemeral local port
uint16_t net_next_port(void);
// protocol of a socket slot (NET_PROTO_*), -1 = bad fd
int net_proto(int fd);
// address family of a socket slot (NET_AF_*), -1 = bad fd
int net_sock_af(int fd);
// limited getsockopt (SOL_SOCKET SO_ERROR/SO_TYPE, IPPROTO_TCP TCP_INFO);
// val is a kernel buffer, 0 ok / -1 unsupported
long net_getsockopt(int fd, int level, int opt, void *val, uint32_t vlen);

// ingress: one ip packet (ethernet header stripped by the nic driver);
// also serves the loopback short-circuit
void net_input(const uint8_t *pkt, uint16_t len);
// ingress: one arp packet (ethernet header stripped by the nic driver)
void net_arp_input(const uint8_t *pkt, uint16_t len);

// rx polling hook (timer tick)
void net_poll(void);

// -- sockets -------------------------------------------------------------
// proto: NET_PROTO_UDP (datagram), NET_PROTO_ICMP (raw ping) or
// NET_PROTO_TCP (stream: connect/listen/accept below).
// a socket carries an address family: v4 by default, flipped to v6 by
// the *6 entry points below (bind6/connect6/sendto6)
int net_socket(int proto);
int net_sendto(int fd, const void *buf, uint16_t len,
               uint32_t ip, uint16_t port);
int net_sendto6(int fd, const void *buf, uint16_t len,
                const uint8_t *ip6, uint16_t port);
// returns bytes copied, 0 = nothing queued yet, -1 bad fd/args
long net_recvfrom(int fd, void *buf, uint16_t len,
                  uint32_t *src_ip, uint16_t *src_port);
long net_recvfrom6(int fd, void *buf, uint16_t len,
                   uint8_t *src6, uint16_t *src_port);
void net_close(int fd);

// tcp extensions (linux-errno flavored)
int net_bind(int fd, uint16_t port);
int net_bind6(int fd, uint16_t port);
int net_listen(int fd, int backlog);
int net_connect(int fd, uint32_t ip, uint16_t port);
int net_connect6(int fd, const uint8_t *ip6, uint16_t port);
// returns a new fd for an established connection, -1 = none pending
long net_accept(int fd, uint32_t *ip, uint16_t *port);
// v6 variant: peer address is 16 bytes, the returned slot is v6
long net_accept6(int fd, uint8_t *ip6, uint16_t *port);
