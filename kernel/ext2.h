// ext2 read-only, 1k blocks, on the ata disk
#pragma once
#include <stdint.h>
#include "vfs.h"

// probe ata primary master for ext2 magic; 0 = ok, -1 = no/foreign fs
int ext2_mount_root(void);
