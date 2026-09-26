// virtual memory: 4-level paging helpers
#pragma once
#include <stdint.h>

#define VMM_PRESENT 0x001
#define VMM_WRITE   0x002
#define VMM_USER    0x004
#define VMM_NX      (1ULL << 63)

uint64_t vmm_kernel_pml4(void);
uint64_t vmm_create_pml4(void);  // returns phys addr, kernel half pre-mapped
void vmm_map(uint64_t pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void vmm_unmap(uint64_t pml4, uint64_t vaddr);
uint64_t vmm_get_phys(uint64_t pml4, uint64_t vaddr); // 0 if unmapped
uint64_t vmm_get_pte(uint64_t pml4, uint64_t vaddr);
void vmm_switch(uint64_t pml4);
