#include "console.h"
#include "font8x8.h"
#include "kernel.h"
#include "string.h"
#include "serial.h"

static uint32_t *fb;
static uint64_t pitch, width, height;
static uint32_t cx, cy, cols, rows;
static uint32_t fg = CONSOLE_FG;

void console_init(void) {
    volatile struct limine_framebuffer_response *resp = fb_request.response;
    if (!resp || resp->framebuffer_count < 1)
        return;
    struct limine_framebuffer *s = resp->framebuffers[0];
    if (s->memory_model != LIMINE_FRAMEBUFFER_RGB || s->bpp != 32)
        return; // fall back to serial
    fb = s->address;
    pitch = s->pitch;
    width = s->width;
    height = s->height;
    cols = width / 8;
    rows = height / 8;
    console_clear();
}

void console_clear(void) {
    if (!fb)
        return;
    for (uint64_t y = 0; y < height; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)fb + y * pitch);
        for (uint64_t x = 0; x < width; x++)
            line[x] = CONSOLE_BG;
    }
    cx = cy = 0;
}

static void draw_char(uint32_t px, uint32_t py, char ch) {
    const char *glyph = font8x8_basic[(uint8_t)ch & 0x7f];
    for (uint32_t gy = 0; gy < 8; gy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fb + (py + gy) * pitch);
        for (uint32_t gx = 0; gx < 8; gx++)
            if (glyph[gy] & (1 << gx))
                row[px + gx] = fg;
    }
}

static void scroll(void) {
    for (uint32_t y = 8; y < rows * 8; y++) {
        memcpy((uint8_t *)fb + (y - 8) * pitch,
               (uint8_t *)fb + y * pitch, width * 4);
    }
    uint32_t *last = (uint32_t *)((uint8_t *)fb + (rows - 1) * 8 * pitch);
    for (uint32_t i = 0; i < width * 8; i++)
        last[i] = CONSOLE_BG;
}

void console_putc(char c) {
    serial_putc(c); // mirror to COM1 always
    if (!fb)
        return;
    if (c == '\n') {
        cx = 0;
        if (++cy >= rows) { cy = rows - 1; scroll(); }
        return;
    }
    if (c == '\r') { cx = 0; return; }
    if (c == '\t') { cx = (cx + 8) & ~7; return; }
    if (c == '\b') { if (cx) cx--; return; }
    draw_char(cx * 8, cy * 8, c);
    if (++cx >= cols) {
        cx = 0;
        if (++cy >= rows) { cy = rows - 1; scroll(); }
    }
}

void console_puts(const char *s) {
    while (*s)
        console_putc(*s++);
}

void console_set_fg(uint32_t color) {
    fg = color;
}
