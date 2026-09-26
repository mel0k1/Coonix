#include "kbd.h"
#include "kernel.h"
#include "idt.h"
#include "task.h"
#include "serial.h"

#define BUF_SIZE 256

static volatile char buf[BUF_SIZE];
static volatile int head, tail;

// us qwerty, make codes 0x02..0x35
static const char map_norm[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};
static const char map_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static void push(char c) {
    int next = (head + 1) % BUF_SIZE;
    if (next == tail)
        return; // full, drop
    buf[head] = c;
    head = next;
}

static void kbd_irq(struct regs *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    static int shift;
    static int escaped;

    if (sc == 0xe0) { escaped = 1; return; }
    if (escaped) {
        escaped = 0;
        return; // arrows etc: ignore for now
    }
    if (sc == 0x2a || sc == 0x36) { shift = 1; return; }
    if (sc == 0xaa || sc == 0xb6) { shift = 0; return; }
    if (sc & 0x80)
        return; // break code

    if (sc < sizeof(map_norm)) {
        char c = shift ? map_shift[sc] : map_norm[sc];
        if (c) {
            push(c);
            task_wake_kbd();
        }
    }
}

void kbd_init(void) {
    head = tail = 0;
    irq_install(1, kbd_irq);
}

char kbd_getchar(void) {
    if (head == tail)
        return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return c;
}

char kbd_getchar_wait(void) {
    for (;;) {
        char c = kbd_getchar();
        if (c >= 0)
            return c;
        hlt();
    }
}
