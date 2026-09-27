#include "ahci.h"
#include "pci.h"
#include "blkdev.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "kernel.h"

// -- hba memory layout ------------------------------------------------------

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_PI    0x0c
#define GHC_HR    (1u << 0)
#define GHC_IE    (1u << 1)
#define GHC_AE    (1u << 31)

#define PORT_OFF  0x100
#define PORT_SZ   0x80

#define PxCLB   0x00
#define PxFB    0x08
#define PxIS    0x10
#define PxCMD   0x18
#define PxTFD   0x20
#define PxSSTS  0x28
#define PxSCTL  0x2c
#define PxSERR  0x30
#define PxCI    0x38

#define CMD_ST    (1u << 0)
#define CMD_FRE   (1u << 4)   // fis receive enable (bit 2 is power-on device!)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)

#define SSTS_DET  0xf
#define SSTS_IPM  (0xfu << 8)

#define SERR_CLEAR 0xffffffffu

#define CMD_ATA_IDENTIFY 0xec
#define CMD_READ_DMA_EXT 0x25
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_FLUSH_EXT    0xe7

// one hba, one active port, one command slot
static struct {
    volatile uint8_t *abar;
    volatile uint8_t *port;
    int port_no;

    uint64_t list_phys;    // 1k command list
    uint64_t fis_phys;     // 256 received fis
    uint64_t ct_phys;      // command table (128 hdr + prdt)
    uint64_t bounce_phys;  // dma bounce page

    uint8_t *list;
    uint8_t *ct;
    uint8_t *bounce;

    uint64_t sectors;
    char model[41];
} hc;

// mmio helpers
static inline uint32_t rd32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

// spin until (rd(mask) & mask) == want, with an iteration budget
static int spin(volatile uint8_t *base, uint32_t off, uint32_t mask,
                uint32_t want, uint64_t budget) {
    for (uint64_t t = 0; t < budget; t++) {
        uint32_t v = rd32(base, off);
        if ((v & mask) == want)
            return 0;
    }
    return -1;
}

// -- command issue ----------------------------------------------------------

// issue a dma command through slot 0, wait for completion; the bounce page
// carries the data (4k = 8 sectors per go). engine must be running.
static int issue(uint8_t cmd, uint64_t lba, uint16_t count, int write) {
    volatile uint8_t *p = hc.port;

    memset(hc.list, 0, 1024);
    // command header 0: fis length 5 dwords, atapi=0, write flag, 1 prdt
    uint16_t flags = 5 | (write ? (1 << 6) : 0);
    *(volatile uint16_t *)(hc.list + 0) = flags;
    *(volatile uint16_t *)(hc.list + 2) = 1;        // prdtl
    *(volatile uint32_t *)(hc.list + 4) = 0;        // prdbc
    *(volatile uint64_t *)(hc.list + 8) = hc.ct_phys;

    memset(hc.ct, 0, 128 + 16);
    // host-to-device fis (ahci 1.3 layout):
    // 0 type 0x27, 1 pmport|C(0x80), 2 command, 3 feat lo, 4..6 lba[23:0],
    // 7 device (lba bit + lba[27:24]), 8 lba[31:24], 9 lba[39:32],
    // 10 lba[47:40], 11 feat hi, 12 count lo, 13 count hi
    hc.ct[0] = 0x27;
    hc.ct[1] = 0x80;                                 // C: update registers
    hc.ct[2] = cmd;
    hc.ct[3] = 0;
    hc.ct[4] = (uint8_t)lba;
    hc.ct[5] = (uint8_t)(lba >> 8);
    hc.ct[6] = (uint8_t)(lba >> 16);
    hc.ct[7] = 0x40 | (uint8_t)(lba >> 24);          // LBA bit + lba[27:24]
    hc.ct[8] = (uint8_t)(lba >> 32);
    hc.ct[9] = (uint8_t)(lba >> 40);
    hc.ct[10] = 0;
    hc.ct[11] = 0;
    hc.ct[12] = (uint8_t)count;
    hc.ct[13] = (uint8_t)(count >> 8);

    // prdt entry 0: dw0/1 = data base addr, dw2 reserved, dw3 = dbc|i.
    // dbc excludes one byte (dbc = bytecount - 1 per spec)
    *(volatile uint64_t *)(hc.ct + 128) = hc.bounce_phys;
    *(volatile uint32_t *)(hc.ct + 136) = 0;                        // dw2
    *(volatile uint32_t *)(hc.ct + 140) = (4096 - 1) | (1u << 31);  // dw3: dbc|i

    wr32(p, PxSERR, SERR_CLEAR);
    wr32(p, PxIS, 0xffffffffu);
    wr32(p, PxCI, 1);

    int rc = spin(p, PxCI, 1, 0, 100000000);
    uint32_t is = rd32(p, PxIS);
    if (rc < 0 || (is & ((1u << 31) | (1u << 30) | (1u << 29))))
        return -1;   // timeout or tfes/hbf
    return rc;
}

