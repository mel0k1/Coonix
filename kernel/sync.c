#include "sync.h"
#include "task.h"
#include "kernel.h"

// one cpu: pause between polls, ticket order for fairness under load
static inline void lock_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

void spin_lock(spinlock_t *l) {
    unsigned t = __atomic_fetch_add(&l->ticket, 1, __ATOMIC_RELAXED);
    for (;;) {
        unsigned n = __atomic_load_n(&l->now, __ATOMIC_ACQUIRE);
        if (n == t)
            return;
        lock_pause();
    }
}

int spin_trylock(spinlock_t *l) {
    unsigned n = __atomic_load_n(&l->now, __ATOMIC_RELAXED);
    unsigned t = __atomic_load_n(&l->ticket, __ATOMIC_RELAXED);
    if (n != t)
        return 0;
    return __atomic_compare_exchange_n(&l->ticket, &t, t + 1, 0,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

void spin_unlock(spinlock_t *l) {
    __atomic_add_fetch(&l->now, 1, __ATOMIC_RELEASE);
}

void spin_lock_irqsave(spinlock_t *l, uint64_t *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) :: "memory");
    spin_lock(l);
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// --- mutex ---

void mutex_init(mutex_t *m) {
    m->q = (spinlock_t)SPINLOCK_INIT;
    m->owner = 0;
}

void mutex_lock(mutex_t *m) {
    for (;;) {
        spin_lock(&m->q);
        if (!m->owner) {
            m->owner = current;
            spin_unlock(&m->q);
            return;
        }
        if (m->owner == current)
            panic("mutex: recursive lock");
        spin_unlock(&m->q);
        lock_pause();
    }
}

int mutex_trylock(mutex_t *m) {
    int got = 0;
    spin_lock(&m->q);
    if (!m->owner) {
        m->owner = current;
        got = 1;
    }
    spin_unlock(&m->q);
    return got;
}

void mutex_unlock(mutex_t *m) {
    spin_lock(&m->q);
    if (m->owner != current)
        panic("mutex: unlock by non-owner");
    m->owner = 0;
    spin_unlock(&m->q);
}

// --- rwlock ---

void rw_read_lock(rwlock_t *l) {
    for (;;) {
        spin_lock(&l->q);
        if (!l->writer) {
            l->readers++;
            spin_unlock(&l->q);
            return;
        }
        spin_unlock(&l->q);
        lock_pause();
    }
}

void rw_read_unlock(rwlock_t *l) {
    spin_lock(&l->q);
    l->readers--;
    spin_unlock(&l->q);
}

void rw_write_lock(rwlock_t *l) {
    for (;;) {
        spin_lock(&l->q);
        if (!l->writer && !l->readers) {
            l->writer = 1;
            spin_unlock(&l->q);
            return;
        }
        spin_unlock(&l->q);
        lock_pause();
    }
}

void rw_write_unlock(rwlock_t *l) {
    spin_lock(&l->q);
    l->writer = 0;
    spin_unlock(&l->q);
}
