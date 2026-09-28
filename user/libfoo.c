// freestanding shared object for the dlopen test: no libc dependency,
// exercises RELATIVE/GLOB_DAT relocations, .data and .bss in a loaded .so
int foo_add(int a, int b) {
    return a + b;
}

static const char namebuf[] = "coonix dlopen ok";
const char *foo_name(void) {
    return namebuf;
}

int foo_counter(void) {
    static int c;   // lands in .bss of the shared object
    return ++c;
}

static int shared_state = 7;   // .data of the shared object

int foo_state(void) {
    return shared_state;
}

void foo_setstate(int v) {
    shared_state = v;
}
