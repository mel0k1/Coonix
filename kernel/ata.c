#include "ata.h"
#include "blkdev.h"
#include "console.h"
#include "string.h"

// primary bus io ports
#define ATA_DATA       0x1f0
#define ATA_ERROR      0x1f1
// 0x1f7: status when read, command when written
#define ATA_SECCOUNT   0x1f2
#define ATA_LBA_LO     0x1f3
#define ATA_LBA_MID    0x1f4
#define ATA_LBA_HI     0x1f5
#define ATA_DRIVE      0x1f6
#define ATA_STATUS     0x1f7
#define ATA_CONTROL    0x3f6   // nIEN bit 1

#define SR_ERR  0x01
#define SR_DRQ  0x08
#define SR_DF   0x20
#define SR_BSY  0x80

#define CMD_IDENTIFY   0xec
#define CMD_READ_EXT   0x24
#define CMD_WRITE_EXT  0x34
#define CMD_FLUSH_EXT  0xe7

static uint64_t max_lba;   // from identify
static char model[41];

static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}

static inline uint16_t inw(uint16_t p) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static inline void outsw(uint16_t p, const void *buf, uint64_t words) {
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(p) : "memory");
}

static inline void insw(uint16_t p, void *buf, uint64_t words) {
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(p) : "memory");
}

static inline void io_wait(void) {
    inb(0x80);
}

// spin until busy clears and any of mask bits sets; mask 0 = just not busy;
// -1 on error/timeout
static int status_wait(uint8_t mask) {
    // 400ns settle
    for (int i = 0; i < 4; i++)
        inb(ATA_CONTROL);
    for (uint64_t t = 0; t < 5000000; t++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & SR_BSY)
            continue;
        if (st & (SR_ERR | SR_DF))
            return -1;
        if (!mask || (st & mask))
            return 0;
    }
    return -1;
}

static void select_drive(void) {
    outb(ATA_DRIVE, 0x40);   // master, lba mode
    io_wait();
    inb(ATA_STATUS);
    io_wait();
}

static void flush_model(const uint16_t *id) {
    // words 27..46, byte-swapped
    for (int i = 0; i < 40; i += 2) {
        uint16_t w = id[27 + i / 2];
        model[i] = (char)(w >> 8);
        model[i + 1] = (char)(w & 0xff);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = 0;
}

uint64_t ata_init(void) {
    max_lba = 0;
    model[0] = 0;

    // silence the irq, we poll
    outb(ATA_CONTROL, 0x02);
    io_wait();
    select_drive();

    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_STATUS, CMD_IDENTIFY);   // 0x1f7 = command register when written
    io_wait();

    uint8_t st = inb(ATA_STATUS);
    if (st == 0)
        return 0;             // no drive at all
    if (inb(ATA_LBA_MID) || inb(ATA_LBA_HI))
        return 0;             // atapi/sata bridge, not a plain ata disk

    if (status_wait(SR_DRQ) < 0)
        return 0;

    uint16_t id[256];
    insw(ATA_DATA, id, 256);

    // lba48 support: word 83 bit 10
    if (!(id[83] & (1 << 10)))
        return 0;
    // user addressable sectors live in words 100..103 (48 bit)
    max_lba = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
              ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    flush_model(id);
    max_lba = max_lba + 1;
    // claim the root disk slot
    if (!root_disk.ready) {
        root_disk.name = "ata0 master (pio)";
        root_disk.sectors = max_lba;
        root_disk.read_sectors = ata_read_sectors;
        root_disk.write_sectors = ata_write_sectors;
        root_disk.ready = 1;
    }
    return max_lba;
}

// issue a read/write for up to 256 sectors, caller holds cli and spun up
static int transfer(uint64_t lba, uint16_t count, void *buf, int write) {
    if (lba + count > max_lba)   // max_lba holds the sector count now
        return -1;

    outb(ATA_DRIVE, 0x40 | ((lba >> 24) & 0x0f));
    status_wait(0);   // not busy

    outb(ATA_SECCOUNT, (uint8_t)(count >> 8));
    outb(ATA_LBA_LO, (uint8_t)(lba >> 24));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 32));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 40));
    outb(ATA_SECCOUNT, (uint8_t)count);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_STATUS, write ? CMD_WRITE_EXT : CMD_READ_EXT);

    for (uint16_t s = 0; s < count; s++) {
        if (status_wait(SR_DRQ) < 0)
            return -1;
        uint8_t *p = (uint8_t *)buf + (uint64_t)s * ATA_SECTOR;
        if (write)
            outsw(ATA_DATA, p, ATA_SECTOR / 2);
        else
            insw(ATA_DATA, p, ATA_SECTOR / 2);
    }
    if (write) {
        outb(ATA_STATUS, CMD_FLUSH_EXT);
        if (status_wait(0) < 0)
            return -1;
    }
    return 0;
}

// irq-free critical section: a pit tick mid-transfer would let another
// task stomp on our shared command ports
#define ATA_ENTER  uint64_t __fl; __asm__ volatile("pushfq; popq %0; cli" : "=r"(__fl))
#define ATA_LEAVE  __asm__ volatile("pushq %0; popfq" :: "r"(__fl) : "memory")

int ata_read_sectors(uint64_t lba, uint16_t count, void *buf) {
    if (!max_lba)
        return -1;
    ATA_ENTER;
    // max 256 sectors per command (0 means 256 in the register, avoid that)
    int rc = 0;
    while (count && !rc) {
        uint16_t chunk = count > 8 ? 8 : count;
        rc = transfer(lba, chunk, buf, 0);
        lba += chunk;
        count -= chunk;
        buf = (uint8_t *)buf + (uint64_t)chunk * ATA_SECTOR;
    }
    ATA_LEAVE;
    return rc;
}

int ata_write_sectors(uint64_t lba, uint16_t count, const void *buf) {
    if (!max_lba)
        return -1;
    ATA_ENTER;
    int rc = 0;
    while (count && !rc) {
        uint16_t chunk = count > 8 ? 8 : count;
        rc = transfer(lba, chunk, (void *)buf, 1);
        lba += chunk;
        count -= chunk;
        buf = (const uint8_t *)buf + (uint64_t)chunk * ATA_SECTOR;
    }
    ATA_LEAVE;
    return rc;
}

// for boot log
const char *ata_model(void) {
    return model;
}
