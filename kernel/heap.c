#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "string.h"
#include "serial.h"

struct chunk {
    size_t size;
    struct chunk *next;
    int used;
};

#define HEAP_CHUNK_PAGES 16   // grow heap by 64k at a time

static struct chunk *head;
static uint64_t heap_pml4;

// kernel heap is mapped at 0xffff900000000000 (below hhdm, inside kernel half)
#define HEAP_BASE 0xffff900000000000ULL

extern uint64_t vmm_kernel_pml4(void);
extern void vmm_map(uint64_t pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

static void heap_grow(size_t bytes) {
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    // find tail; keep it page aligned so a chunk never spans an
    // unmapped page (its data end must stay inside the last mapped page)
    uint64_t tail_vaddr = HEAP_BASE;
    if (head) {
        struct chunk *c = head;
        while (c->next) c = c->next;
        tail_vaddr = ((uint64_t)c) + sizeof(struct chunk) + c->size;
        tail_vaddr = (tail_vaddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }
    for (uint64_t i = 0; i < npages; i++) {
        void *p = pmm_alloc();
        if (!p)
            panic("heap: out of memory");
        vmm_map(heap_pml4, tail_vaddr + i * PAGE_SIZE, (uint64_t)p,
                VMM_PRESENT | VMM_WRITE);
    }
    struct chunk *c = (struct chunk *)tail_vaddr;
    c->size = npages * PAGE_SIZE - sizeof(struct chunk);
    c->used = 0;
    c->next = 0;
    if (!head) {
        head = c;
    } else {
        struct chunk *t = head;
        while (t->next) t = t->next;
        t->next = c;
    }
}

void heap_init(void) {
    heap_pml4 = vmm_kernel_pml4();
    head = 0;
    heap_grow(HEAP_CHUNK_PAGES * PAGE_SIZE);
}

void *kmalloc(size_t size) {
    if (!size)
        return 0;
    size = (size + 15) & ~15UL; // 16-byte align

    // first fit
    for (struct chunk *c = head; c; c = c->next) {
        if (!c->used && c->size >= size) {
            // split if plenty of room
            if (c->size > size + sizeof(struct chunk) + 16) {
                struct chunk *rest = (struct chunk *)((uint64_t)c + sizeof(struct chunk) + size);
                rest->size = c->size - size - sizeof(struct chunk);
                rest->used = 0;
                rest->next = c->next;
                c->next = rest;
                c->size = size;
            }
            c->used = 1;
            return (void *)((uint64_t)c + sizeof(struct chunk));
        }
    }
    heap_grow(size + sizeof(struct chunk));
    return kmalloc(size); // retry once on fresh space
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr) {
    if (!ptr)
        return;
    struct chunk *c = (struct chunk *)((uint64_t)ptr - sizeof(struct chunk));
    c->used = 0;
    // simple coalesce forward
    struct chunk *n;
    while ((n = c->next) && !n->used && (uint64_t)n == (uint64_t)c + sizeof(struct chunk) + c->size) {
        c->size += sizeof(struct chunk) + n->size;
        c->next = n->next;
    }
}
