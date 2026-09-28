// dlopen groundwork test: load a shared object from the ext2 disk at
// runtime through glibc's dynamic loader, bind a symbol, call it, unload
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

typedef int (*fn2)(int, int);
typedef const char *(*fn0)(void);
typedef int (*fnc)(void);

int main(void) {
    printf("dltest: dlopen /lib/libfoo.so\n");
    fflush(stdout);

    void *h = dlopen("/lib/libfoo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        printf("dltest: dlopen failed: %s\n", dlerror());
        return 1;
    }
    printf("dltest: handle %p\n", h);

    fn2 add = (fn2)dlsym(h, "foo_add");
    if (!add) {
        printf("dltest: dlsym(foo_add) failed: %s\n", dlerror());
        return 2;
    }
    int r = add(20, 22);
    printf("dltest: foo_add(20,22) = %d %s\n", r, r == 42 ? "ok" : "WRONG");

    fn0 name = (fn0)dlsym(h, "foo_name");
    if (name)
        printf("dltest: foo_name() = %s\n", name());
    else
        printf("dltest: dlsym(foo_name) failed: %s\n", dlerror());

    // exercise the lib's writable data + bss through calls
    fnc cnt = (fnc)dlsym(h, "foo_counter");
    if (cnt) {
        int a = cnt(), b = cnt();
        printf("dltest: counter %d then %d %s\n", a, b,
               (a == 1 && b == 2) ? "ok" : "WRONG");
    }

    int rc = dlclose(h);
    printf("dltest: dlclose rc=%d\n", rc);

    // reload to prove the cleanup path works
    h = dlopen("/lib/libfoo.so", RTLD_NOW);
    if (!h) {
        printf("dltest: reopen failed: %s\n", dlerror());
        return 3;
    }
    add = (fn2)dlsym(h, "foo_add");
    printf("dltest: reopen + call = %d\n", add ? add(1, 2) : -1);
    dlclose(h);

    printf("dltest: PASS\n");
    return 0;
}
