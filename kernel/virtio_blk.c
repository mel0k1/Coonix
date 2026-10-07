#include "virtio.h"
#include "virtio_blk.h"
#include "pci.h"
#include "blkdev.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "console.h"
#include "kernel.h"

// legacy virtio-blk: bar0 io interface, one queue, one in-flight
// request, 8-sector bounce chunks (same shape as the ahci driver)

#define VBLK_T_IN  1   // device->guest read
#define VBLK_T_OUT 0   // guest->device write
#define VBLK_S_OK  0

// request header the device dma-reads, status byte it dma-writes; both
// live in one pm page (device needs physical addresses)
struct vblk_hdr {
    uint32_t type;
    uint32_t res;
    uint64_t sector;
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

static struct {
    uint16_t base;        // bar0 io port base
    uint32_t qsize;

    uint64_t ring_phys;   // 2-page contiguous chunk
    uint8_t *ring;        // kernel view

    uint64_t meta_phys;   // hdr + status page
    struct vblk_hdr *hdr;
    volatile uint8_t *status;

    uint64_t bounce_phys;
    uint8_t *bounce;

    uint16_t avail_idx;
    uint16_t used_seen;

    uint64_t sectors;
} vb;

static inline void wr32(uint16_t off, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(vb.base + off) : "memory");
}

static inline uint32_t rd32(uint16_t off) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(vb.base + off) : "memory");
    return v;
}

static inline void wr16(uint16_t off, uint16_t v) {
    __asm__ volatile("outw %w0, %w1" :: "a"(v), "Nd"(vb.base + off) : "memory");
}

static inline uint16_t rd16(uint16_t off) {
    uint16_t v;
    __asm__ volatile("inw %w1, %w0" : "=a"(v) : "Nd"(vb.base + off) : "memory");
    return v;
}

static inline void wr8(uint16_t off, uint8_t v) {
    outb(vb.base + off, v);
}

static inline uint8_t rd8(uint16_t off) {
    uint8_t v = inb(vb.base + off);
    __asm__ volatile("" ::: "memory");
    return v;
}

// push a 3-desc chain and wait for the device to consume it
static int req(uint32_t type, uint64_t sector, uint16_t count, int write) {
    // volatile: the device updates used.idx (and reads what we wrote)
    // asynchronously; the compiler must not cache or reorder around it
    volatile struct vring_desc *desc =
        (volatile struct vring_desc *)vb.ring;
    volatile struct vring_avail *avail =
        (volatile struct vring_avail *)(vb.ring + VRING_AVAIL_OFF(vb.qsize));
    volatile struct vring_used *used =
        (volatile struct vring_used *)(vb.ring + VRING_USED_OFF(vb.qsize));

    vb.hdr->type = type;
    vb.hdr->res = 0;
    vb.hdr->sector = sector;
    *vb.status = 0x99;   // poison: the device must overwrite it

    desc[0].addr = vb.meta_phys;
    desc[0].len = sizeof(struct vblk_hdr);
    desc[0].flags = VRING_DESC_NEXT;
    desc[0].next = 1;
    desc[1].addr = vb.bounce_phys;
    desc[1].len = (uint32_t)count * 512;
    desc[1].flags = VRING_DESC_NEXT | (write ? 0 : VRING_DESC_WRITE);
    desc[1].next = 2;
    desc[2].addr = vb.meta_phys + sizeof(struct vblk_hdr);
    desc[2].len = 1;
    desc[2].flags = VRING_DESC_WRITE;
    desc[2].next = 0;

    avail->ring[vb.avail_idx % vb.qsize] = 0;
    vb.avail_idx++;
    avail->idx = vb.avail_idx;
    __asm__ volatile("mfence" ::: "memory");   // stores before the notify
    wr16(VIRTIO_QUEUE_NOTIFY, 0);

    // poll: the used ring advance is the completion signal; the isr read
    // yields the emulator's main loop so the completion can actually run
    uint64_t budget = 400000000;
    while (budget--) {
        if (used->idx != vb.used_seen)
            break;
        if (rd8(VIRTIO_ISR) & 1)
            break;
    }
    if (used->idx == vb.used_seen)
        return -1;   // timed out
    __asm__ volatile("mfence" ::: "memory");

    // device-writable bytes: reads write data + status, writes only status
    uint32_t expect = (write ? 1 : (uint32_t)count * 512 + 1);
    while (vb.used_seen != used->idx) {
        volatile struct vring_used_elem *e =
            &used->ring[vb.used_seen % vb.qsize];
        vb.used_seen++;
        if (e->id != 0 || e->len != expect || *vb.status != VBLK_S_OK)
            return -1;
    }
    return 0;
}

static int vblk_read(uint64_t lba, uint16_t count, void *buf) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl));
    int rc = 0;
    uint8_t *out = buf;
    while (count && !rc) {
        uint16_t chunk = count > 8 ? 8 : count;
        rc = req(VBLK_T_IN, lba, chunk, 0);
        if (!rc)
            memcpy(out, vb.bounce, (uint64_t)chunk * 512);
        out += (uint64_t)chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory");
    return rc;
}

