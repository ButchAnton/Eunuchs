#!/usr/bin/env python3
"""Host test: fsck passes a fresh image and catches each kind of corruption.

usage: test_fsck.py MKFS_BINARY SOME_FILE
"""
import os
import struct
import subprocess
import sys
import tempfile

BS = 4096


def main():
    mkfs, payload = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp()
    img = os.path.join(tmp, "t.img")
    subprocess.run([mkfs, img, "4", "/bin/a=" + payload, "/bin/b=@/bin/a", "/etc/x=" + payload,
                    "/tmp/"], check=True, stdout=subprocess.DEVNULL)
    clean = open(img, "rb").read()
    sb = struct.unpack_from("<10I", clean, BS)
    inodestart, bmapstart, datastart = sb[7], sb[8], sb[9]

    def inode_off(ino):
        return inodestart * BS + ino * 128

    def check(data, want_errors, label):
        path = os.path.join(tmp, "c.img")
        open(path, "wb").write(data)
        p = subprocess.run([mkfs, "--check", path], capture_output=True, text=True)
        found = p.returncode != 0
        out = p.stdout + p.stderr
        ok = found == bool(want_errors) and all(w in out for w in want_errors)
        print("  %-36s %s" % (label, "ok" if ok else "FAILED"))
        if not ok:
            print(p.stdout)
        return ok

    def corrupt(fn):
        d = bytearray(clean)
        fn(d)
        return bytes(d)

    def clear_bit(d, b):
        d[bmapstart * BS + b // 8] &= ~(1 << (b % 8))

    def set_last_bit(d):
        b = sb[3] - 1
        d[bmapstart * BS + b // 8] |= 1 << (b % 8)

    def bump_nlink(d, ino, by):
        n = struct.unpack_from("<H", d, inode_off(ino) + 2)[0]
        struct.pack_into("<H", d, inode_off(ino) + 2, n + by)

    def dup_block(d):
        # inode 3 = /bin/a: point its second direct block at its first
        first = struct.unpack_from("<I", d, inode_off(3) + 40)[0]
        struct.pack_into("<I", d, inode_off(3) + 44, first)

    def orphan(d):
        # make a free inode look allocated
        struct.pack_into("<HH", d, inode_off(200), 2, 1)

    def bad_dotdot(d):
        # the first directory after root: rewrite its ".." to point at itself
        for ino in range(2, 10):
            if struct.unpack_from("<H", d, inode_off(ino))[0] == 1:
                blk = struct.unpack_from("<I", d, inode_off(ino) + 40)[0]
                struct.pack_into("<I", d, blk * BS + 64, ino)
                return

    results = [
        check(clean, [], "fresh image is clean"),
        check(corrupt(lambda d: clear_bit(d, datastart)), ["free in the bitmap"], "used block marked free"),
        check(corrupt(set_last_bit), ["leaked"], "leaked block"),
        check(corrupt(lambda d: bump_nlink(d, 3, 1)), ["nlink is"], "wrong link count"),
        check(corrupt(dup_block), ["also used by"], "block used twice"),
        check(corrupt(orphan), ["orphan"], "orphaned inode"),
        check(corrupt(bad_dotdot), ["'..' points to"], "bad '..' entry"),
        check(corrupt(lambda d: struct.pack_into("<I", d, BS, 0)), ["not an efs image"],
              "bad superblock magic"),
    ]
    print("fsck: %d checks, %d failures" % (len(results), results.count(False)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
