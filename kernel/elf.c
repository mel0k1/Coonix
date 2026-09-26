#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "kernel.h"
#include "console.h"
#include "string.h"

struct elf64_hdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

uint64_t elf_load_user(uint64_t pml4, const void *elf, size_t size) {
    const struct elf64_hdr *eh = elf;
    if (size < sizeof(*eh) || eh->ident[0] != 0x7f || eh->ident[1] != 'E')
        return 0;

    // switch so we can write into the new address space
    uint64_t old = vmm_kernel_pml4();
    vmm_switch(pml4);

    const struct elf64_phdr *ph = (const void *)((const uint8_t *)elf + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD)
            continue;
        uint64_t flags = VMM_PRESENT | VMM_USER | VMM_NX;
        if (ph[i].flags & PF_W)
            flags |= VMM_WRITE;
        if (ph[i].flags & PF_X)
            flags &= ~VMM_NX;

        uint64_t start = ph[i].vaddr & ~0xfffULL;
        uint64_t end = (ph[i].vaddr + ph[i].memsz + 0xfff) & ~0xfffULL;
        // map writable for the copy, relax perms afterwards
        uint64_t wflags = flags | VMM_WRITE;
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            void *page = pmm_alloc_zeroed();
            if (!page) {
                vmm_switch(old);
                return 0;
            }
            vmm_map(pml4, va, (uint64_t)page, wflags);
        }
        if (ph[i].filesz)
            memcpy((void *)ph[i].vaddr, (const uint8_t *)elf + ph[i].offset, ph[i].filesz);
        // TODO: drop write bit for read-only segments (W^X)
    }
    return eh->entry;
}
