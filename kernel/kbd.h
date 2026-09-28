// PS/2 keyboard + console line discipline
#pragma once
#include <stdint.h>

void kbd_init(void);
char kbd_getchar(void);          // non-blocking, -1 if empty
char kbd_getchar_wait(void);     // blocking

// kernel termios (36 bytes, same layout the linux TCGETS ioctl uses)
struct ktermios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line;
    uint8_t cc[19];
} __attribute__((packed));

_Static_assert(sizeof(struct ktermios) == 36, "termios size");

extern struct ktermios tty_termios;
extern int tty_fg_pgid;

// -1 = nothing to read yet (caller blocks), else bytes copied (0 = eof)
int tty_read(char *out, int len);
