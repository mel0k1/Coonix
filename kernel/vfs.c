#include "vfs.h"
#include "pipe.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "net.h"

static struct vnode *resolve_parent(const char *path, char *name, int nsize);

static struct vnode *vfs_root;

// --- overlay mounts -----------------------------------------------------
// keyed by (parent vnode, name): when resolution walks into the named
// entry under the recorded parent, the cover vnode is returned instead of
// consulting the underlying fs. ext2/tmpfs hand out stable vnodes (node
// caches), so parent pointers are safe to keep
#define MOUNT_MAX 8
static struct mount_ent {
    struct vnode *parent;
    char name[32];
    struct vnode *cover;
} mounts[MOUNT_MAX];
static int mount_used;

// shadow check during path walks
static struct vnode *mount_check(struct vnode *dir, const char *name) {
    for (int i = 0; i < mount_used; i++)
        if (mounts[i].parent == dir && !strcmp(mounts[i].name, name))
            return mounts[i].cover;
    return 0;
}

int vfs_mount_at(const char *path, struct vnode *cover) {
    if (!cover || !path || path[0] != '/')
        return -1;
    char name[64];
    struct vnode *parent = resolve_parent(path, name, sizeof(name));
    if (!parent || !*name)
        return -1;
    if (mount_used >= MOUNT_MAX)
        return -1;
    for (int i = 0; i < mount_used; i++)
        if (mounts[i].parent == parent && !strcmp(mounts[i].name, name))
            return -1;   // already mounted there
    struct mount_ent *m = &mounts[mount_used++];
    m->parent = parent;
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = 0;
    m->cover = cover;
    return 0;
}

struct vnode *vfs_get_root(void) {
    return vfs_root;
}

void vfs_init(void) {
    vfs_root = 0;
    mount_used = 0;
}

void vfs_mount_root(struct vnode *vn) {
    vfs_root = vn;
}

// component-wise walk from `start`. symlink components are followed
// (max depth 8); nofollow=1 leaves the FINAL component undereferenced.
// returns 0 if any component is missing
static struct vnode *resolve_from(struct vnode *start, const char *path,
                                  int nofollow, int depth) {
    if (!start || !path)
        return 0;
    if (depth > 8)
        return 0;   // symlink loop

    struct vnode *dir = start;
    char comp[64];
    int i = 0;

    while (path[i]) {
        while (path[i] == '/')
            i++;
        if (!path[i])
            break;

        int len = 0;
        while (path[i] && path[i] != '/') {
            if (len < (int)sizeof(comp) - 1)
                comp[len++] = path[i];
            else
                return 0;   // too long: fail the lookup, never truncate
            i++;
        }
        comp[len] = 0;

        // trailing slashes after the last component: fine
        int more = 0;
        for (int j = i; path[j]; j++)
            if (path[j] != '/') { more = 1; break; }

        if (!strcmp(comp, "."))
            continue;
        if (!strcmp(comp, "..")) {
            // no parent pointers: ".." works only if the fs driver knows
            if (dir->ops->lookup)
                dir = dir->ops->lookup(dir, "..");
            if (!dir)
                return 0;
            continue;
        }
        if (dir->type != VNODE_DIR)
            return 0;
        struct vnode *vn = mount_check(dir, comp);
        if (!vn)
            vn = dir->ops->lookup(dir, comp);
        if (!vn)
            return 0;

        if (vn->type == VNODE_LNK && (more || !nofollow)) {
            // follow: resolve the target, then keep walking from there
            char target[128];
            long n = vn->ops->readlink
                ? vn->ops->readlink(vn, target, sizeof(target) - 1)
                : -1;
            if (n < 0)
                return 0;
            target[n] = 0;
            struct vnode *tl = target[0] == '/'
                ? resolve_from(vfs_root, target, 0, depth + 1)
                : resolve_from(dir, target, 0, depth + 1);
            if (!tl)
                return 0;
            vn = tl;
        }
        dir = vn;
        if (!more)
            return dir;
    }
    return dir;
}

struct vnode *vfs_resolve(const char *path) {
    if (!vfs_root || !path || path[0] != '/')
        return 0;
    return resolve_from(vfs_root, path, 0, 0);
}

struct vnode *vfs_resolve_nofollow(const char *path) {
    if (!vfs_root || !path || path[0] != '/')
        return 0;
    return resolve_from(vfs_root, path, 1, 0);
}

struct vnode *vfs_resolve_prog(const char *name) {
    if (!name || !*name)
        return 0;
    // path with slashes: use as-is
    int has_slash = 0;
    for (const char *p = name; *p; p++)
        if (*p == '/') { has_slash = 1; break; }
    if (has_slash)
        return vfs_resolve(name);

    // bare name: /bin/<name>
    char buf[128];
    strcpy(buf, "/bin/");
    strncpy(buf + 5, name, sizeof(buf) - 6);
    buf[sizeof(buf) - 1] = 0;
    return vfs_resolve(buf);
}

