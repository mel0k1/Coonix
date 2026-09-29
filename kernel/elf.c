#include "elf.h"
#include "vmm.h"
#include "heap.h"
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
#define PT_INTERP 3
#define PF_X 1
#define PF_W 2

uint64_t elf_load_user(uint64_t pml4, const void *elf, size_t size,
                       uint64_t *image_end) {
    (void)image_end;   // kept for the old call sites; use *_info for details
    return elf_load_user_info(pml4, elf, size, 0);
}

uint64_t elf_load_user_info(uint64_t pml4, const void *elf, size_t size,
                            struct elf_info *info) {
    const struct elf64_hdr *eh = elf;
    if (size < sizeof(*eh) || eh->ident[0] != 0x7f || eh->ident[1] != 'E')
        return 0;

    if (info) {
        info->entry = 0;
        info->jump = 0;
        info->base = 0;
        info->image_end = 0;
        info->phdr_va = 0;
        info->phent = 0;
        info->phnum = 0;
    }

    // switch so we can write into the new address space
    uint64_t old = vmm_kernel_pml4();
    vmm_switch(pml4);

    uint64_t top = 0;
    uint64_t base_va = 0;   // va of the segment covering the phdrs
    int have_base = 0;
    uint64_t interp_off = 0, interp_sz = 0;   // PT_INTERP path string
    const struct elf64_phdr *ph = (const void *)((const uint8_t *)elf + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_INTERP) {
            interp_off = ph[i].offset;
            interp_sz = ph[i].filesz;
            continue;
        }
        if (ph[i].type != PT_LOAD)
            continue;
        // remember the lowest segment: the phdrs live inside it
        if (!have_base || ph[i].vaddr < base_va) {
            base_va = ph[i].vaddr & ~0xfffULL;
            have_base = 1;
        }
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
        // relax the copy-time write bit: read-only segments (text, rodata,
        // the elf header page) must actually fault on write, or the null
        // page of a PIE (mapped at vaddr 0) silently accepts stores
        if (!(ph[i].flags & PF_W))
            vmm_mprotect(pml4, start, (end - start) / PAGE_SIZE, flags);
        if (end > top)
            top = end;
    }
    if (info) {
        info->entry = eh->entry;
        info->image_end = top;
        info->phdr_va = have_base ? base_va + eh->phoff : 0;
        info->phent = eh->phentsize;
        info->phnum = eh->phnum;
    }

    // dynamic program: load the interpreter too, jump to it instead
    if (interp_off && interp_off + interp_sz <= size) {
        const char *path = (const char *)elf + interp_off;
        // keep it simple: the only interp we support is the linux default
        static const char ld_path[] = "/lib64/ld-linux-x86-64.so.2";
        uint64_t plen = sizeof(ld_path) - 1;
        if (interp_sz < plen || __builtin_memcmp(path, ld_path, plen + 1) != 0) {
            vmm_switch(old);
            return 0;
        }
        // ld.so itself is an elf file the caller hands us via the vfs
        void *ldimg = 0;
        extern long vfs_read_file(const char *path, void **outbuf);
        long ldsz = vfs_read_file(ld_path, &ldimg);
        if (ldsz <= 0) {
            vmm_switch(old);
            return 0;
        }
        const struct elf64_hdr *le = ldimg;
        const struct elf64_phdr *lph =
            (const void *)((const uint8_t *)ldimg + le->phoff);
        for (int i = 0; i < le->phnum; i++) {
            if (lph[i].type != PT_LOAD)
                continue;
            uint64_t flags = VMM_PRESENT | VMM_USER | VMM_NX;
            if (lph[i].flags & PF_W)
                flags |= VMM_WRITE;
            if (lph[i].flags & PF_X)
                flags &= ~VMM_NX;
            uint64_t start = INTERP_BASE + (lph[i].vaddr & ~0xfffULL);
            uint64_t end = INTERP_BASE +
                ((lph[i].vaddr + lph[i].memsz + 0xfff) & ~0xfffULL);
            uint64_t wflags = flags | VMM_WRITE;
            for (uint64_t va = start; va < end; va += PAGE_SIZE) {
                void *page = pmm_alloc_zeroed();
                if (!page) {
                    kfree(ldimg);
                    vmm_switch(old);
                    return 0;
                }
                vmm_map(pml4, va, (uint64_t)page, wflags);
            }
            if (lph[i].filesz)
                memcpy((void *)(INTERP_BASE + lph[i].vaddr),
                       (const uint8_t *)ldimg + lph[i].offset, lph[i].filesz);
            if (!(lph[i].flags & PF_W))
                vmm_mprotect(pml4, start, (end - start) / PAGE_SIZE, flags);
            uint64_t iend = INTERP_BASE +
                ((lph[i].vaddr + lph[i].memsz + 0xfff) & ~0xfffULL);
            if (iend > top)
                top = iend;
        }
        if (info) {
            info->base = INTERP_BASE;
            info->jump = INTERP_BASE + le->entry;
        }
        kfree(ldimg);
    }
    return eh->entry;
}
