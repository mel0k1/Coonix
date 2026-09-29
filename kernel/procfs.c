// procfs: process information pseudo-filesystem. the root lists one
// directory per live process (tgid leaders); each carries stat / status /
// cmdline / cwd, generated fresh from task_table on every read, linux
// format where it matters (stat/status) so procps-style tools can parse it
#include "procfs.h"
#include "task.h"
#include "heap.h"
#include "string.h"

// --- forward declarations (ops table needs the cli-guarded wrappers) ---
static struct vnode *pfs_lookup_impl(struct vnode *dir, const char *name);
static long pfs_read_impl(struct vnode *vn, void *buf, uint64_t off,
                          uint64_t len);
static int pfs_readdir_impl(struct vnode *dir, uint64_t *ctx, uint64_t *ino,
                            int *type, char *name, int name_cap);
static long pfs_readlink_impl(struct vnode *vn, char *buf, uint64_t size);

static struct vnode *pfs_lookup(struct vnode *dir, const char *name);
static long pfs_read(struct vnode *vn, void *buf, uint64_t off, uint64_t len);
static int pfs_readdir(struct vnode *dir, uint64_t *ctx, uint64_t *ino,
                       int *type, char *name, int name_cap);
static long pfs_readlink(struct vnode *vn, char *buf, uint64_t size);

// vfs_ops order: lookup read write create truncate readdir unlink mkdir
//                rmdir readlink symlink truncate_to chmod link
struct vfs_ops pfs_ops = {
    pfs_lookup, pfs_read, 0, 0, 0,
    pfs_readdir, 0, 0, 0,
    pfs_readlink, 0, 0, 0, 0
};

// --- node pool ---------------------------------------------------------
// vnodes returned by lookup must stay valid while a struct file references
// them (this vfs does not refcount vnodes); a fixed pool keyed by ino
// mirrors the ext2 node cache. slots are recycled on lookup by ino match

enum { P_STAT, P_STATUS, P_CMDLINE, P_CWD };
#define N_PFILES 4
static const char *pfile_names[N_PFILES] = { "stat", "status", "cmdline",
                                             "cwd" };

enum { PN_ROOT, PN_PIDDIR, PN_FILE, PN_LNK };
struct pnode {
    struct vnode vn;
    int used;
    int kind;        // PN_*
    int pid;         // 0 for the root
    int which;       // P_* for files/links
};
#define POOL_N 128
static struct pnode pool[POOL_N];

static struct vnode *procfs_parent;   // ".." from the root: mount dir

// ino layout (stable across lookups, drives pool recycling):
//   root    = 1
//   pid dir = 0x1000 + pid
//   file    = 0x2000 + pid * 8 + which
static uint64_t make_ino(int kind, int pid, int which) {
    if (kind == PN_ROOT)
        return 1;
    if (kind == PN_PIDDIR)
        return 0x1000 + (uint64_t)pid;
    return 0x2000 + (uint64_t)pid * 8 + (uint64_t)which;
}

static struct vnode *pnode_get(int kind, int pid, int which) {
    uint64_t ino = make_ino(kind, pid, which);

    for (int i = 0; i < POOL_N; i++)
        if (pool[i].used && pool[i].vn.ino == ino)
            return &pool[i].vn;

    for (int i = 0; i < POOL_N; i++) {
        if (!pool[i].used) {
            struct pnode *pn = &pool[i];
            memset(pn, 0, sizeof(*pn));
            pn->used = 1;
            pn->kind = kind;
            pn->pid = pid;
            pn->which = which;
            pn->vn.ops = &pfs_ops;
            pn->vn.fs_data = pn;
            pn->vn.ino = ino;
            pn->vn.dev = 0x5052;       // "PR": pseudo-fs device id
            switch (kind) {
            case PN_ROOT:
            case PN_PIDDIR:
                pn->vn.type = VNODE_DIR;
                pn->vn.mode = 0x4000 | 0555;
                pn->vn.size = 4096;
                break;
            case PN_LNK:
                pn->vn.type = VNODE_LNK;
                pn->vn.mode = 0xA000 | 0777;
                pn->vn.size = 0;       // filled per lookup
                break;
            default:
                pn->vn.type = VNODE_FILE;
                pn->vn.mode = 0x8000 | 0444;
                pn->vn.size = 0;       // generated content, sized on read
            }
            return &pn->vn;
        }
    }
    return 0;
}

// task_table scan: the process (tgid leader) behind a pid, 0 if gone
static struct task *find_leader(int pid) {
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &task_table[i];
        if (t->state != T_FREE && t->pid == pid && t->tgid == pid)
            return t;
    }
    return 0;
}

// --- formatting helpers (kernel has no printf) -------------------------

struct sbuf {
    char *p;
    int left;
};

static void sb_raw(struct sbuf *b, const char *s, int n) {
    while (n-- > 0 && b->left > 1) {
        *b->p++ = *s++;
        b->left--;
    }
    *b->p = 0;
}

static void sb_str(struct sbuf *b, const char *s) {
    while (*s && b->left > 1) {
        *b->p++ = *s++;
        b->left--;
    }
    *b->p = 0;
}

