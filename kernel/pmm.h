// physical memory manager: bitmap over limine memmap
#pragma once
#include <stdint.h>

void pmm_init(void);
// mark a physical range as used (kernel image, limine modules)
void pmm_reserve_range(uint64_t phys, uint64_t len);
void *pmm_alloc(void);           // 1 page, dirty
void *pmm_alloc_zeroed(void);    // 1 page, zeroed via hhdm
void *pmm_alloc_contig(unsigned n);  // n physically contiguous pages
void pmm_free(void *page);       // drops one reference, frees at zero

void pmm_ref(void *page);        // +1 sharer (copy-on-write fork)
int pmm_refcount(void *page);    // sharers of a page

uint64_t pmm_total_mem(void);    // bytes usable
uint64_t pmm_free_mem(void);     // bytes free
