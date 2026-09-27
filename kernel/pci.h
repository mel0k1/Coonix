// pci config space: find devices by class
#pragma once
#include <stdint.h>

#define PCI_CLASS_MASS   0x01
#define PCI_SUB_SATA     0x06   // AHCI (serial ata controller)

// 32-bit config read
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
// 32-bit config write
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

// scan buses 0..15 for the first device matching class/subclass;
// fills bar5 with BAR5 (mem base) if non-null; returns 1 if found
int pci_find_class(uint8_t class, uint8_t subclass,
                   uint8_t *bus, uint8_t *dev, uint8_t *fn, uint32_t *bar5);
