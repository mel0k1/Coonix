#!/usr/bin/env python3
"""extract a file from the coonix ext2 image and hash it.
usage: checklib.py <disk.img> <path-in-fs>"""
import hashlib
import struct
import sys

BLK = 1024


def main():
    img_path, want = sys.argv[1], sys.argv[2].strip("/")
    data = open(img_path, "rb").read()

    sb = 1024
    inodes_count = struct.unpack_from("<I", data, sb + 0)[0]
    log_bs = struct.unpack_from("<I", data, sb + 24)[0]
    assert log_bs == 0, "expect 1k blocks"
    inodes_per_group = struct.unpack_from("<I", data, sb + 40)[0]
    inode_size = struct.unpack_from("<H", data, sb + 88)[0]
    first_ino = struct.unpack_from("<I", data, sb + 84)[0]
    bgd_block = 2 + inodes_count * inode_size // BLK + 1  # not used; fixed layout
    # fixed layout from mkdisk: itable at 3 (256 inodes * 128 = 32 blocks)
    ITABLE = 3
    BG = 2 + 32 + 1  # bgd at 35? no: mkdisk: BGD_BLOCK=2, itable 3..34, bmap 35?
    # replicate mkdisk constants:
    INODES = 256
    ITABLE_BLOCKS = INODES * 128 // BLK          # 16
    BLOCK_BITMAP_BLOCK = ITABLE + ITABLE_BLOCKS  # 19
    # bgd is at block 2 (see mkdisk: BGD_BLOCK = 2)
    bgd_off = 2 * BLK
    inode_bitmap = struct.unpack_from("<I", data, bgd_off + 8)[0]

    def read_inode(ino):
        idx = ino - 1
        off = ITABLE * BLK + idx * 128
        mode = struct.unpack_from("<H", data, off)[0]
        size = struct.unpack_from("<I", data, off + 4)[0]
        blocks = struct.unpack_from("<15I", data, off + 40)
        return mode, size, blocks

    def block_bytes(b):
        return data[b * BLK:(b + 1) * BLK]

    def file_bytes(ino):
        mode, size, blk = read_inode(ino)
        out = b""
        for b in blk[:12]:
            if len(out) >= size or not b:
                break
            out += block_bytes(b)
        # single indirect
        if len(out) < size and blk[12]:
            t = block_bytes(blk[12])
            for j in range(256):
                if len(out) >= size:
                    break
                b2 = struct.unpack_from("<I", t, j * 4)[0]
                if b2:
                    out += block_bytes(b2)
        return out[:size]

    # walk dirs from root (ino 2)
    def lookup(dir_ino, name):
        mode, size, blk = read_inode(dir_ino)
        raw = b""
        for b in blk[:12]:
            if b:
                raw += block_bytes(b)
        pos = 0
        while pos + 8 <= len(raw):
            ino, reclen, nlen = struct.unpack_from("<IHB", raw, pos)
            if not reclen:
                break
            nm = raw[pos + 8:pos + 8 + nlen]
            if ino and nm.decode(errors="replace") == name:
                return ino
            pos += reclen
        return None

    cur = 2
    for comp in want.split("/"):
        cur = lookup(cur, comp)
        if cur is None:
            print(f"NOT FOUND: {comp}")
            return 1
    b = file_bytes(cur)
    print(f"{want}: size={len(b)} md5={hashlib.md5(b).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
