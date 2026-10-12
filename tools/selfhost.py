#!/usr/bin/env python3
"""make selfhost: Eunuchs builds itself.

Boots Eunuchs and runs tools/selfhost.sh in the image's own source tree,
/usr/src/eunuchs, into /tmp/selfhost on its own disk, as it would with no Mac
at all: user/build.mk and kernel/build.mk, twice:

  stage 1  with the image's /bin/tcc (which gcc built)
  stage 2  with stage 1's tcc

Then it copies stage1/, stage2/ and the logs over the share (mounted on
/mnt/host) into build/selfhost, installs stage 2's kernel as /boot/kernel.elf
and reboots: the boot loader must bring up that kernel (its banner says tcc).

Each stage builds the C library and tcc's runtime into a sysroot, then every
program (tcc and kld, the kernel's linker, included), then the kernel. The
two stages must be byte-for-byte identical: tcc built by tcc builds the same
system as tcc built by gcc (a fixed point). The Makefile then boots stage 2's
kernel with an image made from stage 2 and runs the test suite.

Usage: selfhost.py --qemu "QEMU COMMAND" --image build/fs.img
"""
import argparse
import filecmp
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_tests import Console, ROOT  # noqa: E402

OUT = os.path.join(ROOT, "build", "selfhost")


def source_date_epoch():
    """The time of the last commit (or now, outside git)."""
    try:
        return int(subprocess.run(["git", "-C", ROOT, "log", "-1", "--format=%ct"], capture_output=True,
                                  text=True, check=True).stdout.strip())
    except (OSError, subprocess.CalledProcessError, ValueError):
        return int(time.time())


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
    t0 = time.time()
    status = None
    rebooted = False
    with open(os.path.join(OUT, "console.log"), "w") as log:
        con = Console(qemu, log)
        try:
            con.expect("root@eunuchs:/# ", 30)
            # the marker is assembled by the shell, so the echoed command line can't match it
            # (the stages are in tools/selfhost.sh: a long command line would
            # not fit the terminal's line)
            # __DATE__ and __TIME__ from the last commit, so both stages and
            # every rebuild of one commit stamp the same time
            con.line("cd /usr/src/eunuchs && SOURCE_DATE_EPOCH=%d SH_OUT=/tmp/selfhost sh tools/selfhost.sh; "
                     "s=$?; m=SELFHOST; echo ${m}-STATUS=$s." % source_date_epoch())
            con.expect("SELFHOST-STATUS=", a.timeout)
            con.expect(".", 5)
            m = re.search(r"SELFHOST-STATUS=(\d+)\.", con.log_text())
            status = int(m.group(1)) if m else None
            # the results (or the logs of a failure) to the Mac
            con.line("cp -r /tmp/selfhost/stage1 /tmp/selfhost/stage2 /tmp/selfhost/*.log "
                     "/mnt/host/build/selfhost; s=$?; m=COPIED; echo ${m}-$s.")
            con.expect("COPIED-", 300)
            con.expect(".", 5)
            if not re.search(r"COPIED-0\.", con.log_text()) and status == 0:
                print("selfhost: copying the results to the Mac failed")
                status = None
            if status == 0:
                # and run it: install the kernel it built, reboot through the
                # boot loader, and see that one come up
                con.line("cp /tmp/selfhost/stage2/kernel/kernel.elf /boot/kernel.elf && reboot")
                con.expect("eboot: loaded /boot/kernel.elf", 60)
                con.expect("by tcc", 30)
                con.expect("root@eunuchs:/# ", 60)
                rebooted = True
        except Exception as e:
            print("selfhost: %s" % e)
        con.close()
    print("selfhost: guest build %s in %.0fs" % ("finished" if status == 0 else "FAILED", time.time() - t0))
    if status == 0 and not rebooted:
        print("selfhost: Eunuchs did not reboot into the kernel it built (see build/selfhost/console.log)")
        return 1
    if rebooted:
        print("selfhost: Eunuchs installed the kernel it built and rebooted into it")
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
    print("selfhost: the kernel, %d programs and the C library; stage 1 and stage 2 are identical"
          % len(progs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
