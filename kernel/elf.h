// user program loader: static ELF64
#pragma once
#include <stdint.h>
#include <stddef.h>

// maps PT_LOADs into fresh user space, returns entry point;
// image_end gets the byte past the last loaded segment (brk base);
// phdr info feeds AT_PHDR/AT_PHNUM/AT_PHENT auxv entries
struct elf_info {
    uint64_t entry;     // program entry point
    uint64_t jump;      // rip to start at (interp entry for dynamic, else entry)
    uint64_t base;      // AT_BASE (interp load base, 0 for static)
    uint64_t image_end;
    uint64_t phdr_va;   // program headers in user va, 0 if unknown
    uint64_t phent;
    uint64_t phnum;
};

// dynamic binaries: where the kernel loads the interpreter (ld.so)
#define INTERP_BASE 0x7f0000000000ULL

uint64_t elf_load_user(uint64_t pml4, const void *elf, size_t size,
                       uint64_t *image_end);
uint64_t elf_load_user_info(uint64_t pml4, const void *elf, size_t size,
                            struct elf_info *info);