struct file *vfs_open(struct vnode *vn) {
    if (!vn)
        return 0;
    struct file *f = kmalloc(sizeof(*f));
    if (!f)
        return 0;
    memset(f, 0, sizeof(*f));   // path[] must start empty
    f->vn = vn;
    f->refs = 1;
    vn->refs++;   // keep the vnode alive while any fd points at it
    return f;
}

long vfs_read(struct file *f, void *buf, uint64_t len) {
    if (!f || !f->vn)
        return -1;
    long n = f->vn->ops->read(f->vn, buf, f->off, len);
    if (n > 0)
        f->off += n;
    return n;
}

long vfs_write(struct file *f, const void *buf, uint64_t len) {
    if (!f || !f->vn)
        return -1;
    long n = f->vn->ops->write(f->vn, buf, f->off, len);
    if (n > 0)
        f->off += n;
    return n;
}

void vfs_close(struct file *f) {
    if (!f)
        return;
    if (--f->refs > 0)
        return;
    if (f->is_console)
        return; // static console slots are never freed
    if (f->is_socket) {
        net_close(f->sock);   // stream teardown runs from the tick
        kfree(f);
        return;
    }
    if (f->pipe)
        pipe_release_end(f->pipe, f->pipe_writer);
    if (f->vn) {
        if (f->vn->refs > 0)
            f->vn->refs--;
        if (f->vn->ops->release)
            f->vn->ops->release(f->vn);   // frees unlinked nodes
    }
    kfree(f);
}

long vfs_read_file(const char *path, void **outbuf) {
    struct vnode *vn = vfs_resolve(path);
    if (!vn || vn->type != VNODE_FILE)
        return -1;
    if (vn->size == 0) {
        *outbuf = 0;
        return 0;
    }
    void *buf = kmalloc(vn->size);
    if (!buf)
        return -1;
    long n = vn->ops->read(vn, buf, 0, vn->size);
    if (n < 0) {
        kfree(buf);
        return -1;
    }
    *outbuf = buf;
    return n;
}

// walk to the parent dir of an absolute path ("/a/b/c" -> vnode of /a/b)
// and leave the last component in name[]
static struct vnode *resolve_parent(const char *path, char *name, int nsize) {
    if (!path || path[0] != '/')
        return 0;

    struct vnode *vn = vfs_root;
    char comp[64];
    int i = 0;

    while (path[i]) {
        while (path[i] == '/')
            i++;
        if (!path[i])
            break;

        int len = 0;
        while (path[i] && path[i] != '/') {
            if (len < (int)sizeof(comp) - 1)
                comp[len++] = path[i];
            else
                return 0;   // too long: fail the lookup, never truncate
            i++;
        }
        comp[len] = 0;

        // last component?
        int last = 1;
        for (int j = i; path[j]; j++)
            if (path[j] != '/') {
                last = 0;
                break;
            }
        if (last) {
            if (len >= nsize)
                return 0;   // caller buffer can't hold the name
            strcpy(name, comp);
            return vn;
        }
        struct vnode *c = mount_check(vn, comp);
        if (!c)
            c = vn->ops->lookup(vn, comp);
        if (!c)
            return 0;
        if (c->type == VNODE_LNK) {
            char target[128];
            long n = c->ops->readlink
                ? c->ops->readlink(c, target, sizeof(target) - 1) : -1;
            if (n < 0)
                return 0;
            target[n] = 0;
            struct vnode *tl = target[0] == '/'
                ? resolve_from(vfs_root, target, 0, 1)
                : resolve_from(vn, target, 0, 1);
            if (!tl)
                return 0;
            c = tl;
        }
        if (c->type != VNODE_DIR)
            return 0;
        vn = c;
    }
    return 0;
}

struct vnode *vfs_create(const char *path) {
    char name[64];
    // check-then-create must not be preemptable apart: a tick between
    // lookup and create lets a second task insert the same name (fs-level
    // ops only guard themselves)
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl));
    struct vnode *dir = resolve_parent(path, name, sizeof(name));
    struct vnode *out = 0;
    if (dir && dir->type == VNODE_DIR && dir->ops->create &&
        !dir->ops->lookup(dir, name))
        out = dir->ops->create(dir, name);   // refuse to clobber
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory");
    return out;
}

int vfs_truncate(struct vnode *vn) {
    if (!vn || vn->type != VNODE_FILE || !vn->ops->truncate)
        return -1;
    return vn->ops->truncate(vn);
}

// mount entries ride behind fs entries via a ctx high bit: fs cursors
// (ext2 block<<32|off, tmpfs index) never set bit 63
#define MOUNT_CTX_BIT (1ULL << 63)