static int ahci_read_dma(uint64_t lba, uint16_t count, void *buf) {
    // irq-free critical section: another task pre-empting mid-command would
    // share our single command slot
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl));
    int rc = 0;
    uint8_t *out = buf;
    while (count && !rc) {
        uint16_t chunk = count > 8 ? 8 : count;
        rc = issue(CMD_READ_DMA_EXT, lba, chunk, 0);
        if (!rc)
            memcpy(out, hc.bounce, (uint64_t)chunk * 512);
        out += (uint64_t)chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory");
    return rc;
}

static int ahci_write_dma(uint64_t lba, uint16_t count, const void *buf) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl));
    int rc = 0;
    const uint8_t *in = buf;
    while (count && !rc) {
        uint16_t chunk = count > 8 ? 8 : count;
        memcpy(hc.bounce, in, (uint64_t)chunk * 512);
        rc = issue(CMD_WRITE_DMA_EXT, lba, chunk, 1);
        in += (uint64_t)chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    if (!rc)
        rc = issue(CMD_FLUSH_EXT, 0, 0, 0);   // flush cache, ext2_sync semantics
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory");
    return rc;
}

// -- init -------------------------------------------------------------------

// allocate port structures in one pm page + a bounce page (all < 4G on qemu).
// pmm_alloc returns physical addresses, hhdm gives the kernel view.
static int port_alloc(void) {
    void *pg = pmm_alloc_zeroed();
    if (!pg)
        return -1;
    hc.list_phys = (uint64_t)pg;
    hc.list = phys2virt(hc.list_phys);
    hc.fis_phys = hc.list_phys + 1024;      // recv fis, 256-aligned
    hc.ct_phys = hc.list_phys + 1280;       // 128-aligned command table
    hc.ct = phys2virt(hc.ct_phys);

    void *bp = pmm_alloc_zeroed();
    if (!bp)
        return -1;
    hc.bounce_phys = (uint64_t)bp;
    hc.bounce = phys2virt(hc.bounce_phys);
    return 0;
}

