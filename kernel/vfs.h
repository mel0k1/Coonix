// virtual filesystem: vnodes, mounts, path resolution, open files
#pragma once
#include <stdint.h>

enum { VNODE_FILE, VNODE_DIR };

struct vnode;

// fs driver callbacks, all take/return vnodes
struct vfs_ops {
    // child by name (".." included); 0 if absent
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    // read/write byte ranges, return bytes moved or -1
    long (*read)(struct vnode *vn, void *buf, uint64_t off, uint64_t len);
    long (*write)(struct vnode *vn, const void *buf, uint64_t off, uint64_t len);
    // create an empty regular file named name in dir; 0 on error
    struct vnode *(*create)(struct vnode *dir, const char *name);
    // cut a regular file down to zero bytes; 0 ok, -1 error
    int (*truncate)(struct vnode *vn);
};

struct vnode {
    int type;              // VNODE_FILE / VNODE_DIR
    uint64_t size;         // files: bytes; dirs: 0
    struct vfs_ops *ops;
    void *fs_data;         // fs-private inode
    uint64_t ino;          // fs inode number (st_ino)
    uint64_t dev;          // per-fs device id (st_dev)
};

// open file description, shared across fork
struct file {
    struct vnode *vn;      // 0 = console
    uint64_t off;
    int refs;
    int is_console;
};

#define FILE_MAX 16

void vfs_init(void);
void vfs_mount_root(struct vnode *vn);

// absolute paths only (no cwd yet)
struct vnode *vfs_resolve(const char *path);
// resolve executable: full path as-is, bare name -> /bin/<name>
struct vnode *vfs_resolve_prog(const char *name);

// allocates a struct file (refs = 1), 0 on error
struct file *vfs_open(struct vnode *vn);
long vfs_read(struct file *f, void *buf, uint64_t len);
long vfs_write(struct file *f, const void *buf, uint64_t len);
void vfs_close(struct file *f);

// whole file into a kmalloc buffer, returns size or -1
long vfs_read_file(const char *path, void **outbuf);

// resolve parent dir of an absolute path, then create the last component;
// returns the new vnode or 0
struct vnode *vfs_create(const char *path);
// truncate a file vnode to zero length
int vfs_truncate(struct vnode *vn);
