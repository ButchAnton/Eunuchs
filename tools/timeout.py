#!/usr/bin/env python3
"""timeout.py SECONDS CMD...: run CMD; kill it if it runs too long (exit 124).

QEMU ignores SIGALRM, so `perl -e 'alarm ...; exec'` does not work for it.
"""
import subprocess
import sys


def main():
    seconds, cmd = float(sys.argv[1]), sys.argv[2:]
    p = subprocess.Popen(cmd)
    try:
        sys.exit(p.wait(timeout=seconds))
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        print("\n*** timeout: %s killed after %gs" % (cmd[0], seconds), file=sys.stderr)
        sys.exit(124)


if __name__ == "__main__":
    main()
