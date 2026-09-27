// initramfs: unpacks the ustar tarball loaded by limine as a module
#pragma once

void initramfs_load(void);   // fills tmpfs, panics on missing module
