// virtio-net legacy driver: registers tx/rx with the net stack
#pragma once
#include <stdint.h>

int virtio_net_init(void);
// drain the rx ring into the stack (tick context)
void virtio_net_poll(void);
