// COM1 debug output, our best friend when video is broken
#pragma once
void serial_init(void);
#include <stdint.h>
void serial_putc(char c);
void serial_puts(const char *s);
void serial_putdig(long v);
void serial_puthex(uint64_t v);