int vfs_readdir(struct vnode *dir, uint64_t *ctx, uint64_t *ino, int *type,
                char *name, int name_cap) {
    if (!dir || dir->type != VNODE_DIR)
        return 0;
    if (!(*ctx & MOUNT_CTX_BIT) && dir->ops->readdir) {
        if (dir->ops->readdir(dir, ctx, ino, type, name, name_cap))
            return 1;
        *ctx = MOUNT_CTX_BIT;   // fs exhausted: switch to mount entries
    }
    if (*ctx & MOUNT_CTX_BIT) {
        uint64_t i = *ctx & ~MOUNT_CTX_BIT;
        while (i < (uint64_t)mount_used) {
            struct mount_ent *m = &mounts[i];
            i++;
            if (m->parent != dir)
                continue;
            strncpy(name, m->name, name_cap - 1);
            name[name_cap - 1] = 0;
            *ino = m->cover->ino;
            *type = VNODE_DIR;
            *ctx = MOUNT_CTX_BIT | i;
            return 1;
        }
        return 0;
    }
    return 0;
}

// generic helpers: resolve the parent, apply the last component

struct vnode *vfs_unlink(const char *path) {
    char name[64];
    struct vnode *dir = resolve_parent(path, name, sizeof(name));
    if (!dir || !dir->ops->unlink || !dir->ops->lookup(dir, name))
        return 0;
    return dir->ops->unlink(dir, name) == 0 ? dir : 0;
}

struct vnode *vfs_mkdir(const char *path) {
    char name[64];
    struct vnode *dir = resolve_parent(path, name, sizeof(name));
    if (!dir || dir->type != VNODE_DIR || !dir->ops->mkdir)
        return 0;
    if (dir->ops->lookup(dir, name))
        return 0;
    return dir->ops->mkdir(dir, name);
}

struct vnode *vfs_rmdir(const char *path) {
    char name[64];
    struct vnode *dir = resolve_parent(path, name, sizeof(name));
    if (!dir || !dir->ops->rmdir)
        return 0;
    if (dir->ops->rmdir(dir, name) == 0)
        return dir;
    return 0;
}

struct vnode *vfs_symlink(const char *path, const char *target) {
    char name[64];
    struct vnode *dir = resolve_parent(path, name, sizeof(name));
    if (!dir || dir->type != VNODE_DIR || !dir->ops->symlink)
        return 0;
    if (dir->ops->lookup(dir, name))
        return 0;
    return dir->ops->symlink(dir, name, target);
}

long vfs_readlink_vn(struct vnode *vn, char *buf, uint64_t size) {
    if (!vn || vn->type != VNODE_LNK || !vn->ops->readlink)
        return -1;
    return vn->ops->readlink(vn, buf, size);
}

int vfs_rename(const char *oldp, const char *newp) {
    char oname[64], nname[64];
    struct vnode *odir = resolve_parent(oldp, oname, sizeof(oname));
    struct vnode *ndir = resolve_parent(newp, nname, sizeof(nname));
    if (!odir || !ndir)
        return -1;
    struct vnode *victim = odir->ops->lookup(odir, oname);
    if (!victim)
        return -1;
    if (victim->type == VNODE_DIR)
        return -1;   // directory rename: not supported yet
    // posix: rename REPLACES an existing target (except dirs)
    struct vnode *existing = ndir->ops->lookup(ndir, nname);
    if (existing && existing != victim) {
        if (existing->type == VNODE_DIR || !ndir->ops->unlink)
            return -1;
        if (ndir->ops->unlink(ndir, nname) < 0)
            return -1;
    }
    if (existing == victim)
        return 0;   // same file, nothing to do
    // files/symlinks: hard-link at the new location, drop the old entry
    if (victim->type == VNODE_LNK) {
        char target[128];
        long n = victim->ops->readlink
            ? victim->ops->readlink(victim, target, sizeof(target) - 1) : -1;
        if (n < 0 || !ndir->ops->symlink)
            return -1;
        target[n] = 0;
        return ndir->ops->symlink(ndir, nname, target) &&
               odir->ops->unlink(odir, oname) == 0 ? 0 : -1;
    }
    if (!ndir->ops->link)
        return -1;
    if (ndir->ops->link(ndir, victim, nname) < 0)
        return -1;
    return odir->ops->unlink(odir, oname);
}

int vfs_truncate_to(struct vnode *vn, uint64_t size) {
    if (!vn || vn->type != VNODE_FILE || !vn->ops->truncate_to)
        return -1;
    return vn->ops->truncate_to(vn, size);
}

int vfs_chmod(struct vnode *vn, uint32_t mode) {
    if (!vn || !vn->ops->chmod)
        return -1;
    return vn->ops->chmod(vn, mode);
}

// hard link: existing vnode gets a second directory entry
int vfs_link(const char *oldp, const char *newp) {
    char name[64];
    struct vnode *vn = vfs_resolve_nofollow(oldp);
    if (!vn || vn->type == VNODE_DIR)
        return -1;
    struct vnode *ndir = resolve_parent(newp, name, sizeof(name));
    if (!ndir || ndir->type != VNODE_DIR || !ndir->ops->link)
        return -1;
    if (ndir->ops->lookup(ndir, name))
        return -1;
    return ndir->ops->link(ndir, vn, name);
}