static int port_start(uint32_t idx) {
    volatile uint8_t *p = hc.abar + PORT_OFF + idx * PORT_SZ;

    // comreset: restart the link so the device sends a fresh d2h fis,
    // otherwise the hba never reports FR after a global hba reset
    wr32(p, PxSCTL, (rd32(p, PxSCTL) & ~0xfu) | 1);
    for (volatile int d = 0; d < 100000; d++)
        ;
    wr32(p, PxSCTL, rd32(p, PxSCTL) & ~0xfu);
    if (spin(p, PxSSTS, SSTS_DET, 3, 10000000) < 0) {
        return -1;
    }

    // stop everything first
    uint32_t c = rd32(p, PxCMD);
    if (c & CMD_ST) {
        wr32(p, PxCMD, c & ~CMD_ST);
        if (spin(p, PxCMD, CMD_CR, 0, 5000000) < 0)
            return -1;
    }
    if (rd32(p, PxCMD) & CMD_FRE) {
        wr32(p, PxCMD, rd32(p, PxCMD) & ~CMD_FRE);
        if (spin(p, PxCMD, CMD_FR, 0, 5000000) < 0)
            return -1;
    }

    if (port_alloc() < 0)
        return -1;
    wr32(p, PxCLB, (uint32_t)hc.list_phys);
    wr32(p, PxFB, (uint32_t)hc.fis_phys);
    wr32(p, PxSERR, SERR_CLEAR);
    wr32(p, PxIS, 0xffffffffu);

    // start: fis receive, then command engine
    wr32(p, PxCMD, (rd32(p, PxCMD) | CMD_FRE) & ~CMD_ST);
    if (spin(p, PxCMD, CMD_FR, CMD_FR, 5000000) < 0) {
        return -1;
    }
    wr32(p, PxCMD, rd32(p, PxCMD) | CMD_ST);
    if (spin(p, PxCMD, CMD_CR, CMD_CR, 5000000) < 0) {
        return -1;
    }
    return 0;
}

// issue identify through the just-started port
static int identify(void) {
    if (issue(CMD_ATA_IDENTIFY, 0, 1, 0) < 0)
        return -1;
    uint16_t id[256];
    memcpy(id, hc.bounce, 512);
    if (!(id[83] & (1 << 10)))
        return -1;   // no lba48
    hc.sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                 ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    for (int i = 0; i < 40; i += 2) {
        uint16_t w = id[27 + i / 2];
        hc.model[i] = (char)(w >> 8);
        hc.model[i + 1] = (char)(w & 0xff);
    }
    hc.model[40] = 0;
    for (int i = 39; i >= 0 && hc.model[i] == ' '; i--)
        hc.model[i] = 0;
    return 0;
}

int ahci_init(void) {
    hc.abar = 0;
    hc.port = 0;
    hc.sectors = 0;
    hc.model[0] = 0;

    uint8_t bus, dev, fn;
    uint32_t abar_phys;
    if (!pci_find_class(PCI_CLASS_MASS, PCI_SUB_SATA, &bus, &dev, &fn,
                        &abar_phys)) {
        return -1;
    }

    // enable memory space + bus mastering
    uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x6);

    hc.abar = mmio_map(abar_phys, 0x2000);
    if (!hc.abar)
        return -1;

    // reset the controller, then re-enable ahci
    wr32(hc.abar, HBA_GHC, rd32(hc.abar, HBA_GHC) | GHC_AE);
    wr32(hc.abar, HBA_GHC, rd32(hc.abar, HBA_GHC) | GHC_HR);
    if (spin(hc.abar, HBA_GHC, GHC_HR, 0, 5000000) < 0) {
        return -1;
    }
    wr32(hc.abar, HBA_GHC, GHC_AE);          // irq off, ahci on

    uint32_t pi = rd32(hc.abar, HBA_PI);
    for (uint32_t i = 0; i < 32; i++) {
        if (!(pi & (1u << i)))
            continue;
        volatile uint8_t *p = hc.abar + PORT_OFF + i * PORT_SZ;
        uint32_t ssts = rd32(p, PxSSTS);
        if ((ssts & SSTS_DET) == 3 && (ssts & SSTS_IPM) != 0) {
            hc.port_no = (int)i;
            hc.port = p;
            break;
        }
    }
    if (!hc.port)
        return -1;

    if (port_start((uint32_t)hc.port_no) < 0)
        return -1;
    if (identify() < 0)
        return -1;

    if (!root_disk.ready) {
        root_disk.name = "ahci sata (dma)";
        root_disk.sectors = hc.sectors;
        root_disk.read_sectors = ahci_read_dma;
        root_disk.write_sectors = ahci_write_dma;
        root_disk.ready = 1;
    }

    console_puts("ahci: port ");
    console_putc((char)('0' + hc.port_no));
    console_puts(": ");
    console_puts(hc.model);
    console_puts("\n");
    return 0;
}
