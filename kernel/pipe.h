// kernel pipes: ring buffer shared by two fds (read + write end)
#pragma once
#include <stdint.h>

#define PIPE_CAP 8192

struct pipe {
    uint8_t buf[PIPE_CAP];
    volatile uint32_t ridx, widx;   // byte indices modulo-cap-safe
    volatile uint32_t count;
    int readers, writers;           // open end counts
};

// create a pipe with both ends open; returns 0 on error
struct pipe *pipe_create(void);
// drop one end; frees the pipe when both are gone
void pipe_release_end(struct pipe *p, int write_end);
// bytes available
uint32_t pipe_avail(struct pipe *p);
// non-blocking move; returns bytes moved (may be 0)
uint64_t pipe_read_nb(struct pipe *p, uint8_t *dst, uint64_t len);
uint64_t pipe_write_nb(struct pipe *p, const uint8_t *src, uint64_t len);

// wake tasks blocked on this pipe (WAIT_PIPE)
void task_wake_pipe(struct pipe *p);
