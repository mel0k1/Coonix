#include "virtio.h"
#include "virtio_net.h"
#include "net.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "string.h"
#include "console.h"

// legacy virtio-net: two queues (rx=0, tx=1), 12-byte net header in
// front of every frame, polling driven (no irq bookkeeping)

struct vnet_hdr {
    uint8_t flags;
    uint8_t gso;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

#define RXQ 0
#define TXQ 1
#define NBUF 16
#define BUFSZ 2048

static struct {
    uint16_t base;
    uint32_t qsize;
    uint64_t rx_phys, tx_phys;
    uint8_t *rx, *tx;
    uint16_t rx_avail, tx_avail;
    uint16_t rx_seen, tx_seen;
    // rx buffers: device writes frames here
    uint64_t rxbuf_phys[NBUF];
    uint8_t *rxbuf[NBUF];
    int rx_posted;
    // tx staging: one pm page holds header + outgoing frame (dma-able)
    uint64_t txbuf_phys;
    uint8_t *txbuf;
} vn;

static inline void vwr16(uint16_t off, uint16_t v) {
    __asm__ volatile("outw %w0, %w1" :: "a"(v), "Nd"(vn.base + off) : "memory");
}

static inline uint16_t vrd16(uint16_t off) {
    uint16_t v;
    __asm__ volatile("inw %w1, %w0" : "=a"(v) : "Nd"(vn.base + off) : "memory");
    return v;
}

static inline void vwr32(uint16_t off, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(vn.base + off) : "memory");
}

static inline void vwr8(uint16_t off, uint8_t v) {
    outb(vn.base + off, v);
}

static inline uint8_t vrd8(uint16_t off) {
    return inb(vn.base + off);
}

static void queue_pfn(uint16_t sel, uint64_t phys) {
    vwr16(VIRTIO_QUEUE_SEL, sel);
    vwr32(VIRTIO_QUEUE_PFN, (uint32_t)(phys >> 12));
}

static int rx_refill(void) {
    // (re)post every free rx buffer; the buffer for slot i lives at
    // rxbuf[i], so completions (used id = slot) find their buffer again
    volatile struct vring_desc *desc =
        (volatile struct vring_desc *)vn.rx;
    volatile struct vring_avail *avail =
        (volatile struct vring_avail *)(vn.rx + VRING_AVAIL_OFF(vn.qsize));
    for (int i = vn.rx_posted; i < NBUF; i++) {
        desc[i].addr = vn.rxbuf_phys[i];
        desc[i].len = BUFSZ;
        desc[i].flags = VRING_DESC_WRITE;   // device writes the buffer
        desc[i].next = 0;
        avail->ring[vn.rx_avail % vn.qsize] = (uint16_t)i;
        vn.rx_avail++;
        avail->idx = vn.rx_avail;
        __asm__ volatile("mfence" ::: "memory");
        vn.rx_posted++;
        vwr16(VIRTIO_QUEUE_NOTIFY, RXQ);
    }
    return 0;
}

static int vnet_send(const void *buf, uint16_t len) {
    if (!vn.base || len == 0 || len > NET_MTU)
        return -1;
    volatile struct vring_desc *desc =
        (volatile struct vring_desc *)vn.tx;
    volatile struct vring_avail *avail =
        (volatile struct vring_avail *)(vn.tx + VRING_AVAIL_OFF(vn.qsize));
    volatile struct vring_used *used =
        (volatile struct vring_used *)(vn.tx + VRING_USED_OFF(vn.qsize));

    // staging page: [vnet header | frame | status byte]
    struct vnet_hdr *hdr = (struct vnet_hdr *)vn.txbuf;
    uint8_t *frame = vn.txbuf + 0x100;
    uint8_t *status = vn.txbuf + 0x100 + len;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(frame, buf, len);
    *status = 0x77;

    uint16_t s0 = (uint16_t)(vn.tx_avail % vn.qsize);
    uint16_t s1 = (uint16_t)((s0 + 1) % vn.qsize);
    uint16_t s2 = (uint16_t)((s0 + 2) % vn.qsize);
    desc[s0].addr = vn.txbuf_phys;
    desc[s0].len = sizeof(struct vnet_hdr);
    desc[s0].flags = VRING_DESC_NEXT;
    desc[s0].next = s1;
    desc[s1].addr = vn.txbuf_phys + 0x100;
    desc[s1].len = len;
    desc[s1].flags = VRING_DESC_NEXT;
    desc[s1].next = s2;
    desc[s2].addr = vn.txbuf_phys + 0x100 + len;
    desc[s2].len = 1;
    desc[s2].flags = VRING_DESC_WRITE;
    desc[s2].next = 0;

    avail->ring[vn.tx_avail % vn.qsize] = s0;
    vn.tx_avail++;
    avail->idx = vn.tx_avail;
    __asm__ volatile("mfence" ::: "memory");
    vwr16(VIRTIO_QUEUE_NOTIFY, TXQ);

    uint64_t budget = 400000000;
    while (budget--) {
        if (used->idx != vn.tx_seen)
            break;
        vrd8(VIRTIO_ISR);
    }
    if (used->idx == vn.tx_seen)
        return -1;
    vn.tx_seen = used->idx;
    return (*status == 0) ? 0 : -1;
}

