#include "vmm.h"
#include "pmm.h"
#include "kernel.h"
#include "console.h"
#include "string.h"

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

// 1 if [uaddr, uaddr+len) is fully mapped user memory. own walk (not
// vmm_get_pte): 2M user pages don't exist here, so a PS-bit entry must
// fail closed rather than be dereferenced as a page table
int vmm_user_range_ok(uint64_t pml4_phys, uint64_t uaddr, uint64_t len,
                      int need_write) {
    if (!len)
        return 1;
    if (uaddr >= VMM_USER_LIMIT || len > VMM_USER_LIMIT - uaddr)
        return 0;
    uint64_t last = (uaddr + len - 1) & ~0xfffULL;
    uint64_t va = uaddr & ~0xfffULL;
    for (;;) {
        uint64_t *pml4 = phys2virt(pml4_phys);
        uint64_t e = pml4[(va >> 39) & 0x1ff];
        if (!(e & VMM_PRESENT) || !(e & VMM_USER))
            return 0;
        uint64_t *pdp = phys2virt(e & 0x000ffffffffff000ULL);
        e = pdp[(va >> 30) & 0x1ff];
        if (!(e & VMM_PRESENT) || !(e & VMM_USER))
            return 0;
        uint64_t *pd = phys2virt(e & 0x000ffffffffff000ULL);
        e = pd[(va >> 21) & 0x1ff];
        if (!(e & VMM_PRESENT) || !(e & VMM_USER) || (e & 0x080))
            return 0;   // PS set: no 2M user pages in this kernel
        uint64_t *pt = phys2virt(e & 0x000ffffffffff000ULL);
        e = pt[(va >> 12) & 0x1ff];
        if (!(e & VMM_PRESENT) || !(e & VMM_USER))
            return 0;
        if (need_write && !(e & (VMM_WRITE | VMM_COW)))
            return 0;
        if (va == last)
            return 1;
        va += PAGE_SIZE;
    }
}

// re-protect an existing range (mprotect); pages must be mapped
void vmm_mprotect(uint64_t pml4, uint64_t vaddr, uint64_t pages, uint64_t flags) {
    uint64_t *pml4t = phys2virt(pml4);
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t va = vaddr + p * PAGE_SIZE;
        uint64_t e1 = pml4t[(va >> 39) & 0x1ff];
        if (!(e1 & VMM_PRESENT))
            continue;
        uint64_t *pdp = phys2virt(e1 & 0x000ffffffffff000ULL);
        uint64_t e2 = pdp[(va >> 30) & 0x1ff];
        if (!(e2 & VMM_PRESENT))
            continue;
        uint64_t *pd = phys2virt(e2 & 0x000ffffffffff000ULL);
        uint64_t e3 = pd[(va >> 21) & 0x1ff];
        if (!(e3 & VMM_PRESENT))
            continue;
        uint64_t *pt = phys2virt(e3 & 0x000ffffffffff000ULL);
        uint64_t *pte = &pt[(va >> 12) & 0x1ff];
        if (!(*pte & VMM_PRESENT))
            continue;
        uint64_t old = *pte;
        uint64_t nw = (old & ~0x1fe & ~VMM_NX) | (flags & 0x1fe) | (flags & VMM_NX);
        if ((flags & VMM_WRITE) && (old & VMM_COW)) {
            // granting W on a cow page must resolve the sharing first,
            // else both sides write one frame with no fault, no copy
            uint64_t phys = old & 0x000ffffffffff000ULL;
            if (pmm_refcount((void *)phys) > 1) {
                void *np = pmm_alloc();
                if (!np)
                    continue;   // stay cow: a write fault resolves later
                memcpy(phys2virt((uint64_t)np), phys2virt(phys), PAGE_SIZE);
                pmm_free((void *)phys);
                phys = (uint64_t)np;
            }
            nw &= ~VMM_COW;   // private now (copied or last holder)
        }
        *pte = nw;
        __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    }
}

void vmm_switch(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

// map an MMIO region into the kernel half, cache-disabled; hhdm covers it
// when limine pre-mapped the hole, otherwise we add mappings on demand
void *mmio_map(uint64_t phys, uint64_t size) {
    for (uint64_t p = 0; p < size; p += PAGE_SIZE) {
        uint64_t a = phys + p;
        uint64_t va = (uint64_t)phys2virt(a) & ~0xfffULL;
        if (!(vmm_get_pte(vmm_kernel_pml4(), va) & VMM_PRESENT))
            vmm_map(vmm_kernel_pml4(), va, a & ~0xfffULL,
                    VMM_PRESENT | VMM_WRITE | VMM_PCD);
    }
    return phys2virt(phys);
}

void vmm_destroy_user(uint64_t pml4_phys) {
    // walk user half (entries 0..255), drop refs on leaves and tables;
    // shared cow pages just decrement via pmm_free
    uint64_t *pml4 = phys2virt(pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_PRESENT))
            continue;
        uint64_t *pdp = phys2virt(pml4[i] & 0x000ffffffffff000ULL);
        for (int j = 0; j < 512; j++) {
            if (!(pdp[j] & VMM_PRESENT))
                continue;
            uint64_t *pd = phys2virt(pdp[j] & 0x000ffffffffff000ULL);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_PRESENT))
                    continue;
                uint64_t *pt = phys2virt(pd[k] & 0x000ffffffffff000ULL);
                for (int m = 0; m < 512; m++)
                    if (pt[m] & VMM_PRESENT)
                        pmm_free((void *)(pt[m] & 0x000ffffffffff000ULL));
                pmm_free((void *)(pd[k] & 0x000ffffffffff000ULL));
            }
            pmm_free((void *)(pdp[j] & 0x000ffffffffff000ULL));
        }
        pmm_free((void *)(pml4[i] & 0x000ffffffffff000ULL));
        pml4[i] = 0;
    }
    pmm_free((void *)pml4_phys);
}

int vmm_page_fault(struct regs *r, uint64_t cr2) {
    (void)r;
    uint64_t cr3 = vmm_kernel_pml4();
    uint64_t pte = vmm_get_pte(cr3, cr2);
    if (!(pte & VMM_PRESENT))
        return 0;
    if (!(r->err & 2))            // not a write fault
        return 0;
    if (pte & VMM_WRITE)          // writable page faulted? kernel bug
        return 0;
    if (!(pte & VMM_COW))         // genuine read-only page (e.g. text)
        return 0;

    uint64_t phys = pte & 0x000ffffffffff000ULL;
    uint64_t flags = (pte & 0x1fe) & ~VMM_COW;
    if (pmm_refcount((void *)phys) > 1) {
        // private copy for the faulting task
        void *np = pmm_alloc();
        if (!np)
            return 0;
        memcpy(phys2virt((uint64_t)np), phys2virt(phys), PAGE_SIZE);
        pmm_free((void *)phys);
        phys = (uint64_t)np;
    }
    // last holder: keep the page, just make it writable again
    vmm_map(cr3, cr2 & ~0xfffULL, phys, flags | (pte & VMM_NX) | VMM_WRITE | VMM_PRESENT);
    return 1;
}
