// root block device abstraction: ata pio or ahci registers here
#pragma once
#include <stdint.h>

struct blkdev {
    const char *name;
    uint64_t sectors;     // total 512-byte sectors
    int ready;
    int (*read_sectors)(uint64_t lba, uint16_t count, void *buf);
    int (*write_sectors)(uint64_t lba, uint16_t count, const void *buf);
};

extern struct blkdev root_disk;
