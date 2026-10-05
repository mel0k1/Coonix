// futex: wait queues hashed by physical address of the uaddr word.
// threads share the address space, processes map the same page at
// different virtual addresses — physical keying makes both work.
#pragma once
#include <stdint.h>

struct task;

void futex_init(void);

// op values we honor (after stripping FUTEX_PRIVATE_FLAG)
#define FUT_WAIT          0
#define FUT_WAKE          1
#define FUT_REQUEUE       3
#define FUT_CMP_REQUEUE   4
#define FUT_WAKE_OP       5
#define FUT_WAIT_BITSET   9
#define FUT_WAKE_BITSET  10
#define FUT_PRIVATE_FLAG 128
#define FUT_CLOCK_REALTIME 256
#define FUT_BITSET_ANY   0xffffffff

// ms deadline helper: ticks run at 100 Hz
uint64_t futex_deadline_from_timespec(const void *ts, int absolute);

// block the calling task until woken or deadline. caller has already
// checked *uaddr == val. the wake/timeout stuffs the syscall result into
// the saved frame; the return value is the frame rsp the entry asm must
// iretq from (this call never "comes back" — treat it as noreturn)
uint64_t futex_wait_key(uint64_t key, uint64_t deadline, uint64_t frame_rsp);
// wake up to n waiters on key (n < 0 = all); returns woken count
int futex_wake_key(uint64_t key, int n);
// wake n from key1, move the rest to key2; returns moved count
int futex_requeue_key(uint64_t key1, uint64_t key2, int n);
// expire deadlines (called from the timer tick)
void futex_tick(void);
// unlink a task's queue entry (signal wake, task death). no-op if none
void futex_cancel_wait(struct task *t);
// futex-wake by user address (clear_child_tid path)
void futex_wake_addr(uint64_t uaddr, int n);
// physical key for a user word, 0 if unmapped
uint64_t futex_key_of(uint64_t uaddr);
