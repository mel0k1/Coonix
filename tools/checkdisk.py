#!/usr/bin/env python3
"""Sanity-check an ext2 image built by mkdisk.py: superblock, walk the tree."""
import struct
import sys

B = 1024


def main():
    img = open(sys.argv[1], "rb").read()
    sb = img[1024:2048]
    magic = struct.unpack_from("<H", sb, 56)[0]
    rev = struct.unpack_from("<I", sb, 76)[0]
    isz = struct.unpack_from("<H", sb, 88)[0]
    blocks = struct.unpack_from("<I", sb, 4)[0]
    print(f"magic=0x{magic:04x} rev={rev} inode_size={isz} blocks={blocks}")
    assert magic == 0xEF53

    itbl = struct.unpack_from("<I", img, 2 * B + 8)[0]

    def inode(ino):
        off = itbl * B + (ino - 1) * isz
        raw = img[off:off + isz]
        mode = struct.unpack_from("<H", raw, 0)[0]
        size = struct.unpack_from("<I", raw, 4)[0]
        blocks_ = list(struct.unpack_from("<12I", raw, 40))
        indirect = struct.unpack_from("<I", raw, 88)[0]
        return mode, size, blocks_, indirect

    def read_file(size, blocks_, indirect):
        data = b""
        need = (size + B - 1) // B
        idxs = list(blocks_)
        if need > 12:
            t = img[indirect * B:indirect * B + B]
            idxs += list(struct.unpack(f"<{need - 12}I", t[:4 * (need - 12)]))
        for i in range(need):
            data += img[idxs[i] * B:idxs[i] * B + B]
        return data[:size]

    def walk(ino, path):
        mode, size, blks, ind = inode(ino)
        d = read_file(size, blks, ind)
        pos = 0
        while pos < len(d):
            f_ino, rl, nl, ft = struct.unpack_from("<IHBB", d, pos)
            if not f_ino:
                pos += rl if rl else B
                continue
            name = d[pos + 8:pos + 8 + nl].decode()
            if name not in (".", ".."):
                print(f"  {path}/{name}  ino={f_ino} type={'dir' if ft==2 else 'file'}")
                if ft == 2:
                    walk(f_ino, f"{path}/{name}")
            pos += rl
            if rl == 0:
                break

    print("tree:")
    walk(2, "")


if __name__ == "__main__":
    main()
