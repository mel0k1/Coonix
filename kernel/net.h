// minimal network stack: arp, ipv4, icmp echo, udp, loopback
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

// ingress: one ip packet (ethernet header stripped by the nic driver);
// also serves the loopback short-circuit
void net_input(const uint8_t *pkt, uint16_t len);
// ingress: one arp packet (ethernet header stripped by the nic driver)
void net_arp_input(const uint8_t *pkt, uint16_t len);

// rx polling hook (timer tick)
void net_poll(void);

// -- sockets -------------------------------------------------------------
// proto: NET_PROTO_UDP (datagram) or NET_PROTO_ICMP (raw ping: sendto =
// echo request, recvfrom = echo replies matched by our id)
int net_socket(int proto);
int net_sendto(int fd, const void *buf, uint16_t len,
               uint32_t ip, uint16_t port);
// returns bytes copied, 0 = nothing queued yet, -1 bad fd/args
long net_recvfrom(int fd, void *buf, uint16_t len,
                  uint32_t *src_ip, uint16_t *src_port);
void net_close(int fd);
