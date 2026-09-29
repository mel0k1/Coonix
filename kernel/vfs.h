// virtual filesystem: vnodes, mounts, path resolution, open files
#pragma once
#include <stdint.h>

enum { VNODE_FILE, VNODE_DIR, VNODE_LNK, VNODE_PIPE };

struct vnode;

struct pipe;

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
    // --- optional (zero = unsupported) ---
    // directory iterator: one entry per call. *ctx is a driver-private
    // cursor (0 = restart); returns 1 with entry filled, 0 = no more
    int (*readdir)(struct vnode *dir, uint64_t *ctx, uint64_t *ino,
                   int *type, char *name, int name_cap);
    // remove name from dir (file or symlink); 0 ok
    int (*unlink)(struct vnode *dir, const char *name);
    // create a directory; 0 ok
    struct vnode *(*mkdir)(struct vnode *dir, const char *name);
    // remove an empty directory; 0 ok
    int (*rmdir)(struct vnode *dir, const char *name);
    // symlink target into buf; bytes copied or -1
    long (*readlink)(struct vnode *vn, char *buf, uint64_t size);
    // create a symlink named name -> target; 0 ok
    struct vnode *(*symlink)(struct vnode *dir, const char *name,
                             const char *target);
    // resize a file (free blocks past the new end); 0 ok
    int (*truncate_to)(struct vnode *vn, uint64_t size);
    // change permission bits (type bits preserved); 0 ok
    int (*chmod)(struct vnode *vn, uint32_t mode);
    // add a second directory entry pointing at an existing vnode (hard
    // link); bumps the link count; 0 ok
    int (*link)(struct vnode *dir, struct vnode *vn, const char *name);
};

struct vnode {
    int type;              // VNODE_*
    uint64_t size;         // files/symlinks: bytes
    uint32_t mode;         // st_mode low bits incl. S_IFxxx
    struct vfs_ops *ops;
    void *fs_data;         // fs-private inode
    uint64_t ino;          // fs inode number (st_ino)
    uint64_t dev;          // per-fs device id (st_dev)
};

// open file description, shared across fork
struct file {
    struct vnode *vn;      // 0 = console or pipe
    struct pipe *pipe;     // 0 unless a pipe endpoint
    int pipe_writer;       // pipe endpoints: 1 = write end
    uint64_t off;
    int refs;
    int is_console;
    char path[56];         // absolute path (procfs fd readlink targets)
};

#define FILE_MAX 16

// fd flags (fcntl F_SETFD / exec)
#define FD_CLOEXEC 1

void vfs_init(void);
void vfs_mount_root(struct vnode *vn);
// overlay mount: `cover` (a fs root vnode) becomes visible at `path`,
// shadowing whatever directory entry is there; the covered dir must exist
// as a path but its entries are hidden while mounted. no refcounts: the
// cover vnode is expected to be permanent (static pool like procfs)
int vfs_mount_at(const char *path, struct vnode *cover);
// the root vnode (for fs drivers that need ".." at the mount origin)
struct vnode *vfs_get_root(void);

// absolute paths; symlink components are followed (last one too)
struct vnode *vfs_resolve(const char *path);
// like vfs_resolve but the final component is NOT dereferenced if a link
struct vnode *vfs_resolve_nofollow(const char *path);
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

// directory iteration glue for getdents64: one entry per call, *ctx is
// an opaque cursor owned by the caller (0 = start)
int vfs_readdir(struct vnode *dir, uint64_t *ctx, uint64_t *ino, int *type,
                char *name, int name_cap);

// namei building blocks used by the syscall layer; paths are absolute,
// the caller prepends cwd for relative names
struct vnode *vfs_unlink(const char *path);
struct vnode *vfs_mkdir(const char *path);
struct vnode *vfs_rmdir(const char *path);
struct vnode *vfs_symlink(const char *path, const char *target);
long vfs_readlink_vn(struct vnode *vn, char *buf, uint64_t size);
int vfs_rename(const char *oldp, const char *newp);
int vfs_truncate_to(struct vnode *vn, uint64_t size);
int vfs_chmod(struct vnode *vn, uint32_t mode);
int vfs_link(const char *oldp, const char *newp);
