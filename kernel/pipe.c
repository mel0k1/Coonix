#include "pipe.h"
#include "heap.h"
#include "string.h"

struct pipe *pipe_create(void) {
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return 0;
    p->ridx = p->widx = p->count = 0;
    p->readers = 1;
    p->writers = 1;
    return p;
}

void pipe_release_end(struct pipe *p, int write_end) {
    if (!p)
        return;
    if (write_end) {
        if (p->writers)
            p->writers--;
    } else {
        if (p->readers)
            p->readers--;
    }
    if (!p->readers && !p->writers) {
        // last end closed; wake stragglers so they see EOF/EPIPE
        task_wake_pipe(p);
        kfree(p);
    } else {
        task_wake_pipe(p);   // EOF for readers / EPIPE for writers
    }
}

uint32_t pipe_avail(struct pipe *p) {
    return p->count;
}

uint64_t pipe_read_nb(struct pipe *p, uint8_t *dst, uint64_t len) {
    uint64_t moved = 0;
    while (moved < len && p->count) {
        dst[moved++] = p->buf[p->ridx];
        p->ridx = (p->ridx + 1) % PIPE_CAP;
        p->count--;
    }
    return moved;
}

uint64_t pipe_write_nb(struct pipe *p, const uint8_t *src, uint64_t len) {
    uint64_t moved = 0;
    while (moved < len && p->count < PIPE_CAP) {
        p->buf[p->widx] = src[moved++];
        p->widx = (p->widx + 1) % PIPE_CAP;
        p->count++;
    }
    return moved;
}
