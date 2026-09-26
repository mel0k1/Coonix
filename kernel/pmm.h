// physical memory manager: bitmap over limine memmap
#pragma once
#include <stdint.h>

void pmm_init(void);
void *pmm_alloc(void);           // 1 page, dirty
void *pmm_alloc_zeroed(void);    // 1 page, zeroed via hhdm
void pmm_free(void *page);

uint64_t pmm_total_mem(void);    // bytes usable
uint64_t pmm_free_mem(void);     // bytes free
