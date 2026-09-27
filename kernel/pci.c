#include "pci.h"

static inline void outl(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p) : "memory");
}

static inline uint32_t inl(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

#define CFG_ADDR 0xcf8
#define CFG_DATA 0xcfc

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) |
                    (off & 0xfc);
    outl(CFG_ADDR, addr);
    outl(CFG_DATA, v);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) |
                    (off & 0xfc);
    outl(CFG_ADDR, addr);
    return inl(CFG_DATA);
}

int pci_find_class(uint8_t class, uint8_t subclass,
                   uint8_t *bus, uint8_t *dev, uint8_t *fn, uint32_t *bar5) {
    for (uint16_t b = 0; b < 16; b++)
        for (uint8_t d = 0; d < 32; d++)
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0x00);
                if (id == 0xffffffff)
                    continue;
                uint32_t cc = pci_read32(b, d, f, 0x08); // [31:24] class, [23:16] subclass
                if ((cc >> 16) == ((uint32_t)class << 8 | subclass)) {
                    if (bus) *bus = b;
                    if (dev) *dev = d;
                    if (fn) *fn = f;
                    if (bar5) {
                        uint32_t v = pci_read32(b, d, f, 0x24);
                        *bar5 = v & 0xfffffff0u;
                    }
                    return 1;
                }
            }
    return 0;
}
