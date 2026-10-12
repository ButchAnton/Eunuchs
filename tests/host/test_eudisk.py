#!/usr/bin/env python3
"""Host test: eudisk makes GPT images and updates them without losing work.

usage: test_eudisk.py EUDISK KERNEL

KERNEL is a Eunuchs kernel (build/kernel.boot) for the boot slots.
"""
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    eudisk, kernel = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp()
    img = os.path.join(tmp, "d.img")
    results = []

    def run(*args, ok=True):
        p = subprocess.run([eudisk] + list(args), capture_output=True, text=True)
        if ok and p.returncode != 0:
            print("    eudisk %s: status %d: %s" % (" ".join(args), p.returncode, p.stderr.strip()))
        return p

    def host(name, text):
        path = os.path.join(tmp, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def check(label, cond):
        print("  %-44s %s" % (label, "ok" if cond else "FAILED"))
        results.append(bool(cond))

    def cat(where):
        return run("cat", img + ":" + where, ok=False).stdout

    v1, v2 = host("v1", "version 1\n"), host("v2", "version 2\n")
    conf1, conf2 = host("c1", "config 1\n"), host("c2", "config 2\n")
    mine = host("mine", "my work\n")

    run("create", img, "boot:8", "boot.old:8", "root:16", "home:8")
    info = run("info", img).stdout
    check("create: four partitions", all(n in info for n in ("boot ", "boot.old", "root", "home")))
    check("fsck of a new image", run("fsck", img).returncode == 0)

    specs = ["/bin/", "/bin/prog=" + v1, "/bin/alias=@/bin/prog", "+/etc/conf=" + conf1, "/tmp/:1777"]
    run("install", img, "root", *specs)
    check("install: files and a hard link", cat("/bin/alias") == "version 1\n")

    # the user's changes, made in the guest
    run("put", host("edited", "edited prog\n"), img + ":/bin/prog")
    run("put", host("myconf", "my config\n"), img + ":/etc/conf")
    run("put", mine, img + ":/tmp/mine")
    run("put", mine, img + ":home:/notes")

    specs = ["/bin/", "/bin/prog=" + v2, "+/etc/conf=" + conf2, "/tmp/:1777"]   # alias dropped
    out = run("install", img, "root", *specs).stdout
    check("an edited file is replaced", cat("/bin/prog") == "version 2\n")
    backups = run("ls", "-R", img + ":/var/backup").stdout
    check("... after a backup of the edit", "prog" in backups)
    check("an edited config file stays", cat("/etc/conf") == "my config\n")
    check("... the new one is beside it", cat("/etc/conf.new") == "config 2\n")
    check("files install never had stay", cat("/tmp/mine") == "my work\n")
    check("other partitions are untouched", cat("home:/notes") == "my work\n")
    check("a link no longer installed is removed", run("ls", img + ":/bin/alias", ok=False).returncode != 0)
    out = run("install", img, "root", *specs).stdout
    check("installing again changes nothing", "0 new, 0 updated" in out and "backed up" not in out)

    # kernels
    check("a program is not a kernel", run("kernel", "install", img, v1, ok=False).returncode != 0)
    kern1 = open(kernel, "rb").read()
    k2 = os.path.join(tmp, "k2")
    with open(k2, "wb") as f:
        f.write(kern1 + b"x")                    # a different kernel

    def slot(name):
        out = os.path.join(tmp, "slot")
        if os.path.exists(out):
            os.remove(out)
        run("get", img + ":" + name + ":/kernel.elf", out, ok=False)
        return open(out, "rb").read() if os.path.exists(out) else None

    run("kernel", "install", "--to", "boot", img, kernel)
    run("kernel", "install", img, k2)
    check("kernel install rotates boot to boot.old", slot("boot") == kern1 + b"x" and slot("boot.old") == kern1)
    out = run("kernel", "install", img, k2).stdout
    check("the same kernel again is a no-op", "already" in out and slot("boot.old") == kern1)
    run("kernel", "rollback", img)
    check("rollback swaps them", slot("boot") == kern1 and slot("boot.old") == kern1 + b"x")

    # copy between images keeps owners and links
    img2 = os.path.join(tmp, "e.img")
    run("create", img2, "root:8", "home:8")
    run("mkdir", "-o", "1000:1000", img + ":home:/guest")
    run("copy", img + ":/", img2 + ":/")
    run("copy", img + ":home:/", img2 + ":home:/")
    lst = run("ls", "-l", img2 + ":home:/").stdout
    check("copy keeps owners", "1000  1000" in lst)
    check("copy leaves existing files alone", "left alone" in run("copy", img + ":/", img2 + ":/", ok=False).stdout)
    check("fsck after all of it", run("fsck", img).returncode == 0 and run("fsck", img2).returncode == 0)

    shutil.rmtree(tmp)
    print("eudisk: %d checks, %d failures" % (len(results), results.count(False)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
