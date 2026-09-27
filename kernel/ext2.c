#include "ext2.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "kernel.h"

// superblock lives 1024 bytes in; with 1k blocks that is device block 2
#define E2_MAGIC   0xef53
#define E2_ROOT    2          // root inode number
#define E2_IFMT    0xf000
#define E2_IFDIR   0x4000
#define E2_IFREG   0x8000

#define BLK_SIZE   1024
#define BLK_IND    256        // u32 entries per indirect block

static struct vnode *e2_lookup(struct vnode *dir, const char *name);
static long e2_read(struct vnode *vn, void *buf, uint64_t off, uint64_t len);
static long e2_write(struct vnode *vn, const void *buf, uint64_t off, uint64_t len);
struct vfs_ops e2_ops = { e2_lookup, e2_read, e2_write };

struct e2_super {
    uint32_t inodes_count, blocks_count, r_blocks, free_blocks, free_inodes,
             first_data_block;
    uint32_t log_block_size;           // 24
    uint32_t log_frag_size;            // 28
    uint32_t blocks_per_group;         // 32
    uint32_t frags_per_group;          // 36
    uint32_t inodes_per_group;         // 40
    uint32_t mtime, wtime;             // 44, 48
    uint16_t mnt_count, max_mnt_count; // 52, 54
    uint16_t magic;                    // 56
    uint16_t state, errors, minor_rev; // 58, 60, 62
    uint32_t lastcheck, checkinterval, creator_os, rev_level; // 64..76
    uint16_t def_resuid, def_resgid;   // 80, 82
    uint32_t first_ino;                // 84
    uint16_t inode_size;               // 88
} __attribute__((packed));

struct e2_bgd {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t pad[14];
} __attribute__((packed));

struct e2_inode {
    uint16_t mode, uid;
    uint32_t size_lo;
    uint32_t atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks512, flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_hi;
    uint32_t faddr;
    uint32_t osd2[3];
} __attribute__((packed));

_Static_assert(sizeof(struct e2_inode) == 128, "ext2 inode layout");

struct e2_dirent {
    uint32_t ino;
    uint16_t rec_len;
    uint8_t name_len, ftype;
    // name follows, padded to 4
} __attribute__((packed));

// fs instance (we have at most one)
static struct {
    uint32_t blocks_count, inodes_per_group, blocks_per_group;
    uint32_t inode_size, bgd_block, first_data_block;
} fs;

// -- block cache -----------------------------------------------------------

#define CACHE_N 64
static struct {
    uint32_t block;
    uint32_t age;
    int valid;
    uint8_t data[BLK_SIZE];
} cache[CACHE_N];
static uint32_t cache_clock;

static const uint8_t *blk_get(uint32_t n) {
    int slot = -1;
    uint32_t oldest = 0xffffffff;
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].valid && cache[i].block == n) {
            cache[i].age = ++cache_clock;
            return cache[i].data;
        }
        if (!cache[i].valid) {
            slot = i;
            break;
        }
        if (cache[i].age < oldest) {
            oldest = cache[i].age;
            slot = i;
        }
    }
    if (ata_read_sectors((uint64_t)n * 2, 2, cache[slot].data) < 0)
        return 0;
    cache[slot].valid = 1;
    cache[slot].block = n;
    cache[slot].age = ++cache_clock;
    return cache[slot].data;
}

// -- inode plumbing --------------------------------------------------------

struct e2_node {
    uint32_t ino;
    struct e2_inode inode;
    struct vnode vn;       // fs_data points back here
};

#define NODE_CACHE 96
static struct e2_node nodes[NODE_CACHE];   // slot 0 reserved for root

static struct e2_node *node_get(uint32_t ino) {
    for (int i = 0; i < NODE_CACHE; i++)
        if (nodes[i].ino == ino)
            return &nodes[i];

