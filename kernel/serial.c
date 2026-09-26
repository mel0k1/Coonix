#include "serial.h"
#include "kernel.h"
#include <stdint.h>

#define COM1 0x3f8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   // no interrupts
    outb(COM1 + 3, 0x80);   // DLAB on
    outb(COM1 + 0, 0x01);   // 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   // 8N1
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x0b);
}

void serial_putc(char c) {
    if (c == '\n')
        serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s)
        serial_putc(*s++);
}

void serial_putdig(long v) {
    if (v < 0) {
        serial_putc('-');
        v = -v;
    }
    char buf[12];
    int i = 11;
    buf[i] = 0;
    if (!v)
        buf[--i] = '0';
    while (v) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    serial_puts(&buf[i]);
}

void serial_puthex(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    buf[16] = 0;
    serial_puts(buf);
}
