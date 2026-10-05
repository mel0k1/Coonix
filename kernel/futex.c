#include "futex.h"
#include "task.h"
#include "vmm.h"
#include "pit.h"
#include "string.h"
#include "heap.h"
#include "kernel.h"

#define FUT_HASH 64

void task_frame_syscall_result(struct task *t, long ret);

struct futex_q {
    struct task *t;
    uint64_t key;
    uint64_t deadline;    // tick units, 0 = forever
    int bitset;
    struct futex_q *next;
};

static struct futex_q *buckets[FUT_HASH];

void futex_init(void) {
    memset(buckets, 0, sizeof(buckets));
}

uint64_t futex_key_of(uint64_t uaddr) {
    uint64_t page_phys = vmm_get_phys(current->pml4, uaddr & ~(PAGE_SIZE - 1));
    if (!page_phys)
        return 0;
    return page_phys | (uaddr & 0xfff);
}

static struct futex_q **bucket_of(uint64_t key) {
    return &buckets[(key >> 12) % FUT_HASH];
}

// drop a task's queue entry: signal wakes, syscall replay and task death
// must not leave a stale node behind, or a later futex_wake burns a wake
// slot on it (lost wakeup for a real waiter) and the node leaks
static void futex_cancel(struct task *t) {
    struct futex_q *q = (struct futex_q *)t->futex_ent;
    if (!q)
        return;
    t->futex_ent = 0;
    struct futex_q **pp = bucket_of(q->key);
    while (*pp) {
        if (*pp == q) {
            *pp = q->next;
            kfree(q);
            return;
        }
        pp = &(*pp)->next;
    }
}

void futex_cancel_wait(struct task *t) {
    // flags save/restore: callers may already run with cli (exit paths)
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl));
    futex_cancel(t);
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory");
}

uint64_t futex_wait_key(uint64_t key, uint64_t deadline, uint64_t frame_rsp) {
    // interrupts off for the whole enqueue+block: the tick must not land
    // between the state change and the schedule pick, or the task ends up
    // schedulable with a kernel context instead of its syscall frame
    cli();
    // a signal-woken waiter re-executes the syscall here with its old
    // entry still queued: drop it before queueing a fresh one
    futex_cancel(current);
    struct futex_q *q = kmalloc(sizeof(*q));
    if (!q) {
        // out of memory: fail the syscall in-place, stay runnable
        sti();
        task_frame_syscall_result(current, -12);   // -ENOMEM
        return frame_rsp;
    }
    q->t = current;
    q->key = key;
    q->deadline = deadline;
    q->bitset = FUT_BITSET_ANY;
    struct futex_q **head = bucket_of(key);
    q->next = *head;          // LIFO wake is fine for round-robin tests
    *head = q;
    current->futex_ent = q;
    // deadlines live in q->deadline (futex_tick): drop any leftover sleep
    // state so the tick never fires a stale WAIT_SLEEP wake into the
    // futex frame
    current->wake_tick = 0;
    current->wait_reason = WAIT_NONE;
    current->rsp = frame_rsp;      // resume here when woken
    current->state = T_BLOCKED;
    // the wake (or timeout) stuffs the syscall result straight into the
    // saved frame, and the resume is a straight iretq to user: this call
    // NEVER comes back. the scheduler's return value (the next task's
    // frame rsp) must flow up to the entry asm — continuing to run here
    // would execute this dead kernel chain on the idle's context and
    // iretq to user with kernel cr3
    return task_schedule(0);
}

// deliver a syscall result straight into a blocked task's saved frame.
// blocking syscalls were replay-rewound (rip points at the int/syscall
// insn); a stuffed result skips the re-execution, so unwind the rewind
void task_frame_syscall_result(struct task *t, long ret) {
    struct regs *r = (struct regs *)t->rsp;
    r->rax = (uint64_t)ret;
    if (r->int_no == 128 || r->int_no == 64)
        r->rip += 2;
    // the frame leaves the kernel for user right away: any rewind marker
    // it still carries is stale and must not leak into a later delivery
    t->sig_woke_rewind = 0;
}

static int wake_match(int n, uint64_t key, int bitset) {
    int woken = 0;
    int lim = n < 0 ? -1 : n;
    struct futex_q **pp = bucket_of(key);
    while (*pp && (lim < 0 || woken < lim)) {
        struct futex_q *q = *pp;
        if (q->key == key && (bitset == (int)FUT_BITSET_ANY || (q->bitset & bitset) != 0)) {
            *pp = q->next;
            q->t->futex_ent = 0;
            // a task woken by a signal while still queued is T_READY:
            // leave its frame alone, its resume path reports the result
            if (q->t->state == T_BLOCKED) {
                task_frame_syscall_result(q->t, 0);
                q->t->state = T_READY;
            }
            kfree(q);
            woken++;
        } else {
            pp = &q->next;
        }
    }
    return woken;
}

int futex_wake_key(uint64_t key, int n) {
    return wake_match(n, key, FUT_BITSET_ANY);
}

int futex_requeue_key(uint64_t key1, uint64_t key2, int n) {
    // wake n from key1, then move remaining key1 waiters to key2
    int woken = wake_match(n, key1, FUT_BITSET_ANY);
    int moved = 0;
    struct futex_q **pp = bucket_of(key1);
    while (*pp) {
        struct futex_q *q = *pp;
        if (q->key == key1) {
            *pp = q->next;
            q->key = key2;
            struct futex_q **h2 = bucket_of(key2);
            q->next = *h2;
            *h2 = q;
            moved++;
        } else {
            pp = &q->next;
        }
    }
    return woken + moved;
}

void futex_wake_addr(uint64_t uaddr, int n) {
    uint64_t key = futex_key_of(uaddr);
    if (key)
        futex_wake_key(key, n);
}

void futex_tick(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < FUT_HASH; i++) {
        struct futex_q **pp = &buckets[i];
        while (*pp) {
            struct futex_q *q = *pp;
            if (q->deadline && now >= q->deadline) {
                *pp = q->next;
                q->t->futex_ent = 0;
                if (q->t->state == T_BLOCKED) {
                    task_frame_syscall_result(q->t, -110);   // -ETIMEDOUT
                    q->t->state = T_READY;
                }
                kfree(q);
            } else {
                pp = &q->next;
            }
        }
    }
}

// timespec {sec, nsec} at 100 Hz ticks; absolute or relative
uint64_t futex_deadline_from_timespec(const void *ts, int absolute) {
    if (!ts)
        return 0;
    uint64_t sec = *(const uint64_t *)ts;
    uint64_t nsec = *((const uint64_t *)ts + 1);
    uint64_t t = sec * 100 + nsec / 10000000;
    if (absolute)
        return t ? t : 1;   // 0 means "no deadline", past due is 1
    return pit_ticks() + (t ? t : 1);
}
