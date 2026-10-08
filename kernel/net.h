// minimal network stack: arp, ipv4, icmp echo, udp, tcp, loopback
#pragma once
#include <stdint.h>

#define NET_ETH_LEN     14
#define NET_IP_LEN      20
#define NET_UDP_LEN     8
#define NET_ICMP_LEN    8
#define NET_ARP_LEN     28
#define NET_MTU         1500

// protocols (netinet values)
#define NET_PROTO_ICMP  1
#define NET_PROTO_TCP   6
#define NET_PROTO_UDP   17

// icmp types
#define NET_ICMP_ECHO_REPLY   0
#define NET_ICMP_ECHO_REQUEST 8

struct net_mac { uint8_t b[6]; };

// our identity (set by the nic driver / defaults)
extern struct net_mac net_hwaddr;
extern uint32_t net_local_ip;    // host order
extern uint32_t net_gw_ip;       // host order
extern uint32_t net_netmask;     // host order

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

// one's complement over the pseudo header + payload; 0 = valid checksum
// on receive, the checksum itself when built with a zeroed field
uint16_t net_pseudo_cksum(uint32_t src, uint32_t dst, uint8_t proto,
                          const void *payload, uint16_t plen);

// fresh ephemeral local port
uint16_t net_next_port(void);

// ingress: one ip packet (ethernet header stripped by the nic driver);
// also serves the loopback short-circuit
void net_input(const uint8_t *pkt, uint16_t len);
// ingress: one arp packet (ethernet header stripped by the nic driver)
void net_arp_input(const uint8_t *pkt, uint16_t len);

// rx polling hook (timer tick)
void net_poll(void);

// -- sockets -------------------------------------------------------------
// proto: NET_PROTO_UDP (datagram), NET_PROTO_ICMP (raw ping) or
// NET_PROTO_TCP (stream: connect/listen/accept below)
int net_socket(int proto);
int net_sendto(int fd, const void *buf, uint16_t len,
               uint32_t ip, uint16_t port);
// returns bytes copied, 0 = nothing queued yet, -1 bad fd/args
long net_recvfrom(int fd, void *buf, uint16_t len,
                  uint32_t *src_ip, uint16_t *src_port);
void net_close(int fd);

// tcp extensions (linux-errno flavored)
int net_bind(int fd, uint16_t port);
int net_listen(int fd, int backlog);
int net_connect(int fd, uint32_t ip, uint16_t port);
// returns a new fd for an established connection, -1 = none pending
long net_accept(int fd, uint32_t *ip, uint16_t *port);
