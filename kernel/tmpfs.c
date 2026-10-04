#include "tmpfs.h"
#include "heap.h"
#include "kernel.h"
#include "string.h"
#include "console.h"

#define NAME_MAX 56
#define TMPFS_MAX_FILE (64ULL << 20)   // ram fs: keep off+len sane

struct tnode {
    char name[NAME_MAX];
    int type;
    int unlinked;          // detached from the tree, alive for open fds
    struct tnode *parent, *child, *sibling;
    uint8_t *data;
    uint64_t size, cap;
    struct vnode vn;       // embedded, fs_data points back
};

static struct tnode *troot;
static int nnodes;
static uint64_t tino;   // monotonic inode id for st_ino

static struct vfs_ops tmpfs_ops;

static struct tnode *tnode_new(const char *name, int type, struct tnode *parent);

static struct tnode *vn2t(struct vnode *vn) {
    return (struct tnode *)vn->fs_data;
}

static long tmpfs_read(struct vnode *vn, void *buf, uint64_t off, uint64_t len) {
    struct tnode *t = vn2t(vn);
    if (t->type == VNODE_DIR) {
        // read() on a dir yields the listing, one name per line
        uint64_t total = 0;
        for (struct tnode *c = t->child; c; c = c->sibling)
            total += strlen(c->name) + 1;
        if (off >= total)
            return 0;
        char *list = kmalloc(total);
        if (!list)
            return -1;
        uint64_t p = 0;
        for (struct tnode *c = t->child; c; c = c->sibling) {
            strcpy(list + p, c->name);
            p += strlen(c->name);
            list[p++] = '\n';
        }
        uint64_t n = total - off;
        if (n > len) n = len;
        memcpy(buf, list + off, n);
        kfree(list);
        return n;
    }
    if (off >= t->size)
        return 0;
    uint64_t n = t->size - off;
    if (n > len) n = len;
    memcpy(buf, t->data + off, n);
    return n;
}

static int tnode_grow(struct tnode *t, uint64_t need) {
    if (need <= t->cap)
        return 0;
    uint64_t cap = t->cap ? t->cap : 256;
    while (cap < need)
        cap *= 2;
    uint8_t *nd = kmalloc(cap);
    if (!nd)
        return -1;
    if (t->data) {
        memcpy(nd, t->data, t->size);
        kfree(t->data);
    }
    t->data = nd;
    t->cap = cap;
    return 0;
}

static long tmpfs_write(struct vnode *vn, const void *buf, uint64_t off, uint64_t len) {
    struct tnode *t = vn2t(vn);
    if (t->type != VNODE_FILE)
        return -1;
    // reject wrap-around offsets (lseek to -8 then write) before the
    // grow: off+len must not overflow
    if (off > TMPFS_MAX_FILE || len > TMPFS_MAX_FILE - off)
        return -1;
    if (tnode_grow(t, off + len) < 0)
        return -1;
    memcpy(t->data + off, buf, len);
    if (off + len > t->size)
        t->size = off + len;
    vn->size = t->size;
    return len;
}

static struct vnode *tmpfs_lookup(struct vnode *dir, const char *name) {
    struct tnode *t = vn2t(dir);
    if (!strcmp(name, ".."))
        return t->parent ? &t->parent->vn : dir;
    for (struct tnode *c = t->child; c; c = c->sibling)
        if (!strcmp(c->name, name))
            return &c->vn;
    return 0;
}

static struct vnode *tmpfs_create(struct vnode *dir, const char *name) {
    struct tnode *t = vn2t(dir);
    if (t->type != VNODE_DIR)
        return 0;
    if (strlen(name) >= NAME_MAX)
        return 0;
    for (struct tnode *x = t->child; x; x = x->sibling)
        if (!strcmp(x->name, name))
            return 0;   // exists
    struct tnode *n = tnode_new(name, VNODE_FILE, t);
    return n ? &n->vn : 0;
}

static int tmpfs_readdir(struct vnode *dir, uint64_t *ctx, uint64_t *ino,
                         int *type, char *name, int name_cap) {
    struct tnode *t = vn2t(dir);
    uint64_t skip = *ctx;
    for (struct tnode *c = t->child; c; c = c->sibling) {
        if (skip) { skip--; continue; }
        strncpy(name, c->name, name_cap - 1);
        name[name_cap - 1] = 0;
        *ino = c->vn.ino;
        *type = c->type;
        (*ctx)++;
        return 1;
    }
    return 0;
}

// last open fd dropped the vnode: free it if unlink already detached it
static void tmpfs_release(struct vnode *vn) {
    struct tnode *t = vn2t(vn);
    if (t->unlinked && !vn->refs) {
        if (t->data)
            kfree(t->data);
        kfree(t);
        nnodes--;
    }
}

static int tmpfs_unlink(struct vnode *dir, const char *name) {
    struct tnode *t = vn2t(dir);
    struct tnode **pp = &t->child;
    while (*pp) {
        if (!strcmp((*pp)->name, name)) {
            if ((*pp)->type == VNODE_DIR)
                return -1;
            struct tnode *dead = *pp;
            *pp = dead->sibling;
            // detach, keep alive while open fds still point at it
            dead->sibling = 0;
            dead->parent = 0;
            dead->unlinked = 1;
            tmpfs_release(&dead->vn);
            return 0;
        }
        pp = &(*pp)->sibling;
    }
    return -1;
}

