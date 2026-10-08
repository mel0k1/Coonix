// boot-time dhcp lease: discover -> offer -> request -> ack
#pragma once

// kick the state machine off once the nic is up (task context)
void dhcp_start(void);
// tick hook, runs under the net lock from net_poll
void dhcp_poll(void);
