#!/usr/bin/env python3
"""Builds a small ext2 disk image from a staging directory.

Layout: 1024B blocks, rev 1, 128-byte inodes, single block group.
8 MB image = 8192 blocks, 128 inodes. Files up to 12 direct + 256
indirect blocks (~268 KB). The kernel side is a read-only ext2 driver.
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
DATA_START = INODE_TABLE_BLOCK + INODES * INODE_SIZE // BLOCK  # 19

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

    def write_super(self):
        sb = struct.pack(
            "<7I", INODES, TOTAL_BLOCKS, 0, 0, INODES - 11, FIRST_DATA_BLOCK, 0)
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
        # group descriptor 0: unused bitmaps, inode table at block 3
        bgd = struct.pack("<3I", 0, 0, INODE_TABLE_BLOCK)
        bgd += struct.pack("<7H", TOTAL_BLOCKS, INODES, 0, TOTAL_BLOCKS, INODES - 11, 0, 0)
        bgd += bytes(32 - 2 * 4 - 7 * 2)
        self.write_block(BGD_BLOCK, bgd.ljust(BLOCK, b"\0"))


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
    for dp, data in dir_data.items():
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
        if len(blocks) > 12:
            ind = img.alloc()
            packed = b"".join(struct.pack("<I", b) for b in blocks[12:12 + 256])
            img.write_block(ind, packed.ljust(BLOCK, b"\0"))
            struct.pack_into("<I", raw, 40 + 4 * 12, ind)
        img.write_inode(ino_of[rel], raw)

    img.write_super()
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
