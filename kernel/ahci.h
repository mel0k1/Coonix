// ahci (sata) driver over pci, q35 ich9 style: polling dma, one slot
#pragma once
#include <stdint.h>

// probe pci for an ahci controller and set up the first attached disk;
// claims root_disk. 0 = ok, -1 = no controller / no disk / init failure
int ahci_init(void);
