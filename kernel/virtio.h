// legacy (transitional) virtio over pci bar0 io ports
#pragma once
#include <stdint.h>

#define VIRTIO_PCI_VENDOR 0x1af4

// legacy pci device ids (modern transitional devices report 0x1040+id)
#define VIRTIO_DEV_NET_LEGACY 0x1000
#define VIRTIO_DEV_BLK_LEGACY 0x1001
#define VIRTIO_DEV_NET_MODERN 0x1041
#define VIRTIO_DEV_BLK_MODERN 0x1042

// common config offsets on bar0 (legacy layout)
#define VIRTIO_HOST_FEATS   0x00
#define VIRTIO_GUEST_FEATS  0x04
#define VIRTIO_QUEUE_PFN    0x08
#define VIRTIO_QUEUE_NUM    0x0c
#define VIRTIO_QUEUE_SEL    0x0e
#define VIRTIO_QUEUE_NOTIFY 0x10
#define VIRTIO_STATUS       0x12
#define VIRTIO_ISR          0x13

#define VIRTIOS_ACK       1
#define VIRTIOS_DRIVER    2
#define VIRTIOS_DRIVER_OK 4
#define VIRTIOS_FAILED    128

// vring descriptor flags
#define VRING_DESC_NEXT  1
#define VRING_DESC_WRITE 2   // device writes this buffer (guest reads)

// split vring, legacy way: desc table at 0, avail ring after it,
// used ring page-aligned behind avail (page size 4096)
#define VRING_AVAIL_OFF(q) (16 * (q))
#define VRING_USED_OFF(q)  ((16 * (q) + 6 + 2 * (q) + 4095) & ~4095ULL)
#define VRING_PAGES(q)     (((VRING_USED_OFF(q) + 4 + 8 * (q)) + \
                            4095) / 4096)
