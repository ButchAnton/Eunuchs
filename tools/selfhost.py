#!/usr/bin/env python3
"""make selfhost: Eunuchs builds its own user space.

Boots Eunuchs with this source tree shared on /mnt/host and runs
user/build.mk there twice:

  stage 1  with the image's /bin/tcc (which gcc built), into build/selfhost/stage1
  stage 2  with stage 1's tcc, into build/selfhost/stage2

Each stage builds the C library and tcc's runtime into a sysroot, then every
program, tcc included. The two stages must be byte-for-byte identical: tcc
built by tcc builds the same user space as tcc built by gcc (a fixed point).
The Makefile then makes build/selfhost/fs.img from stage 2 and runs the test
suite on it.

Usage: selfhost.py --qemu "QEMU COMMAND" --image build/fs.img
"""
import argparse
import filecmp
import os
import re
import shlex
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_tests import Console, ROOT  # noqa: E402

OUT = os.path.join(ROOT, "build", "selfhost")


def differences(a, b):
    """Files that differ or exist on one side only, below directories a and b."""
    cmp = filecmp.dircmp(a, b)
    found = [os.path.join(a, f) for f in cmp.left_only] + [os.path.join(b, f) for f in cmp.right_only]
    for f in cmp.common_files:
        if not filecmp.cmp(os.path.join(a, f), os.path.join(b, f), shallow=False):
            found.append(os.path.relpath(os.path.join(a, f), OUT))
    for d in cmp.common_dirs:
        found += differences(os.path.join(a, d), os.path.join(b, d))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True, help="QEMU command without the disk")
    ap.add_argument("--image", required=True)
    ap.add_argument("--timeout", type=float, default=900, help="seconds for both stages")
    a = ap.parse_args()

    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    img = os.path.join(OUT, "boot.img")
    shutil.copyfile(a.image, img)
    qemu = shlex.split(a.qemu) + [
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", "file=%s,if=none,format=raw,id=hd0" % img, "-device", "virtio-blk-device,drive=hd0",
        "-fsdev", "local,id=hostfs,path=%s,security_model=none" % ROOT,
        "-device", "virtio-9p-device,fsdev=hostfs,mount_tag=host",
        "-append", "panic=shutdown"]
    build = ("make -f user/build.mk OUT=build/selfhost/stage1 > build/selfhost/stage1.log 2>&1 && "
             "make -f user/build.mk OUT=build/selfhost/stage2 TCC=build/selfhost/stage1/bin/tcc "
             "> build/selfhost/stage2.log 2>&1")
    t0 = time.time()
    status = None
    with open(os.path.join(OUT, "console.log"), "w") as log:
        con = Console(qemu, log)
        try:
            con.expect("root@eunuchs:/# ", 30)
            # the marker is assembled by the shell, so the echoed command line can't match it
            con.line("cd /mnt/host && %s; s=$?; m=SELFHOST; echo ${m}-STATUS=$s." % build)
            con.expect("SELFHOST-STATUS=", a.timeout)
            con.expect(".", 5)
            m = re.search(r"SELFHOST-STATUS=(\d+)\.", con.log_text())
            status = int(m.group(1)) if m else None
        except Exception as e:
            print("selfhost: %s" % e)
        con.close()
    print("selfhost: guest build %s in %.0fs" % ("finished" if status == 0 else "FAILED", time.time() - t0))
    if status != 0:
        for stage in ("stage1", "stage2"):
            p = os.path.join(OUT, stage + ".log")
            if os.path.exists(p):
                print("--- last lines of build/selfhost/%s.log ---" % stage)
                print("\n".join(open(p).read().splitlines()[-15:]))
        return 1

    progs = sorted(os.listdir(os.path.join(OUT, "stage2", "bin")))
    diff = differences(os.path.join(OUT, "stage1"), os.path.join(OUT, "stage2"))
    if diff:
        print("selfhost: stage 1 and stage 2 differ:\n  " + "\n  ".join(diff[:20]))
        return 1
    print("selfhost: %d programs and the C library; stage 1 and stage 2 are identical" % len(progs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
