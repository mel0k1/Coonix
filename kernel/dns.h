// kernel dns stub: A-record resolution over the udp socket layer
#pragma once
#include <stdint.h>

// one non-blocking resolver step: drain pending udp replies (matched
// by question name, not id), then (re)send the query. returns
// 0 = resolved (out_ip filled), 1 = in progress (call again later),
// -1 = bad args/stack, -2 = name error (rcode 3).
// server = dns server, host order (0 = the configured net_dns_ip)
int dns_resolve(const char *name, uint32_t server, uint32_t *out_ip);

// create the persistent resolver socket (kmain, after net_init)
void dns_init(void);
