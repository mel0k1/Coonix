// kernel heap: linked-list allocator over pmm pages
#pragma once
#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);
