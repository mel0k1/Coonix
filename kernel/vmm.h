// virtual memory: 4-level paging helpers
#pragma once
#include <stdint.h>
#include "idt.h"

#define VMM_PRESENT 0x001
#define VMM_WRITE   0x002
#define VMM_USER    0x004
#define VMM_PCD     0x010   // cache-disable (MMIO regions)
#define VMM_NX      (1ULL << 63)
// software pte bit (bit 9, cpu ignores it): page is cow-shared
#define VMM_COW     0x200

// top of the user half (all user pointers must end below this)
#define VMM_USER_LIMIT 0x800000000000ULL

uint64_t vmm_kernel_pml4(void);
uint64_t vmm_create_pml4(void);  // returns phys addr, kernel half pre-mapped
void vmm_map(uint64_t pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void vmm_unmap(uint64_t pml4, uint64_t vaddr);
uint64_t vmm_get_phys(uint64_t pml4, uint64_t vaddr); // 0 if unmapped
uint64_t vmm_get_pte(uint64_t pml4, uint64_t vaddr);
// 1 if [uaddr, uaddr+len) is fully mapped user memory; need_write also
// accepts cow pages (a write fault resolves them)
int vmm_user_range_ok(uint64_t pml4_phys, uint64_t uaddr, uint64_t len,
                      int need_write);
// change protection flags of a mapped range
void vmm_mprotect(uint64_t pml4, uint64_t vaddr, uint64_t pages, uint64_t flags);
void vmm_switch(uint64_t pml4);

// map an MMIO physical range (cache-disabled) into the kernel half
void *mmio_map(uint64_t phys, uint64_t size);

// free all user-half pages + tables + the pml4 itself
void vmm_destroy_user(uint64_t pml4);

// cow resolution on #PF; 1 = handled, 0 = real fault
int vmm_page_fault(struct regs *r, uint64_t cr2);