static void sb_u64(struct sbuf *b, uint64_t v) {
    char tmp[21];
    int i = 0;
    if (!v)
        tmp[i++] = '0';
    while (v) {
        tmp[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i-- > 0)
        sb_raw(b, &tmp[i], 1);
}

static void sb_i64(struct sbuf *b, int64_t v) {
    if (v < 0) {
        sb_raw(b, "-", 1);
        v = -v;
    }
    sb_u64(b, (uint64_t)v);
}

// single-letter process state for stat
static char state_char(int state) {
    switch (state) {
    case T_RUNNING: return 'R';
    case T_READY:   return 'R';
    case T_BLOCKED: return 'S';
    case T_ZOMBIE:  return 'Z';
    default:        return '?';
    }
}

// long state name for status
static const char *state_long(int state) {
    switch (state) {
    case T_RUNNING: return "R (running)";
    case T_READY:   return "R (ready)";
    case T_BLOCKED: return "S (sleeping)";
    case T_ZOMBIE:  return "Z (zombie)";
    default:        return "? (unknown)";
    }
}

// rough virtual size: program break span + mmap regions
static uint64_t task_vsize(struct task *t) {
    uint64_t v = t->brk_cur > t->brk_base ? t->brk_cur - t->brk_base : 0;
    for (struct mmap_region *m = t->mmaps; m; m = m->next)
        v += m->end - m->start;
    return v;
}

// threads in the process (tgid group)
static int task_threads(struct task *t) {
    return task_count_group(t->tgid, 0);
}

// --- content generators ------------------------------------------------

// linux /proc/<pid>/stat: pid (comm) state ppid pgrp session tty_nr tpgid
// flags minflt cminflt majflt cmajflt utime stime cutime cstime priority
// nice num_threads itrealvalue starttime vsize rss ...
static void gen_stat(struct task *t, struct sbuf *b) {
    char sc[2] = { state_char(t->state), 0 };

    sb_u64(b, (uint64_t)t->pid);
    sb_raw(b, " (", 2);
    sb_str(b, t->comm);
    sb_raw(b, ") ", 2);
    sb_str(b, sc);
    sb_raw(b, " ", 1);
    sb_i64(b, t->parent ? t->parent->pid : 0);   // ppid
    sb_raw(b, " ", 1);
    sb_i64(b, t->pgid);                          // pgrp
    sb_raw(b, " ", 1);
    sb_i64(b, t->pgid);                          // session
    sb_raw(b, " 0 ", 3);                         // tty_nr
    sb_i64(b, t->pgid);                          // tpgid
    sb_raw(b, " 0", 2);                          // flags
    for (int i = 0; i < 8; i++)                  // minflt..cstime: 0
        sb_raw(b, " 0", 2);
    sb_raw(b, " 20 0", 5);                       // priority, nice
    sb_raw(b, " ", 1);
    sb_u64(b, (uint64_t)task_threads(t));        // num_threads
    sb_raw(b, " 0 0 ", 5);                       // itrealvalue, starttime
    sb_u64(b, task_vsize(t));                    // vsize
    sb_raw(b, " 0\n", 3);                        // rss
}

// linux /proc/<pid>/status, the human-readable view
static void gen_status(struct task *t, struct sbuf *b) {
    sb_str(b, "Name:\t");
    sb_str(b, t->comm);
    sb_raw(b, "\nState:\t", 8);
    sb_str(b, state_long(t->state));
    sb_str(b, "\nTgid:\t");
    sb_i64(b, t->tgid);
    sb_str(b, "\nPid:\t");
    sb_i64(b, t->pid);
    sb_str(b, "\nPPid:\t");
    sb_i64(b, t->parent ? t->parent->pid : 0);
    sb_str(b, "\nUid:\t0\t0\t0\t0");
    sb_str(b, "\nGid:\t0\t0\t0\t0");
    sb_str(b, "\nThreads:\t");
    sb_i64(b, (int64_t)task_threads(t));
    sb_raw(b, "\n", 1);
}

// --- ops ----------------------------------------------------------------

static struct vnode *pfs_lookup_impl(struct vnode *dir, const char *name) {
    struct pnode *d = dir->fs_data;

    if (d->kind == PN_ROOT) {
        if (!strcmp(name, ".."))
            return procfs_parent;
        if (!*name)
            return 0;
        // decimal only
        int pid = 0, digits = 0;
        for (const char *p = name; *p; p++, digits++) {
            if (*p < '0' || *p > '9' || digits > 6)
                return 0;
            pid = pid * 10 + (*p - '0');
        }
        if (pid <= 0)
            return 0;
        if (!find_leader(pid))
            return 0;
        return pnode_get(PN_PIDDIR, pid, 0);
    }

    if (d->kind == PN_PIDDIR) {
        for (int w = 0; w < N_PFILES; w++) {
            if (!strcmp(name, pfile_names[w])) {
                struct vnode *vn = pnode_get(w == P_CWD ? PN_LNK : PN_FILE,
                                             d->pid, w);
                if (vn && w == P_CWD) {
                    struct task *t = find_leader(d->pid);
                    if (t)
                        vn->size = strlen(t->cwd);
                }
                return vn;
            }
        }
    }
    return 0;
}

// generate the file content fresh, then copy out [off, off+len)
static long pfs_read_impl(struct vnode *vn, void *buf, uint64_t off,
                          uint64_t len) {
    struct pnode *pn = vn->fs_data;
    if (pn->kind == PN_ROOT || pn->kind == PN_PIDDIR)
        return -1;                   // dirs are not readable as files
    if (!len)
        return 0;

    struct task *t = find_leader(pn->pid);
    if (!t)
        return 0;                    // process vanished: clean EOF

    // cmdline: a plain kernel block, no generation needed
    if (pn->which == P_CMDLINE) {
        uint64_t total = (uint64_t)t->cmdline_len;
        if (!t->cmdline || off >= total)
            return 0;
        uint64_t n = total - off < len ? total - off : len;
        memcpy(buf, t->cmdline + off, n);
        return (long)n;
    }

    // generated: 1k is plenty for stat/status
    char *gen = kmalloc(1024);
    if (!gen)
        return -1;
    struct sbuf b = { .p = gen, .left = 1024 };
    gen[0] = 0;
    if (pn->which == P_STAT)
        gen_stat(t, &b);
    else
        gen_status(t, &b);
    uint64_t total = (uint64_t)(b.p - gen);

    if (off >= total) {
        kfree(gen);
        return 0;
    }
    uint64_t n = total - off < len ? total - off : len;
    memcpy(buf, gen + off, n);
    kfree(gen);
    return (long)n;
}

// one entry per call; *ctx = next task_table slot (root) or file index
// (pid dir)
static int pfs_readdir_impl(struct vnode *dir, uint64_t *ctx, uint64_t *ino,
                            int *type, char *name, int name_cap) {
    struct pnode *d = dir->fs_data;

    if (d->kind == PN_ROOT) {
        for (uint64_t i = *ctx; i < (uint64_t)TASK_MAX; i = *ctx) {
            struct task *t = &task_table[i];
            *ctx = i + 1;
            if (t->state == T_FREE || t->pid != t->tgid || t->pid == 0)
                continue;
            char tmp[12];
            int j = 0;
            int v = t->pid;
            do {
                tmp[j++] = '0' + v % 10;
                v /= 10;
            } while (v);
            if (j + 1 > name_cap)
                return 0;
            for (int k = 0; k < j; k++)
                name[k] = tmp[j - 1 - k];
            name[j] = 0;
            *ino = make_ino(PN_PIDDIR, t->pid, 0);
            *type = VNODE_DIR;
            return 1;
        }
        return 0;
    }

    if (d->kind == PN_PIDDIR) {
        uint64_t i = *ctx;
        *ctx = i + 1;
        if (i < (uint64_t)N_PFILES) {
            strncpy(name, pfile_names[i], name_cap - 1);
            name[name_cap - 1] = 0;
            *ino = make_ino(i == P_CWD ? PN_LNK : PN_FILE, d->pid, (int)i);
            *type = i == P_CWD ? VNODE_LNK : VNODE_FILE;
            return 1;
        }
    }
    return 0;
}

static long pfs_readlink_impl(struct vnode *vn, char *buf, uint64_t size) {
    struct pnode *pn = vn->fs_data;
    if (pn->kind != PN_LNK || pn->which != P_CWD)
        return -1;
    struct task *t = find_leader(pn->pid);
    if (!t)
        return -1;
    uint64_t n = strlen(t->cwd);
    if (n > size - 1)
        n = size - 1;
    memcpy(buf, t->cwd, n);
    buf[n] = 0;
    return (long)n;
}

// positional cli-guarded wrappers (same pattern as ext2): the impls touch
// task_table and kmalloc, a timer tick mid-op would let processes die
#define PFS_ENTER uint64_t __pfl; __asm__ volatile("pushfq; popq %0; cli" : "=r"(__pfl))
#define PFS_LEAVE __asm__ volatile("pushq %0; popfq" :: "r"(__pfl) : "memory")

static struct vnode *pfs_lookup(struct vnode *dir, const char *name) {
    PFS_ENTER;
    struct vnode *r = pfs_lookup_impl(dir, name);
    PFS_LEAVE;
    return r;
}

static long pfs_read(struct vnode *vn, void *buf, uint64_t off, uint64_t len) {
    PFS_ENTER;
    long r = pfs_read_impl(vn, buf, off, len);
    PFS_LEAVE;
    return r;
}

static int pfs_readdir(struct vnode *dir, uint64_t *ctx, uint64_t *ino,
                       int *type, char *name, int name_cap) {
    PFS_ENTER;
    int r = pfs_readdir_impl(dir, ctx, ino, type, name, name_cap);
    PFS_LEAVE;
    return r;
}

static long pfs_readlink(struct vnode *vn, char *buf, uint64_t size) {
    PFS_ENTER;
    long r = pfs_readlink_impl(vn, buf, size);
    PFS_LEAVE;
    return r;
}

struct vnode *procfs_mount(struct vnode *mp_parent) {
    procfs_parent = mp_parent;
    return pnode_get(PN_ROOT, 0, 0);
}
