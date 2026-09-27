#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    printf("hello from real glibc on coonix, pid %d\n", (int)getpid());
    int total = 0;
    for (int i = 1; i <= 10; i++)
        total += i;
    printf("sum 1..10 = %d\n", total);
    char *heap = (char *)malloc(64);
    sprintf(heap, "malloc works too");
    puts(heap);
    return 0;
}
