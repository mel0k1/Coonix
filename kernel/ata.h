// ata pio driver, primary bus (ports 0x1f0), lba48, polling
#pragma once
#include <stdint.h>

#define ATA_SECTOR 512
#define ATA_NO_DISK 0

// returns max lba (sector count), 0 = no drive
uint64_t ata_init(void);
// count <= 256, may cli/sti around transfers
int ata_read_sectors(uint64_t lba, uint16_t count, void *buf);
int ata_write_sectors(uint64_t lba, uint16_t count, const void *buf);
// drive identity string from identify, "" if absent
const char *ata_model(void);
