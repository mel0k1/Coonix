#!/usr/bin/env python3
"""Builds a small ext2 disk image from a staging directory.

Layout: 1024B blocks, rev 1, 128-byte inodes, single block group.
8 MB image = 8192 blocks, 128 inodes. Files up to 12 direct + 256
indirect blocks (~268 KB). The kernel side is a read/write ext2 driver,
so the block/inode bitmaps and free counts must be real.
"""
import os
import struct
import sys

BLOCK = 1024
TOTAL_BLOCKS = 8192
INODES = 128
INODE_SIZE = 128
FIRST_DATA_BLOCK = 1
BGD_BLOCK = 2
INODE_TABLE_BLOCK = 3
INODE_TABLE_BLOCKS = INODES * INODE_SIZE // BLOCK      # 16
BLOCK_BITMAP_BLOCK = INODE_TABLE_BLOCK + INODE_TABLE_BLOCKS   # 19
INODE_BITMAP_BLOCK = BLOCK_BITMAP_BLOCK + 1                    # 20
DATA_START = INODE_BITMAP_BLOCK + 1                            # 21

S_IFDIR = 0x4000
S_IFREG = 0x8000


class Image:
    def __init__(self):
        self.buf = bytearray(TOTAL_BLOCKS * BLOCK)
        self.next_block = DATA_START

    def alloc(self, n=1):
        b = self.next_block
        assert b + n <= TOTAL_BLOCKS, "image full"
        self.next_block += n
        return b

    def write_block(self, n, data):
        assert len(data) <= BLOCK
        self.buf[n * BLOCK:n * BLOCK + len(data)] = data

    def write_inode(self, ino, raw):
        assert len(raw) == INODE_SIZE
        off = INODE_TABLE_BLOCK * BLOCK + (ino - 1) * INODE_SIZE
        self.buf[off:off + INODE_SIZE] = raw

    def write_super(self, used_blocks, inode_list, used_dirs):
        used_inodes = len(inode_list)
        free_blocks = TOTAL_BLOCKS - used_blocks
        free_inodes = INODES - used_inodes
        sb = struct.pack(
            "<7I", INODES, TOTAL_BLOCKS, 0, free_blocks, free_inodes,
            FIRST_DATA_BLOCK, 0)
        sb += struct.pack("<2I", 0, TOTAL_BLOCKS)          # frags, blocks/group
        sb += struct.pack("<2I", 0, INODES)                # frags/group, inodes/group
        sb += struct.pack("<2I", 0, 0)                     # mtime, wtime
        sb += struct.pack("<2H", 0, 0xFFFF)                # mnt_count, max_mnt_count
        sb += struct.pack("<H", 0xEF53)                    # magic
        sb += struct.pack("<3H", 1, 0, 3)                  # state, errors, minor_rev
        sb += struct.pack("<4I", 0, 0, 0, 1)               # lastcheck, interval, os, rev=1
        sb += struct.pack("<2H", 0, 0)                     # resuid, resgid
        sb += struct.pack("<I", 11)                        # first_ino
        sb += struct.pack("<H", INODE_SIZE)                # inode_size
        self.write_block(1, sb.ljust(BLOCK, b"\0"))
        # group descriptor 0: real bitmaps + free counts
        bgd = struct.pack("<3I", BLOCK_BITMAP_BLOCK, INODE_BITMAP_BLOCK,
                          INODE_TABLE_BLOCK)
        bgd += struct.pack("<3H", free_blocks, free_inodes, used_dirs)
        bgd += bytes(32 - 3 * 4 - 3 * 2)
        self.write_block(BGD_BLOCK, bgd.ljust(BLOCK, b"\0"))

        # block bitmap: bit i covers block i + FIRST_DATA_BLOCK; block 0
        # (boot block) is outside the bitmap domain
        bbm = bytearray(BLOCK)
        for b in range(FIRST_DATA_BLOCK, used_blocks + 1):
            i = b - FIRST_DATA_BLOCK
            assert 0 <= i < BLOCK * 8
            bbm[i >> 3] |= 1 << (i & 7)
        self.write_block(BLOCK_BITMAP_BLOCK, bytes(bbm))

        # inode bitmap: bit i covers inode i + 1
        ibm = bytearray(BLOCK)
        for ino in inode_list:
            i = ino - 1
            assert 0 <= i < BLOCK * 8
            ibm[i >> 3] |= 1 << (i & 7)
        self.write_block(INODE_BITMAP_BLOCK, bytes(ibm))