    // load from disk
    struct e2_node *n = 0;
    for (int i = 0; i < NODE_CACHE; i++)
        if (!nodes[i].ino) {
            n = &nodes[i];
            break;
        }
    if (!n) {
        // cache full: uncached side alloc, vfs has no vnode refcounts yet,
        // so entries handed out earlier must stay untouched
        n = kzalloc(sizeof(*n));
        if (!n)
            return 0;
    }
    uint32_t group = (ino - 1) / fs.inodes_per_group;
    uint32_t index = (ino - 1) % fs.inodes_per_group;
    const struct e2_bgd *bg = (const struct e2_bgd *)blk_get(fs.bgd_block + group);
    if (!bg)
        return 0;
    uint64_t off = (uint64_t)index * fs.inode_size;
    uint32_t tbl = bg->inode_table;
    const uint8_t *b1 = blk_get(tbl + (uint32_t)(off / BLK_SIZE));
    if (!b1)
        return 0;
    if (off % BLK_SIZE + fs.inode_size <= BLK_SIZE) {
        memcpy(&n->inode, b1 + off % BLK_SIZE, sizeof(struct e2_inode));
    } else {
        // inode spans two blocks
        const uint8_t *b2 = blk_get(tbl + (uint32_t)(off / BLK_SIZE) + 1);
        if (!b2)
            return 0;
        uint64_t first = BLK_SIZE - off % BLK_SIZE;
        memcpy(&n->inode, b1 + off % BLK_SIZE, first);
        memcpy((uint8_t *)&n->inode + first, b2, sizeof(struct e2_inode) - first);
    }
    n->ino = ino;

    int type = n->inode.mode & E2_IFMT;
    n->vn.type = type == E2_IFDIR ? VNODE_DIR : VNODE_FILE;
    n->vn.size = n->inode.size_lo;
    n->vn.ops = &e2_ops;
    n->vn.fs_data = n;
    return n;
}

// data block index -> device block, 0 on hole/error
static uint32_t blk_of(struct e2_inode *in, uint64_t bi) {
    if (bi < 12)
        return in->block[bi];
    bi -= 12;
    if (bi < BLK_IND) {
        if (!in->block[12])
            return 0;
        const uint32_t *t = (const uint32_t *)blk_get(in->block[12]);
        return t ? t[bi] : 0;
    }
    bi -= BLK_IND;
    if (bi < (uint64_t)BLK_IND * BLK_IND) {
        if (!in->block[13])
            return 0;
        const uint32_t *l1 = (const uint32_t *)blk_get(in->block[13]);
        if (!l1)
            return 0;
        const uint32_t *l2 = (const uint32_t *)blk_get(l1[bi / BLK_IND]);
        return l2 ? l2[bi % BLK_IND] : 0;
    }
    return 0;   // triple indirect: not needed at this size
}

// -- vfs ops ---------------------------------------------------------------