static int vblk_write(uint64_t lba, uint16_t count, const void *buf) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl));
    int rc = 0;
    const uint8_t *in = buf;
    while (count && !rc) {
        uint16_t chunk = count > 8 ? 8 : count;
        memcpy(vb.bounce, in, (uint64_t)chunk * 512);
        rc = req(VBLK_T_OUT, lba, chunk, 1);
        in += (uint64_t)chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory");
    return rc;
}

int virtio_blk_init(void) {
    uint8_t bus, dev, fn, irq;
    uint32_t bar0;
    if (!pci_find_vendor_device(VIRTIO_PCI_VENDOR, VIRTIO_DEV_BLK_LEGACY,
                                &bus, &dev, &fn, &bar0, &irq) &&
        !pci_find_vendor_device(VIRTIO_PCI_VENDOR, VIRTIO_DEV_BLK_MODERN,
                                &bus, &dev, &fn, &bar0, &irq))
        return -1;
    if (!(bar0 & 1))   // legacy interface lives in io space
        return -1;

    vb.base = (uint16_t)(bar0 & 0xfffc);
    vb.sectors = 0;
    vb.avail_idx = 0;
    vb.used_seen = 0;

    // io space + bus master (the device dmAs our buffers)
    uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x7);

    // reset, then handshake: guest features must precede queue setup
    wr8(VIRTIO_STATUS, 0);
    wr8(VIRTIO_STATUS, VIRTIOS_ACK | VIRTIOS_DRIVER);
    wr32(VIRTIO_GUEST_FEATS, 0);   // no features negotiated

    wr16(VIRTIO_QUEUE_SEL, 0);
    uint32_t qsize = rd16(VIRTIO_QUEUE_NUM);
    if (!qsize || qsize > 1024)
        return -1;

    // TEMP: burn the low 16MB so the ring lands at a high address
    void *burn;
    while ((burn = pmm_alloc()) != 0 && (uint64_t)burn < 0x1000000)
        ;
    void *ring = pmm_alloc_contig(VRING_PAGES(qsize));
    if (!ring)
        return -1;
    vb.ring_phys = (uint64_t)ring;
    vb.ring = phys2virt(vb.ring_phys);
    memset(vb.ring, 0, VRING_PAGES(qsize) * PAGE_SIZE);

    void *meta = pmm_alloc_zeroed();
    if (!meta)
        return -1;
    vb.meta_phys = (uint64_t)meta;
    vb.hdr = phys2virt(vb.meta_phys);
    vb.status = phys2virt(vb.meta_phys + sizeof(struct vblk_hdr));

    void *bounce = pmm_alloc_zeroed();
    if (!bounce)
        return -1;
    vb.bounce_phys = (uint64_t)bounce;
    vb.bounce = phys2virt(vb.bounce_phys);

    vb.qsize = qsize;
    wr32(VIRTIO_QUEUE_PFN, (uint32_t)(vb.ring_phys >> 12));

    // device config: capacity (512b sectors) right after the common cfg
    uint64_t cap = rd32(0x14) | ((uint64_t)rd32(0x18) << 32);

    if (!cap)
        return -1;
    vb.sectors = cap;

    wr8(VIRTIO_STATUS, rd8(VIRTIO_STATUS) | VIRTIOS_DRIVER_OK);

    // self-test: write a pattern to the last sector, read it back, then
    // restore the original content. if the data path is broken (seen on
    // some emulator builds), refuse the disk so ata/ahci can take over
    {
        uint64_t scratch = vb.sectors - 1;
        uint8_t orig[512];
        int rc = vblk_read(scratch, 1, orig);
        if (rc == 0) {
            for (int i = 0; i < 512; i++)
                vb.bounce[i] = (uint8_t)(0xC0 ^ (i * 7));
            rc = vblk_write(scratch, 1, vb.bounce);
            if (rc == 0) {
                uint8_t back[512];
                // wipe the bounce: a working data path refills it from
                // the disk; a broken one leaves the wipe visible
                memset(vb.bounce, 0x55, 512);
                rc = vblk_read(scratch, 1, back);
                if (rc == 0) {
                    for (int i = 0; i < 512; i++)
                        if (back[i] != (uint8_t)(0xC0 ^ (i * 7)))
                            rc = -1;
                }
            }
            // restore whatever lived there
            memcpy(vb.bounce, orig, 512);
            int rc2 = vblk_write(scratch, 1, vb.bounce);
            if (rc == 0)
                rc = rc2;
        }
        if (rc != 0) {
            console_puts("virtio-blk: data path self-test failed\n");
            return -1;
        }
    }

    if (!root_disk.ready) {
        root_disk.name = "virtio-blk (dma)";
        root_disk.sectors = vb.sectors;
        root_disk.read_sectors = vblk_read;
        root_disk.write_sectors = vblk_write;
        root_disk.ready = 1;
    }

    console_puts("virtio-blk: pci ");
    console_putc((char)('0' + bus));
    console_puts(":");
    console_putc((char)('0' + dev));
    console_puts(".");
    console_putc((char)('0' + fn));
    console_puts(", ");
    console_puts("io 0x");
    for (int i = 12; i >= 4; i -= 4)
        console_putc("0123456789abcdef"[(bar0 >> i) & 0xf]);
    console_puts("\n");
    return 0;
}
