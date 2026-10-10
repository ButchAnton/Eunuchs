#!/usr/bin/env python3
"""make ci: everything, headless, from a clean build.

Steps (each one's output is in build/ci/<step>.log):
  build         clean build of the kernel, user programs and disk image (-Werror)
  host tests    printf, buddy allocator, TCP over a lossy link, fsck (ASan/UBSan)
  test (4 CPUs) the full scenario: kernel+user tests, crash recovery, SMP
                stress, shell, networking (tools/run_tests.py)
  test (1 CPU)  the same on a uniprocessor
  soak          SMP stress for STRESS_SECS (default 60) with lockdep=panic
  replay        record a boot, replay it, require identical output
Exit status 0 only if every step passed. Options: --quick (skip the 1-CPU run
and the soak), --keep-going (run every step even after a failure).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--keep-going", action="store_true")
    ap.add_argument("--stress-secs", default=os.environ.get("STRESS_SECS", "60"))
    a = ap.parse_args()
    make = [os.environ.get("MAKE", "make"), "--no-print-directory", "-C", ROOT]
    jobs = "-j%d" % (os.cpu_count() or 4)

    shutil.rmtree(os.path.join(ROOT, "build"), ignore_errors=True)
    logdir = os.path.join(ROOT, "build", "ci")
    os.makedirs(logdir, exist_ok=True)

    steps = [
        ("build", make + [jobs, "all", "user"]),
        ("host tests", make + ["test-host"]),
        ("test (4 CPUs)", make + ["test", "CPUS=4"]),
        ("test (1 CPU)", make + ["test", "CPUS=1"]),
        ("soak", make + ["stress", "CPUS=4", "STRESS_SECS=%s" % a.stress_secs]),
        ("replay", make + ["replay-check"]),
    ]
    if a.quick:
        steps = [s for s in steps if s[0] not in ("test (1 CPU)", "soak")]

    results = []
    t_all = time.time()
    for name, cmd in steps:
        t0 = time.time()
        log = os.path.join(logdir, "".join(c if c.isalnum() else "-" for c in name).strip("-") + ".log")
        tty = sys.stdout.isatty()
        if tty:
            print("ci: %-14s ..." % name, end="", flush=True)
        with open(log, "w") as f:
            rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        ok = rc == 0
        results.append((name, ok, time.time() - t0, log))
        print("%sci: %-14s %s  (%.0fs)" % ("\r" if tty else "", name, "ok" if ok else "FAILED",
                                          time.time() - t0), flush=True)
        if not ok:
            print("    --- last lines of %s ---" % os.path.relpath(log, ROOT))
            with open(log) as f:
                for line in f.read().splitlines()[-20:]:
                    print("    " + line)
            if not a.keep_going:
                break
    failed = [r for r in results if not r[1]]
    print("ci: %d of %d steps passed in %.0fs%s" % (
        len(results) - len(failed), len(steps), time.time() - t_all,
        "" if not failed and len(results) == len(steps) else
        " -- FAILED: " + ", ".join(r[0] for r in failed)))
    return 0 if not failed and len(results) == len(steps) else 1


if __name__ == "__main__":
    sys.exit(main())
