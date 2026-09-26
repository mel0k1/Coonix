// user program loader: static ELF64
#pragma once
#include <stdint.h>
#include <stddef.h>

// maps PT_LOADs into fresh user space, returns entry point
uint64_t elf_load_user(uint64_t pml4, const void *elf, size_t size);
