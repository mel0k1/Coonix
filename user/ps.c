// ps: list running processes from /proc (PID PPID S COMMAND)
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int is_num(const char *s) {
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

static int to_num(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

// parse "/proc/<pid>/stat": "<pid> (<comm>) <state> <ppid> ..."
// returns 1 on success
static int read_stat(int pid, char *comm, int comm_cap, char *state,
                     int *ppid) {
    char path[48];
    char buf[512];
    // build the path
    {
        char *w = path;
        const char *src = "/proc/";
        while (*src)
            *w++ = *src++;
        // decimal pid
        char digits[12];
        int n = 0;
        int v = pid;
        do {
            digits[n++] = '0' + v % 10;
            v /= 10;
        } while (v);
        while (n)
            *w++ = digits[--n];
        const char *tail = "/stat";
        while (*tail)
            *w++ = *tail++;
        *w = 0;
    }

    long fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long n = read(fd, buf, (int)sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;

    // comm sits between the first '(' and the last ')': comm may contain
    // ')' itself (truncated names), linux parses the same way
    char *lp = 0, *rp = 0;
    for (char *q = buf; *q; q++) {
        if (*q == '(' && !lp)
            lp = q;
        if (*q == ')')
            rp = q;
    }
    if (!lp || !rp || rp < lp)
        return 0;
    long clen = rp - lp - 1;
    if (clen > comm_cap - 1)
        clen = comm_cap - 1;
    memcpy(comm, lp + 1, clen);
    comm[clen] = 0;

    // after ')': " S <ppid> ..."
    char *q = rp + 1;
    while (*q == ' ')
        q++;
    if (!*q)
        return 0;
    *state = *q++;
    while (*q == ' ')
        q++;
    *ppid = to_num(q);
    return 1;
}

static void print_row(int pid, int ppid, char state, const char *comm) {
    // right-aligned 5-wide numbers (printf here has no width for %d)
    char spid[8], sppid[8];
    int np = 0, npp = 0;
    int v = pid;
    do {
        spid[np++] = '0' + v % 10;
        v /= 10;
    } while (v);
    v = ppid;
    do {
        sppid[npp++] = '0' + v % 10;
        v /= 10;
    } while (v);
    printf("  ");
    for (int i = 5; i > np; i--)
        printf(" ");
    while (np)
        printf("%c", spid[--np]);
    printf(" ");
    for (int i = 5; i > npp; i--)
        printf(" ");
    while (npp)
        printf("%c", sppid[--npp]);
    printf(" %c %s\n", state, comm);
}

int main(void) {
    printf("  PID  PPID S COMMAND\n");

    long fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        printf("ps: cannot open /proc\n");
        return 1;
    }

    char dbuf[2048];
    int listed = 0;
    long n;
    while ((n = getdents64(fd, dbuf, (int)sizeof(dbuf))) > 0) {
        long off = 0;
        while (off < n) {
            struct coonix_dirent64 *d =
                (struct coonix_dirent64 *)(dbuf + off);
            off += d->d_reclen;
            if (d->d_type != DT_DIR || !is_num(d->d_name))
                continue;
            int pid = to_num(d->d_name);
            char comm[16];
            char state = '?';
            int ppid = 0;
            if (read_stat(pid, comm, (int)sizeof(comm), &state, &ppid)) {
                print_row(pid, ppid, state, comm);
                listed++;
            }
        }
    }
    close(fd);
    if (!listed)
        printf("(no processes)\n");
    return 0;
}
