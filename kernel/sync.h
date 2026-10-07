// kernel lock primitives
#pragma once
#include <stdint.h>

struct task;

// ticket spinlock: fifo fairness, bounded wait. short sections only —
// kernel context cannot sleep in this kernel (blocking is syscall-replay
// only), so a held lock must never be waited on with anything but spins
typedef struct {
    volatile unsigned ticket;
    volatile unsigned now;
} spinlock_t;

#define SPINLOCK_INIT {0, 0}

void spin_lock(spinlock_t *l);
int spin_trylock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
// variants that keep interrupts off across the section (irq handlers
// taking the same lock would otherwise self-deadlock on one cpu)
void spin_lock_irqsave(spinlock_t *l, uint64_t *flags);
void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);

// adaptive mutex: owner-tracked, spins with pause between owner polls.
// recursion panics (a self-deadlock caught early beats a hang)
typedef struct {
    spinlock_t q;
    struct task *owner;
} mutex_t;

#define MUTEX_INIT {{0, 0}, 0}

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

// reader-writer lock: read path is a counter under a brief spin, writer
// is exclusive. readers may starve writers (fine for cache-like use)
typedef struct {
    spinlock_t q;
    int readers;
    int writer;
} rwlock_t;

#define RWLOCK_INIT {{0, 0}, 0, 0}

void rw_read_lock(rwlock_t *l);
void rw_read_unlock(rwlock_t *l);
void rw_write_lock(rwlock_t *l);
void rw_write_unlock(rwlock_t *l);
