// kernel-wide basics: limine requests, phys/virt helpers
#pragma once

#include <stdint.h>
#include "limine.h"

#define KERNEL_VIRT 0xffffffff80000000UL
#define KERNEL_PHYS 0x200000UL
#define PAGE_SIZE   4096UL

// higher half direct map offset
uint64_t hhdm_offset(void);
void *phys2virt(uint64_t phys);
uint64_t virt2phys(void *virt);

// limine requests live in kmain.c
extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_framebuffer_request fb_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_stack_size_request stack_size_request;

void panic(const char *msg) __attribute__((noreturn));

// io ports
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) {
    outb(0x80, 0);
}
static inline void cli(void) {
    __asm__ volatile("cli");
}
static inline void hlt(void) {
    __asm__ volatile("hlt");
}
static inline void enable_nxe(void) {
    uint64_t efer;
    __asm__ volatile("rdmsr" : "=A"(efer) : "c"(0xC0000080));
    efer |= (1UL << 11);
    __asm__ volatile("wrmsr" :: "A"(efer), "c"(0xC0000080));
}

