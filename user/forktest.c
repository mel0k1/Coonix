// forktest: proves copy-on-write isolation after fork
// children scribble over shared pages, parent data must stay intact
#include "stdio.h"
#include "string.h"
#include "coonix.h"

// 8k static buffer spans two pages, both become cow on fork
static char data[8192];

int main(void) {
    strcpy(data, "enot-original");
    printf("[forktest] parent pid %d, forking 3 children\n", (int)getpid());

    for (int i = 1; i <= 3; i++) {
        long pid = fork();
        if (pid == 0) {
            // write own pattern -> triggers cow faults
            for (int j = 0; j < 200; j++)
                data[j] = 'A' + i;
            data[21] = 0; // terminate our private copy for printing
            printf("[forktest] child %d (pid %d) sees %s\n",
                   i, (int)getpid(), data);
            exit(i);
        }
    }

    int st;
    for (int i = 1; i <= 3; i++)
        wait(&st);

    if (!strcmp(data, "enot-original")) {
        printf("[forktest] parent data intact: cow works\n");
        return 0;
    }
    printf("[forktest] parent data corrupted: %.20s\n", data);
    return 1;
}
