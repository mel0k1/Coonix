#include "pmm.h"
#include "kernel.h"
#include "console.h"
#include "string.h"

// limine v9 memmap types
#define TYPE_USABLE 0
#define TYPE_BL_RECLAIMABLE 5

#define MAX_REGIONS 32

struct region {
    uint64_t base, len;
};

static struct region usable[MAX_REGIONS];
static int nregions;

static uint8_t *bitmap;         // 1 bit per page, set = used
static uint64_t bitmap_pages;
static uint16_t *refs;          // sharers per page (cow fork)
static uint64_t base, pages_total, pages_used;

static inline int bit_test(uint64_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

static inline void bit_set(uint64_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bit_clear(uint64_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

void pmm_init(void) {
    volatile struct limine_memmap_response *mm = memmap_request.response;
    if (!mm)
        panic("pmm: no memmap");

    // snapshot usable regions first, memmap is precious
    // keep only regions inside [1MB, 4GB): low memory is crowded with
    // limine/bios structures, above 4G qemu may expose phantom windows
    uint64_t min = ~0ULL, max = 0;
    for (uint64_t i = 0; i < mm->entry_count && nregions < MAX_REGIONS; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != TYPE_USABLE && e->type != TYPE_BL_RECLAIMABLE)
            continue;
        if (e->base < 0x100000ULL || e->base >= 0x100000000ULL)
            continue;
        uint64_t len = e->length;
        if (e->base + len > 0x100000000ULL)
            len = 0x100000000ULL - e->base;
        usable[nregions].base = e->base;
        usable[nregions].len = len;
        nregions++;
        if (e->base < min)
            min = e->base;
        if (e->base + len > max)
            max = e->base + len;
    }
    if (!nregions)
        panic("pmm: no usable memory");

    base = min & ~0xfffULL;
    pages_total = (max - base) >> 12;
    uint64_t bitmap_bytes = (pages_total + 7) / 8;
    bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) >> 12;
    uint64_t refs_bytes = pages_total * sizeof(uint16_t);

    // place bitmap + refcount array in the tail of the biggest usable region
    int best = -1;
    uint64_t best_len = 0;
    for (int i = 0; i < nregions; i++) {
        if (usable[i].len > best_len) {
            best_len = usable[i].len;
            best = i;
        }
    }
    if (best < 0 || usable[best].len < bitmap_bytes + refs_bytes)
        panic("pmm: no room for bitmap");

    uint64_t bitmap_phys = (usable[best].base + usable[best].len - bitmap_bytes) & ~0xfffULL;
    uint64_t refs_phys = bitmap_phys - ((refs_bytes + PAGE_SIZE - 1) & ~0xfffULL);
    bitmap = phys2virt(bitmap_phys);
    memset(bitmap, 0xff, bitmap_bytes);
    refs = phys2virt(refs_phys);
    memset(refs, 0, refs_bytes);

    // free pages of all usable regions except bitmap pages
    for (int i = 0; i < nregions; i++) {
        uint64_t start = (usable[i].base + PAGE_SIZE - 1) & ~0xfffULL;
        uint64_t end = (usable[i].base + usable[i].len) & ~0xfffULL;
        for (uint64_t p = start; p < end; p += PAGE_SIZE) {
            uint64_t idx = (p - base) >> 12;
            bit_clear(idx);
        }
    }
    // mark bitmap + refs pages used
    for (uint64_t p = refs_phys; p < bitmap_phys + bitmap_pages * PAGE_SIZE; p += PAGE_SIZE)
        bit_set((p - base) >> 12);

    pages_used = (bitmap_phys + bitmap_pages * PAGE_SIZE - refs_phys) / PAGE_SIZE;
}

void *pmm_alloc(void) {
    for (uint64_t p = 0; p < pages_total; p++) {
        if (!bit_test(p)) {
            bit_set(p);
            refs[p] = 1;
            pages_used++;
            return (void *)(base + p * PAGE_SIZE);
        }
    }
    return 0;
}

void *pmm_alloc_zeroed(void) {
    void *page = pmm_alloc();
    if (page)
        memset(phys2virt((uint64_t)page), 0, PAGE_SIZE);
    return page;
}

void pmm_free(void *page) {
    uint64_t p = ((uint64_t)page - base) >> 12;
    if (refs[p] > 1) {
        refs[p]--;         // still shared (cow), keep allocated
        return;
    }
    refs[p] = 0;
    if (bit_test(p)) {
        bit_clear(p);
        pages_used--;
    }
}

void pmm_ref(void *page) {
    uint64_t p = ((uint64_t)page - base) >> 12;
    if (refs[p] < 0xffff)
        refs[p]++;
}

int pmm_refcount(void *page) {
    uint64_t p = ((uint64_t)page - base) >> 12;
    return refs[p];
}

uint64_t pmm_total_mem(void) {
    return pages_total * PAGE_SIZE;
}

uint64_t pmm_free_mem(void) {
    return (pages_total - pages_used) * PAGE_SIZE;
}