static struct vnode *tmpfs_mkdir(struct vnode *dir, const char *name) {
    struct tnode *t = vn2t(dir);
    if (t->type != VNODE_DIR || strlen(name) >= NAME_MAX)
        return 0;
    for (struct tnode *x = t->child; x; x = x->sibling)
        if (!strcmp(x->name, name))
            return 0;
    struct tnode *n = tnode_new(name, VNODE_DIR, t);
    return n ? &n->vn : 0;
}

static int tmpfs_rmdir(struct vnode *dir, const char *name) {
    struct tnode *t = vn2t(dir);
    struct tnode **pp = &t->child;
    while (*pp) {
        if (!strcmp((*pp)->name, name)) {
            struct tnode *dead = *pp;
            if (dead->type != VNODE_DIR || dead->child)
                return -1;
            *pp = dead->sibling;
            kfree(dead);
            nnodes--;
            return 0;
        }
        pp = &(*pp)->sibling;
    }
    return -1;
}

static int tmpfs_truncate(struct vnode *vn) {
    struct tnode *t = vn2t(vn);
    if (t->type != VNODE_FILE)
        return -1;
    if (t->data)
        kfree(t->data);
    t->data = 0;
    t->size = 0;
    t->cap = 0;
    vn->size = 0;
    return 0;
}

static struct tnode *tnode_new(const char *name, int type, struct tnode *parent) {
    struct tnode *t = kzalloc(sizeof(*t));
    if (!t)
        return 0;
    strncpy(t->name, name, NAME_MAX - 1);
    t->type = type;
    t->parent = parent;
    t->vn.type = type;
    t->vn.ops = &tmpfs_ops;
    t->vn.fs_data = t;
    t->vn.ino = ++tino;
    t->vn.dev = 0x0009;   // distinct from ext2, stable per-fs id
    t->vn.mode = type == VNODE_DIR ? 0x4000 | 0755 : 0x8000 | 0644;
    if (parent) {
        t->sibling = parent->child;
        parent->child = t;
    }
    nnodes++;
    return t;
}

void tmpfs_mount(void) {
    tmpfs_ops.lookup = tmpfs_lookup;
    tmpfs_ops.read = tmpfs_read;
    tmpfs_ops.write = tmpfs_write;
    tmpfs_ops.create = tmpfs_create;
    tmpfs_ops.truncate = tmpfs_truncate;
    tmpfs_ops.readdir = tmpfs_readdir;
    tmpfs_ops.unlink = tmpfs_unlink;
    tmpfs_ops.release = tmpfs_release;
    tmpfs_ops.mkdir = tmpfs_mkdir;
    tmpfs_ops.rmdir = tmpfs_rmdir;
    troot = tnode_new("", VNODE_DIR, 0);
    if (!troot)
        panic("tmpfs: no root");
    vfs_mount_root(&troot->vn);
}

// resolve parent dir of path, creating missing components when create is set
static struct tnode *walk_parent(const char *path, int create) {
    if (path[0] != '/')
        return 0;
    struct tnode *t = troot;
    char comp[NAME_MAX];
    int i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        int len = 0;
        while (path[i] && path[i] != '/') {
            if (len < (int)sizeof(comp) - 1)
                comp[len++] = path[i];
            i++;
        }
        comp[len] = 0;
        // is this the last component?
        int last = 1;
        for (int j = i; path[j]; j++)
            if (path[j] != '/') { last = 0; break; }
        if (last)
            return t;
        struct tnode *c = 0;
        for (struct tnode *x = t->child; x; x = x->sibling)
            if (!strcmp(x->name, comp)) { c = x; break; }
        if (!c) {
            if (!create)
                return 0;
            c = tnode_new(comp, VNODE_DIR, t);
            if (!c)
                return 0;
        }
        if (c->type != VNODE_DIR)
            return 0;
        t = c;
    }
    return t;
}

int tmpfs_mkdir_p(const char *path) {
    // walk_parent stops before last comp; make the last one here
    const char *p = path + strlen(path);
    while (p > path && *p != '/') p--;
    if (p == path)
        return -1;
    struct tnode *parent = walk_parent(path, 1);
    if (!parent || parent->type != VNODE_DIR)
        return -1;
    const char *name = p + 1;
    if (!*name)
        return -1;
    for (struct tnode *x = parent->child; x; x = x->sibling)
        if (!strcmp(x->name, name))
            return 0; // exists
    return tnode_new(name, VNODE_DIR, parent) ? 0 : -1;
}

int tmpfs_put_file(const char *path, const void *data, uint64_t size) {
    const char *p = path + strlen(path);
    while (p > path && *p != '/') p--;
    if (p == path)
        return -1;
    struct tnode *parent = walk_parent(path, 1);
    if (!parent || parent->type != VNODE_DIR)
        return -1;
    const char *name = p + 1;
    if (!*name)
        return -1;
    struct tnode *t = 0;
    for (struct tnode *x = parent->child; x; x = x->sibling)
        if (!strcmp(x->name, name)) { t = x; break; }
    if (!t)
        t = tnode_new(name, VNODE_FILE, parent);
    if (!t || t->type != VNODE_FILE)
        return -1;
    if (tnode_grow(t, size) < 0)
        return -1;
    memcpy(t->data, data, size);
    t->size = size;
    t->vn.size = size;
    return 0;
}

int tmpfs_node_count(void) {
    return nnodes;
}
