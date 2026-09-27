// ext2 read-only, 1k blocks, on the ata disk
#pragma once
#include <stdint.h>
#include "vfs.h"

// probe the root block device for ext2 magic; 0 = ok, -1 = no/foreign fs
int ext2_mount_root(void);
// flush all dirty cached blocks to the device
void ext2_sync(void);
