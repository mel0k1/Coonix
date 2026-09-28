// ps/2 keyboard + console line discipline (termios-lite).
// canonical mode: chars build a line in the irq, readers get it whole;
// raw mode: chars pass through the ring one read at a time.
#include "kbd.h"
#include "kernel.h"
#include "idt.h"
#include "task.h"
#include "signal.h"
#include "console.h"
#include "string.h"

#define BUF_SIZE 256
#define LINE_MAX 256

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

// terminal state (lflag bits below)
#define ISIG    0x001
#define ICANON  0x002
#define ECHO    0x008
#define ECHOE   0x010

struct ktermios tty_termios = {
    .iflag = 0x0500,          // ICRNL | IXON-ish defaults
    .oflag = 0x0005,          // OPOST | ONLCR
    .cflag = 0x00bf,          // CS8 | CREAD | B38400
    .lflag = 0x8a3b,          // ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN|ECHOCTL
    .line = 0,
    .cc = { 0x03, 0x1c, 0x7f, 0x15, 0x04, 0, 1, 0 },  // INTR QUIT ERASE KILL EOF.. VMIN=1
};

int tty_fg_pgid = 1;          // set to the shell's group at boot

static volatile char buf[BUF_SIZE];
static volatile int head, tail;

static char line[LINE_MAX];
static int line_len;
static int line_ready;        // full line (with \n) or EOF marker present
static int line_eof;          // line was ^D: zero-length read
static int line_pos;          // read offset within a ready line

static void push(char c) {
    int next = (head + 1) % BUF_SIZE;
    if (next == tail)
        return; // full, drop
    buf[head] = c;
    head = next;
}

static void line_reset(void) {
    line_len = line_ready = line_eof = line_pos = 0;
}

// canonical-mode input processing, irq context
static void line_input(char c) {
    uint32_t lflag = tty_termios.lflag;
    if (lflag & ISIG) {
        if (c == tty_termios.cc[0]) {           // VINTR ^C
            console_puts("^C\n");
            line_reset();
            signal_send_pgid(tty_fg_pgid, SIGINT);
            return;
        }
        if (c == tty_termios.cc[1]) {           // VQUIT ctrl-backslash
            console_puts("^\\\n");
            line_reset();
            signal_send_pgid(tty_fg_pgid, SIGQUIT);
            return;
        }
    }
    if (c == tty_termios.cc[4] && (lflag & ICANON)) {   // VEOF ^D
        if (!line_len) {
            line_eof = 1;
            line_ready = 1;
        }
        // ^D mid-line: commit what's typed
        else {
            line[line_len++] = '\n';
            line_ready = 1;
            if (lflag & ECHO)
                console_putc('\n');
        }
        return;
    }
    if (c == '\b' || c == 0x7f) {               // VERASE
        if (line_len) {
            line_len--;
            if (lflag & ECHOE)
                console_puts("\b \b");
        }
        return;
    }
    if (line_len >= LINE_MAX - 1)
        return;                                  // drop, no beep
    line[line_len++] = c;
    if (lflag & ECHO)
        console_putc(c);
    if (c == '\n')
        line_ready = 1;
}

// kernel-side read: -1 = nothing yet (caller blocks on WAIT_KBD)
int tty_read(char *out, int len) {
    if (tty_termios.lflag & ICANON) {
        if (!line_ready)
            return -1;
        int avail = line_len - line_pos;
        int n = len < avail ? len : avail;
        memcpy(out, line + line_pos, n);
        line_pos += n;
        if (line_pos >= line_len) {
            if (line_eof && line_pos == 0) {
                // ^D on empty line: one zero-length read, then re-arm
                line_reset();
                return 0;
            }
            line_reset();
        }
        if (line_eof && n == 0) {
            line_reset();
            return 0;
        }
        return n;
    }
    // raw: whatever accumulated
    int n = 0;
    while (n < len) {
        if (head == tail)
            break;
        out[n++] = buf[tail];
        tail = (tail + 1) % BUF_SIZE;
    }
    return n ? n : -1;
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
            if (tty_termios.lflag & ICANON)
                line_input(c);
            else
                push(c);
            task_wake_kbd();
        }
    }
}

void kbd_init(void) {
    head = tail = 0;
    line_reset();
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
