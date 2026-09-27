// tmpfs: in-memory filesystem, tree of nodes on the kernel heap
#pragma once
#include "vfs.h"

// mounts tmpfs as vfs root ("/")
void tmpfs_mount(void);

// mkdir -p, 0 on success
int tmpfs_mkdir_p(const char *path);
// create/overwrite file with data copy, 0 on success
int tmpfs_put_file(const char *path, const void *data, uint64_t size);

int tmpfs_node_count(void);
