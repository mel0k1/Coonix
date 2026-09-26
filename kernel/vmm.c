#include "vmm.h"
#include "pmm.h"
#include "kernel.h"
#include "console.h"

static uint64_t *next_level(uint64_t *table, uint64_t idx, uint64_t flags) {
    uint64_t e = table[idx];
    if (e & VMM_PRESENT)
        return phys2virt(e & 0x000ffffffffff000ULL);
    uint64_t phys = (uint64_t)pmm_alloc_zeroed(); // phys addr as pointer
    if (!phys)
        return 0;
    table[idx] = phys | flags;
    return phys2virt(phys);
}

uint64_t vmm_kernel_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & 0x000ffffffffff000ULL;
}

uint64_t vmm_create_pml4(void) {
    uint64_t pml4_phys = (uint64_t)pmm_alloc_zeroed();
    if (!pml4_phys)
        return 0;
    uint64_t *kern = phys2virt(vmm_kernel_pml4());
    uint64_t *pml4 = phys2virt(pml4_phys);
    // copy kernel half (entries 256..511): kernel text + hhdm stay visible
    for (int i = 256; i < 512; i++)
        pml4[i] = kern[i];
    return pml4_phys;
}

void vmm_map(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    // intermediate levels must inherit USER, else ring3 code gets #PF
    uint64_t mid = VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    uint64_t *pml4 = phys2virt(pml4_phys);
    uint64_t *pdp = next_level(pml4, (vaddr >> 39) & 0x1ff, mid);
    if (!pdp) panic("vmm: out of memory (pdp)");
    uint64_t *pd  = next_level(pdp, (vaddr >> 30) & 0x1ff, mid);
    if (!pd) panic("vmm: out of memory (pd)");
    uint64_t *pt  = next_level(pd,  (vaddr >> 21) & 0x1ff, mid);
    if (!pt) panic("vmm: out of memory (pt)");
    pt[(vaddr >> 12) & 0x1ff] = (paddr & 0x000ffffffffff000ULL) | flags;
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

void vmm_unmap(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pml4 = phys2virt(pml4_phys);
    uint64_t e = pml4[(vaddr >> 39) & 0x1ff];
    if (!(e & VMM_PRESENT)) return;
    uint64_t *pdp = phys2virt(e & 0x000ffffffffff000ULL);
    e = pdp[(vaddr >> 30) & 0x1ff];
    if (!(e & VMM_PRESENT)) return;
    uint64_t *pd = phys2virt(e & 0x000ffffffffff000ULL);
    e = pd[(vaddr >> 21) & 0x1ff];
    if (!(e & VMM_PRESENT)) return;
    uint64_t *pt = phys2virt(e & 0x000ffffffffff000ULL);
    pt[(vaddr >> 12) & 0x1ff] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pml4 = phys2virt(pml4_phys);
    uint64_t e = pml4[(vaddr >> 39) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pdp = phys2virt(e & 0x000ffffffffff000ULL);
    e = pdp[(vaddr >> 30) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pd = phys2virt(e & 0x000ffffffffff000ULL);
    e = pd[(vaddr >> 21) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pt = phys2virt(e & 0x000ffffffffff000ULL);
    e = pt[(vaddr >> 12) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    return (e & 0x000ffffffffff000ULL) + (vaddr & 0xfff);
}

uint64_t vmm_get_pte(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pml4 = phys2virt(pml4_phys);
    uint64_t e = pml4[(vaddr >> 39) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pdp = phys2virt(e & 0x000ffffffffff000ULL);
    e = pdp[(vaddr >> 30) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pd = phys2virt(e & 0x000ffffffffff000ULL);
    e = pd[(vaddr >> 21) & 0x1ff];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pt = phys2virt(e & 0x000ffffffffff000ULL);
    return pt[(vaddr >> 12) & 0x1ff];
}

void vmm_switch(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}