static long e2_read(struct vnode *vn, void *buf, uint64_t off, uint64_t len) {
    struct e2_node *n = vn->fs_data;

    if (n->vn.type == VNODE_DIR) {
        // same contract as tmpfs: read() on a dir yields "name\n" listing
        uint64_t cap = vn->size + BLK_SIZE;
        char *list = kmalloc(cap);
        if (!list)
            return -1;
        uint64_t p = 0;
        for (uint32_t b = 0; b * BLK_SIZE < vn->size; b++) {
            const uint8_t *raw = blk_get(blk_of(&n->inode, b));
            if (!raw)
                break;
            uint64_t pos = 0;
            while (pos + sizeof(struct e2_dirent) <= BLK_SIZE) {
                const struct e2_dirent *d =
                    (const struct e2_dirent *)(raw + pos);
                if (d->rec_len < sizeof(struct e2_dirent) ||
                    pos + d->rec_len > BLK_SIZE)
                    break;
                if (d->ino) {
                    memcpy(list + p, raw + pos + sizeof(*d), d->name_len);
                    p += d->name_len;
                    list[p++] = '\n';
                }
                pos += d->rec_len;
            }
        }
        if (off >= p) {
            kfree(list);
            return 0;
        }
        uint64_t n2 = p - off;
        if (n2 > len)
            n2 = len;
        memcpy(buf, list + off, n2);
        kfree(list);
        return n2;
    }

    // regular file
    if (off >= vn->size)
        return 0;
    uint64_t want = vn->size - off;
    if (want > len)
        want = len;
    uint64_t done = 0;
    while (done < want) {
        uint64_t abs = off + done;
        uint32_t b = (uint32_t)(abs / BLK_SIZE);
        uint32_t inblk = (uint32_t)(abs % BLK_SIZE);
        uint32_t db = blk_of(&n->inode, b);
        if (!db)
            return done ? (long)done : -1;
        uint8_t *dst = (uint8_t *)buf + done;
        uint64_t chunk = BLK_SIZE - inblk;
        if (chunk > want - done)
            chunk = want - done;
        if (db) {
            const uint8_t *raw = blk_get(db);
            if (!raw)
                return done ? (long)done : -1;
            memcpy(dst, raw + inblk, chunk);
        } else {
            memset(dst, 0, chunk);   // sparse hole
        }
        done += chunk;
    }
    return (long)done;
}

static long e2_write(struct vnode *vn, const void *buf, uint64_t off, uint64_t len) {
    (void)vn; (void)buf; (void)off; (void)len;
    return -1;   // read-only for now
}

static struct vnode *e2_lookup(struct vnode *dir, const char *name) {
    struct e2_node *d = dir->fs_data;
    uint64_t dlen = strlen(name);

    for (uint32_t b = 0; b * BLK_SIZE < dir->size; b++) {
        uint32_t db = blk_of(&d->inode, b);
        if (!db)
            return 0;
        const uint8_t *raw = blk_get(db);
        if (!raw)
            return 0;
        uint64_t pos = 0;
        while (pos + sizeof(struct e2_dirent) <= BLK_SIZE) {
            const struct e2_dirent *e = (const struct e2_dirent *)(raw + pos);
            if (e->rec_len < sizeof(struct e2_dirent) ||
                pos + e->rec_len > BLK_SIZE)
                break;
            if (e->ino && e->name_len == dlen &&
                !memcmp(raw + pos + sizeof(*e), name, dlen))
                return &node_get(e->ino)->vn;
            pos += e->rec_len;
        }
    }
    return 0;
}

// -- mount -----------------------------------------------------------------

int ext2_mount_root(void) {
    memset(cache, 0, sizeof(cache));

    // superblock: device blocks 2..3 (1024 bytes into a block-sized buffer!)
    uint8_t raw[BLK_SIZE];
    struct e2_super sb;
    if (ata_read_sectors(2, 2, raw) < 0)
        return -1;
    memcpy(&sb, raw, sizeof(sb));
    if (sb.magic != E2_MAGIC)
        return -1;
    if (sb.log_block_size != 0 || sb.rev_level < 1 ||
        sb.inode_size != sizeof(struct e2_inode))
        return -1;

    fs.blocks_count = sb.blocks_count;
    fs.inodes_per_group = sb.inodes_per_group;
    fs.blocks_per_group = sb.blocks_per_group;
    fs.inode_size = sb.inode_size;
    fs.first_data_block = sb.first_data_block;
    fs.bgd_block = sb.first_data_block + 1;

    struct e2_node *root = node_get(E2_ROOT);
    if (!root || root->vn.type != VNODE_DIR)
        return -1;
    vfs_mount_root(&root->vn);
    console_puts("ext2: root fs on ata0 master, ");
    char num[12];
    uint32_t kib = fs.blocks_count;   // 1k blocks -> KiB
    int i = 11;
    num[i] = 0;
    do
        num[--i] = '0' + kib % 10;
    while ((kib /= 10));
    console_puts(&num[i]);
    console_puts(" KiB image\n");
    return 0;
}
