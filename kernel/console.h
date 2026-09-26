// framebuffer text console (8x8 glyphs, 32bpp)
#pragma once
#include <stdint.h>

#define CONSOLE_FG 0xd8d8d8
#define CONSOLE_BG 0x14161a
#define CONSOLE_ACCENT 0xffa640

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *s);
void console_set_fg(uint32_t color);
