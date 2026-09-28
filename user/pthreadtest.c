// pthreadtest: real glibc threads on Coonix.
// exercises SYS_clone (CLONE_THREAD/SETTLS/PARENT_SETTID/CHILD_CLEARTID),
// futex wait/wake via mutex contention and condvar signaling, thread exit
// + join via clear_child_tid.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NT 4
#define LOOPS 4000
#define FLIPS 60

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter;
static int turn;                       // condvar ping-pong state
static volatile int flips;

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < LOOPS; i++) {
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
    }
    return (void *)(id + 10);
}

static void *pinger(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < FLIPS; i++) {
        while (turn != 0)
            pthread_cond_wait(&cv, &mtx);
        turn = 1;
        flips++;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mtx);
    return 0;
}

static void *ponger(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < FLIPS; i++) {
        while (turn != 1)
            pthread_cond_wait(&cv, &mtx);
        turn = 0;
        flips++;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mtx);
    return 0;
}

int main(void) {
    pthread_t th[NT], tp1, tp2;

    for (long i = 0; i < NT; i++)
        if (pthread_create(&th[i], 0, worker, (void *)i) != 0) {
            printf("pthreadtest: create failed\n");
            return 1;
        }
    if (pthread_create(&tp1, 0, pinger, 0) != 0 ||
        pthread_create(&tp2, 0, ponger, 0) != 0) {
        printf("pthreadtest: ping/pong create failed\n");
        return 1;
    }

    long sum = 0;
    for (int i = 0; i < NT; i++) {
        void *ret;
        pthread_join(th[i], &ret);
        sum += (long)ret;
    }
    pthread_join(tp1, 0);
    pthread_join(tp2, 0);

    long expect = (long)NT * LOOPS;
    long retsum = 10L * NT + (long)NT * (NT - 1) / 2;   // each id + 10
    printf("pthreadtest: counter %ld (want %ld), flips %d (want %d)\n",
           counter, expect, flips, FLIPS * 2);
    if (counter != expect || flips != FLIPS * 2 || sum != retsum) {
        printf("pthreadtest: FAIL\n");
        return 1;
    }
    printf("pthreadtest: OK\n");
    return 0;
}
