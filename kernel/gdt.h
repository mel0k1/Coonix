// GDT: kernel + user segments + TSS (for ring3 kernel stack)
#pragma once
#include <stdint.h>

void gdt_init(void);

// segment selectors
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28

// called from task switch, sets TSS.rsp0
void tss_set_rsp0(uint64_t rsp);
