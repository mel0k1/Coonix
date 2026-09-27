#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "console.h"

static struct vnode *vfs_root;

void vfs_init(void) {
    vfs_root = 0;
}

void vfs_mount_root(struct vnode *vn) {
    vfs_root = vn;
}

struct vnode *vfs_resolve(const char *path) {
    if (!vfs_root || !path || path[0] != '/')
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
            i++;
        }
        comp[len] = 0;

        if (!strcmp(comp, "."))
            continue;
        if (vn->type != VNODE_DIR)
            return 0;
        vn = vn->ops->lookup(vn, comp);
        if (!vn)
            return 0;
    }
    return vn;
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
    f->vn = vn;
    f->off = 0;
    f->refs = 1;
    f->is_console = 0;
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
            strncpy(name, comp, nsize - 1);
            name[nsize - 1] = 0;
            return vn;
        }
        if (vn->type != VNODE_DIR)
            return 0;
        vn = vn->ops->lookup(vn, comp);
        if (!vn)
            return 0;
    }
    return 0;
}

struct vnode *vfs_create(const char *path) {
    char name[64];
    struct vnode *dir = resolve_parent(path, name, sizeof(name));
    if (!dir || dir->type != VNODE_DIR || !dir->ops->create)
        return 0;
    // refuse to clobber
    if (dir->ops->lookup(dir, name))
        return 0;
    return dir->ops->create(dir, name);
}

int vfs_truncate(struct vnode *vn) {
    if (!vn || vn->type != VNODE_FILE || !vn->ops->truncate)
        return -1;
    return vn->ops->truncate(vn);
}