void virtio_net_poll(void) {
    if (!vn.base)
        return;
    volatile struct vring_used *used =
        (volatile struct vring_used *)(vn.rx + VRING_USED_OFF(vn.qsize));
    while (vn.rx_seen != used->idx) {
        volatile struct vring_used_elem *e =
            &used->ring[vn.rx_seen % vn.qsize];
        vn.rx_seen++;
        uint16_t slot = (uint16_t)(e->id & (vn.qsize - 1));
        uint8_t *buf = vn.rxbuf[slot];
        if (e->len > 12 && e->len <= BUFSZ) {
            // device wrote: 12-byte net header + the ethernet frame
            uint8_t *frame = buf + 12;
            uint16_t flen = (uint16_t)(e->len - 12);
            if (flen >= 14 && frame[12] == 0x08 && frame[13] == 0x06)
                net_arp_input(frame + 14, (uint16_t)(flen - 14));
            else if (flen >= 14 + NET_IP_LEN)
                net_input(frame + 14, (uint16_t)(flen - 14));
        }
        // repost this slot
        volatile struct vring_desc *desc =
            (volatile struct vring_desc *)vn.rx;
        volatile struct vring_avail *avail =
            (volatile struct vring_avail *)(vn.rx + VRING_AVAIL_OFF(vn.qsize));
        desc[slot].addr = vn.rxbuf_phys[slot];
        desc[slot].len = BUFSZ;
        desc[slot].flags = VRING_DESC_WRITE;
        desc[slot].next = 0;
        avail->ring[vn.rx_avail % vn.qsize] = slot;
        vn.rx_avail++;
        avail->idx = vn.rx_avail;
        __asm__ volatile("mfence" ::: "memory");
        vwr16(VIRTIO_QUEUE_NOTIFY, RXQ);
    }
}

int virtio_net_init(void) {
    uint8_t bus, dev, fn, irq;
    uint32_t bar0;
    if (!pci_find_vendor_device(VIRTIO_PCI_VENDOR, VIRTIO_DEV_NET_LEGACY,
                                &bus, &dev, &fn, &bar0, &irq) &&
        !pci_find_vendor_device(VIRTIO_PCI_VENDOR, VIRTIO_DEV_NET_MODERN,
                                &bus, &dev, &fn, &bar0, &irq))
        return -1;
    if (!(bar0 & 1))
        return -1;
    vn.base = (uint16_t)(bar0 & 0xfffc);
    vn.rx_avail = vn.tx_avail = vn.rx_seen = vn.tx_seen = 0;
    vn.rx_posted = 0;

    uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x7);

    vwr8(VIRTIO_STATUS, 0);
    vwr8(VIRTIO_STATUS, VIRTIOS_ACK | VIRTIOS_DRIVER);
    vwr32(VIRTIO_GUEST_FEATS, 0);

    vwr16(VIRTIO_QUEUE_SEL, RXQ);
    uint32_t qsize = vrd16(VIRTIO_QUEUE_NUM);
    if (!qsize || qsize > 1024)
        return -1;
    vn.qsize = qsize;

    void *rxring = pmm_alloc_contig(VRING_PAGES(qsize));
    void *txring = pmm_alloc_contig(VRING_PAGES(qsize));
    void *txbuf = pmm_alloc_zeroed();
    if (!rxring || !txring || !txbuf)
        return -1;
    vn.rx_phys = (uint64_t)rxring;
    vn.rx = phys2virt(vn.rx_phys);
    vn.tx_phys = (uint64_t)txring;
    vn.tx = phys2virt(vn.tx_phys);
    vn.txbuf_phys = (uint64_t)txbuf;
    vn.txbuf = phys2virt(vn.txbuf_phys);
    memset(vn.rx, 0, VRING_PAGES(qsize) * PAGE_SIZE);
    memset(vn.tx, 0, VRING_PAGES(qsize) * PAGE_SIZE);

    for (int i = 0; i < NBUF; i++) {
        void *p = pmm_alloc_zeroed();
        if (!p)
            return -1;
        vn.rxbuf_phys[i] = (uint64_t)p;
        vn.rxbuf[i] = phys2virt((uint64_t)p);
    }

    queue_pfn(RXQ, vn.rx_phys);
    queue_pfn(TXQ, vn.tx_phys);

    // mac from the device config (bar0+0x14)
    uint8_t mac[6];
    for (int i = 0; i < 6; i++)
        mac[i] = vrd8((uint16_t)(0x14 + i));
    for (int i = 0; i < 6; i++)
        net_hwaddr.b[i] = mac[i];

    vwr8(VIRTIO_STATUS, vrd8(VIRTIO_STATUS) | VIRTIOS_DRIVER_OK);

    rx_refill();

    net_nic.send = vnet_send;
    console_puts("net: virtio-net ");
    for (int i = 0; i < 6; i++) {
        if (i)
            console_putc(':');
        console_putc("0123456789abcdef"[mac[i] >> 4]);
        console_putc("0123456789abcdef"[mac[i] & 0xf]);
    }
    console_puts("\n");
    return 0;
}
