// regression test for the userspace porting set: cwd, namei, getdents,
// pipes, fds. all via the mini libc wrappers (int 0x80)
#include "stdio.h"
#include "string.h"
#include "coonix.h"

static int fails;

#define CHECK(cond, msg) do { \
    if (cond) printf("ok: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void) {
    // --- namei: mkdir/rmdir/creat/write/rename/unlink ---
    long r = mkdir("/tmp_x", 0755);
    CHECK(r == 0, "mkdir /tmp_x");

    long fd = open("/tmp_x/f1", O_CREAT | O_RDWR, 0644);
    CHECK(fd >= 0, "open O_CREAT");
    const char *msg = "hello coonix fs";
    CHECK(write(fd, msg, strlen(msg)) == (long)strlen(msg), "write");
    close(fd);

    CHECK(rename("/tmp_x/f1", "/tmp_x/f2") == 0, "rename f1->f2");
    fd = open("/tmp_x/f2", O_RDONLY, 0);
    CHECK(fd >= 0, "open renamed");
    char buf[64];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    CHECK(n == (long)strlen(msg) && !strcmp(buf, msg), "content survives rename");

    CHECK(unlink("/tmp_x/f2") == 0, "unlink f2");
    CHECK(open("/tmp_x/f2", O_RDONLY, 0) < 0, "f2 gone");

    // --- getdents64 ---
    fd = open("/bin", O_RDONLY, 0);
    CHECK(fd >= 0, "open /bin");
    char dbuf[2048];
    n = getdents64(fd, dbuf, sizeof(dbuf));
    CHECK(n > 0, "getdents64 /bin");
    int saw_shell = 0, entries = 0;
    long off = 0;
    while (off < n) {
        struct coonix_dirent64 *d = (struct coonix_dirent64 *)(dbuf + off);
        if (!strcmp(d->d_name, "shell"))
            saw_shell = 1;
        entries++;
        off += d->d_reclen;
    }
    CHECK(saw_shell, "dentry 'shell' present");
    printf("  (%d entries)\n", entries);
    close(fd);

    // --- symlinks: create + resolve through exec path semantics ---
    CHECK(symlink("/bin/shell", "/bin/xsh") == 0, "symlink /bin/xsh");
    CHECK(symlink("/bin/shell", "/tmp_x/relsh") == 0, "symlink in dir");
    CHECK(access("/bin/xsh", 0) == 0, "access through symlink");
    CHECK(unlink("/bin/xsh") == 0, "unlink symlink");
    CHECK(unlink("/tmp_x/relsh") == 0, "unlink symlink 2");

    // --- cwd: chdir/getcwd round trip ---
    char cwd0[128], cwd1[128];
    CHECK(getcwd(cwd0, sizeof(cwd0)) > 0, "getcwd");
    CHECK(chdir("/tmp_x") == 0, "chdir /tmp_x");
    CHECK(getcwd(cwd1, sizeof(cwd1)) > 0 && !strcmp(cwd1, "/tmp_x"),
          "getcwd after chdir");
    CHECK(chdir("..") == 0, "chdir ..");
    CHECK(getcwd(cwd1, sizeof(cwd1)) > 0 && !strcmp(cwd1, "/"),
          "back to / via ..");
    // relative open against cwd
    CHECK(chdir("/tmp_x") == 0, "chdir again");
    fd = open("relfile", O_CREAT | O_RDWR, 0644);
    CHECK(fd >= 0, "relative O_CREAT");
    close(fd);
    CHECK(unlink("relfile") == 0, "relative unlink");
    CHECK(chdir("/") == 0, "chdir /");
    CHECK(rmdir("/tmp_x") == 0, "rmdir /tmp_x");
    CHECK(open("/tmp_x", O_RDONLY, 0) < 0, "dir gone");

    // --- pipes: write in child, read in parent (blocking both ways) ---
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe()");
    long pid = fork();
    if (pid == 0) {
        close(fds[0]);
        const char *m = "pipe ping";
        write(fds[1], m, strlen(m));
        close(fds[1]);
        exit(0);
    }
    close(fds[1]);
    n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    if (n > 0)
        buf[n] = 0;
    CHECK(n == 9 && !strcmp(buf, "pipe ping"), "pipe data through child");
    int st;
    wait(&st);

    // --- dup2 redirect: child writes to a file via stdout ---
    fd = open("/tmp_o", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "open /tmp_o");
    pid = fork();
    if (pid == 0) {
        close(1);
        CHECK(dup2(fd, 1) == 1, "dup2 to 1");
        printf("redirected hello\n");
        exit(0);
    }
    wait(&st);
    // rewind and verify
    lseek(fd, 0, 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    // the child's CHECK line lands in the file too; look for our marker
    // anywhere (no strstr in the mini libc)
    int found = 0;
    for (long k = 0; k + 16 <= n; k++)
        if (!strncmp(buf + k, "redirected hello", 16)) {
            found = 1;
            break;
        }
    CHECK(found, "stdout landed in file");
    CHECK(unlink("/tmp_o") == 0, "unlink /tmp_o");

    // --- fcntl flags round trip ---
    fd = open("/etc/motd", O_RDONLY, 0);
    CHECK(fd >= 0, "open motd");
    CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "F_SETFD");
    CHECK(fcntl(fd, F_GETFD, 0) == FD_CLOEXEC, "F_GETFD");
    long d2 = fcntl(fd, F_DUPFD, 3);
    CHECK(d2 >= 3, "F_DUPFD");
    close(fd);
    close(d2);

    printf("fsx: %s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