def dirent_block(entries):
    """entries: list of (ino, ftype, name) -> one block of dirents"""
    out = bytearray()
    for ino, ftype, name in entries:
        nb = name.encode()
        reclen = (8 + len(nb) + 3) & ~3
        out += struct.pack("<IHBB", ino, reclen, len(nb), ftype) + nb
        out += bytes(reclen - 8 - len(nb))
    assert len(out) <= BLOCK, "directory overflow"
    # stretch the last real entry across the tail
    pos = 0
    while pos < len(out):
        rl = struct.unpack_from("<H", out, pos + 4)[0]
        if pos + rl >= len(out):
            struct.pack_into("<H", out, pos + 4, BLOCK - pos)
            break
        pos += rl
    return bytes(out).ljust(BLOCK, b"\0")


def build(root):
    img = Image()

    # walk staging tree
    entries = []  # (relpath, fullpath, is_dir)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for d in sorted(dirnames):
            rel = os.path.relpath(os.path.join(dirpath, d), root)
            entries.append((rel, os.path.join(dirpath, d), True))
        for f in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, f), root)
            entries.append((rel, os.path.join(dirpath, f), False))

    ino_of = {".": 2}
    nxt = 11
    for rel, full, is_dir in entries:
        ino_of[rel] = nxt
        nxt += 1
        assert nxt <= INODES, "too many files"

    # directory data blocks
    dir_data = {}
    for dp in ["."] + [rel for rel, _, is_d in entries if is_d]:
        ents = [(2, 2, "."), (2, 2, "..")]
        for rel, full, is_dir in entries:
            if (os.path.dirname(rel) or ".") == dp:
                ents.append((ino_of[rel], 2 if is_dir else 1, os.path.basename(rel)))
        dir_data[dp] = dirent_block(ents)

    # directory inodes
    dir_count = 0
    for dp, data in dir_data.items():
        dir_count += 1
        nblocks = 1
        b = img.alloc()
        img.write_block(b, data)
        raw = bytearray(INODE_SIZE)
        struct.pack_into("<H", raw, 0, S_IFDIR | 0o755)
        struct.pack_into("<I", raw, 4, len(data))
        struct.pack_into("<H", raw, 26, 2)        # links: '.' + parent's entry
        struct.pack_into("<I", raw, 28, 2)        # blocks in 512B units
        struct.pack_into("<I", raw, 40, b)
        img.write_inode(ino_of[dp], raw)

    # file inodes + data
    for rel, full, is_dir in entries:
        if is_dir:
            continue
        with open(full, "rb") as fh:
            data = fh.read()
        nblocks = (len(data) + BLOCK - 1) // BLOCK
        blocks = [img.alloc() for _ in range(nblocks)]
        for i, b in enumerate(blocks):
            img.write_block(b, data[i * BLOCK:(i + 1) * BLOCK])
        raw = bytearray(INODE_SIZE)
        struct.pack_into("<H", raw, 0, S_IFREG | 0o755)
        struct.pack_into("<I", raw, 4, len(data))
        struct.pack_into("<H", raw, 26, 1)
        struct.pack_into("<I", raw, 28, nblocks * 2)
        for i, b in enumerate(blocks[:12]):
            struct.pack_into("<I", raw, 40 + 4 * i, b)
        rest = blocks[12:]

        def pack_ptrs(ptrs):
            ptrs = list(ptrs) + [0] * (256 - len(ptrs))
            return b"".join(struct.pack("<I", p) for p in ptrs)

        # singly indirect: 256 blocks
        if rest:
            ind1 = img.alloc()
            img.write_block(ind1, pack_ptrs(rest[:256]))
            struct.pack_into("<I", raw, 40 + 4 * 12, ind1)
            rest = rest[256:]
        # doubly indirect: 256 * 256 blocks (~256 MiB max)
        if rest:
            ind2 = img.alloc()
            subs = []
            while rest:
                sub = img.alloc()
                img.write_block(sub, pack_ptrs(rest[:256]))
                subs.append(sub)
                rest = rest[256:]
            img.write_block(ind2, pack_ptrs(subs))
            struct.pack_into("<I", raw, 40 + 4 * 13, ind2)
        assert not rest, "triple indirect needed"
        img.write_inode(ino_of[rel], raw)

    img.write_super(used_blocks=img.next_block - 1,
                    inode_list=[2] + list(range(11, nxt)),
                    used_dirs=dir_count)
    return bytes(img.buf)


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <staging-dir> <out.img>")
        return 1
    data = build(sys.argv[1])
    with open(sys.argv[2], "wb") as fh:
        fh.write(data)
    print(f"disk: {sys.argv[2]}, {len(data) // 1024} KiB, ext2 rev1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
