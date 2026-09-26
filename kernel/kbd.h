// PS/2 keyboard, irq1, ascii queue
#pragma once
#include <stdint.h>

void kbd_init(void);
char kbd_getchar(void);          // non-blocking, -1 if empty
char kbd_getchar_wait(void);     // blocking
