#include "stdio.h"
#include "coonix.h"

static void puts_n(const char *s, long len) {
    write(1, s, len);
}

void puts(const char *s) {
    long len = 0;
    while (s[len])
        len++;
    puts_n(s, len);
    puts_n("\n", 1);
}

static void print_num(long v, unsigned base, int is_signed) {
    char buf[24];
    int i = sizeof(buf) - 1;
    buf[i] = 0;
    unsigned long uv;
    if (is_signed && v < 0) {
        uv = -v;
    } else {
        uv = v;
    }
    if (uv == 0)
        buf[--i] = '0';
    while (uv) {
        int d = uv % base;
        buf[--i] = d < 10 ? '0' + d : 'a' + d - 10;
        uv /= base;
    }
    if (is_signed && v < 0)
        buf[--i] = '-';
    puts_n(&buf[i], sizeof(buf) - 1 - i);
}

void printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            puts_n(fmt, 1);
            continue;
        }
        fmt++;
        // minimal flags: '-' (left align) + decimal width for %s
        int width = 0;
        int left = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        // 'l' modifier: the arg really is long-sized
        int lmod = 0;
        if (*fmt == 'l') {
            lmod = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            long len = 0;
            while (s[len]) len++;
            if (!left)
                for (long pad = len; pad < width; pad++)
                    puts_n(" ", 1);
            puts_n(s, len);
            if (left)
                for (long pad = len; pad < width; pad++)
                    puts_n(" ", 1);
            break;
        }
        case 'c':
            puts_n(&(char){__builtin_va_arg(ap, int)}, 1);
            break;
        case 'd':
            // %d must consume an int-sized slot: va_arg(long) on an int
            // arg reads whatever the caller left in the upper half
            print_num(lmod ? __builtin_va_arg(ap, long)
                           : __builtin_va_arg(ap, int), 10, 1);
            break;
        case 'u':
            print_num(lmod ? __builtin_va_arg(ap, unsigned long)
                           : __builtin_va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            print_num(lmod ? __builtin_va_arg(ap, unsigned long)
                           : __builtin_va_arg(ap, unsigned int), 16, 0);
            break;
        case '%':
            puts_n("%", 1);
            break;
        default:
            puts_n("%", 1);
            puts_n(fmt, 1);
        }
    }
    __builtin_va_end(ap);
}
