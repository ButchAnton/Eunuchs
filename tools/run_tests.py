#!/usr/bin/env python3
"""Run the full Eunuchs test scenario (make test).

Stages run against a scratch copy of the disk image, so build/fs.img is
untouched. Each stage's console output goes to build/test-logs/<stage>.log;
on failure the tail of the log is shown.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Terminal control sequences from the shell's line editor (colors, cursor
# movement, OSC 133 prompt marks): dropped from what tests match against.
ESCAPES = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[78=>]")


class Console:
    """Minimal expect: drive the guest's serial console."""

    def __init__(self, cmd, log):
        import select  # noqa: F401
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""
        self.raw = b""          # undecoded tail: possibly the start of an escape sequence
        self.log = log

    def _add(self, d):
        self.raw += d
        cut = self.raw.rfind(b"\x1b")
        keep = b""
        if cut >= 0 and not ESCAPES.match(self.raw, cut) and len(self.raw) - cut < 64:
            self.raw, keep = self.raw[:cut], self.raw[cut:]
        text = ESCAPES.sub(b"", self.raw)
        self.raw = keep
        self.buf += text
        self.log.write(text.decode("utf-8", "replace"))

    def expect(self, text, timeout=10):
        import select
        end = time.time() + timeout
        want = text.encode()
        while want not in self.buf:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("expected %r" % text)
            r, _, _ = select.select([self.p.stdout], [], [], min(left, 0.2))
            if r:
                d = os.read(self.p.stdout.fileno(), 65536)
                if not d:
                    raise EOFError("console closed while expecting %r" % text)
                self._add(d)
        i = self.buf.index(want) + len(want)
        self.buf = self.buf[i:]

    def send(self, data):
        self.p.stdin.write(data.encode() if isinstance(data, str) else data)
        self.p.stdin.flush()

    def line(self, text):
        self.send(text + "\r")

    def log_text(self):
        self.log.flush()
        return open(self.log.name).read()

    def close(self):
        self.p.kill()
        self.p.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True, help="QEMU command without the disk")
    ap.add_argument("--image", required=True)
    ap.add_argument("--mkfs", required=True)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--append", default="", help="extra kernel options for every boot")
    a = ap.parse_args()

    build = os.path.join(ROOT, "build")
    logs = os.path.join(build, "test-logs")
    os.makedirs(logs, exist_ok=True)
    img = os.path.join(build, "test-fs.img")
    shutil.copyfile(a.image, img)
    disk = ["-global", "virtio-mmio.force-legacy=false",
            "-drive", "file=%s,if=none,format=raw,id=hd0" % img,
            "-device", "virtio-blk-device,drive=hd0"]
    results = []

    def stage(name, cmd, expect_rc=0, expect=(), forbid=()):
        t0 = time.time()
        log = os.path.join(logs, name + ".log")
        try:
            p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=a.timeout)
            out, rc = p.stdout.decode("utf-8", "replace"), p.returncode
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode("utf-8", "replace") + "\n*** TIMEOUT\n"
            rc = "timeout"
            subprocess.run(["pkill", "-f", img], stderr=subprocess.DEVNULL)
        with open(log, "w") as f:
            f.write(out)
        problems = []
        if rc != expect_rc:
            problems.append("exit status %s, expected %s" % (rc, expect_rc))
        problems += ["missing: %r" % m for m in expect if m not in out]
        problems += ["unexpected: %r" % m for m in forbid if m in out]
        ok = not problems
        results.append((name, ok, time.time() - t0))
        print("  %-22s %s  (%.1fs)" % (name, "ok" if ok else "FAILED: " + "; ".join(problems),
                                       time.time() - t0), flush=True)
        if not ok:
            print("    --- last lines of %s ---" % os.path.relpath(log, ROOT))
            for line in out.splitlines()[-30:]:
                print("    " + line)
        return ok

    qemu = shlex.split(a.qemu) + disk

    def boot(name, opts, **kw):
        return stage(name, qemu + ["-append", "panic=shutdown lockdep=panic %s %s" % (opts, a.append)], **kw)

    def fsck(name, **kw):
        return stage(name, [a.mkfs, "--check", img], **kw)

    def interactive(name, steps):
        """steps: (send, expect[, prompt]) tuples. send is a command line (then we
        wait for `expect` and the next prompt, unless prompt=False) or bytes for
        raw keys like ^C (sent after a short pause so the target is running)."""
        t0 = time.time()
        logpath = os.path.join(logs, name + ".log")
        with open(logpath, "w") as log:
            con = Console(qemu + ["-append", "panic=shutdown lockdep=panic " + a.append], log)
            problem = None
            try:
                con.expect("root@eunuchs:/# ", 20)
                for step in steps:
                    send, want = step[0], step[1]
                    prompt = step[2] if len(step) > 2 else True
                    if isinstance(send, bytes):
                        time.sleep(0.5)
                        con.send(send)
                    elif send is not None:
                        con.line(send)
                    if want:
                        con.expect(want)
                    if prompt:
                        con.expect("@eunuchs:")     # the next prompt: command finished
                        con.expect(" ")
            except Exception as e:
                problem = "%s (after sending %r)" % (e, send)
            con.close()
        ok = problem is None
        results.append((name, ok, time.time() - t0))
        print("  %-22s %s  (%.1fs)" % (name, "ok" if ok else "FAILED: " + problem, time.time() - t0),
              flush=True)
        if not ok:
            print("    --- last lines of %s ---" % os.path.relpath(logpath, ROOT))
            for line in open(logpath).read().splitlines()[-25:]:
                print("    " + line)

    def free_port():
        import socket
        so = socket.socket()
        so.bind(("127.0.0.1", 0))
        port = so.getsockname()[1]
        so.close()
        return port

    def network(name, opts, script):
        """Boot with a NIC (QEMU user networking) whose guest ports 80, 5000
        and UDP 7 are forwarded to free host ports; then run script(con, ports)
        against the console and the network. Captures build/test-logs/<name>.pcap."""
        t0 = time.time()
        logpath = os.path.join(logs, name + ".log")
        ports = {"http": free_port(), "data": free_port(), "udp": free_port()}
        net = ["-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:80,hostfwd=tcp:127.0.0.1:%d-:5000,"
               "hostfwd=udp:127.0.0.1:%d-:7" % (ports["http"], ports["data"], ports["udp"]),
               "-device", "virtio-net-device,netdev=net0",
               "-object", "filter-dump,id=pcap0,netdev=net0,file=%s" %
               os.path.join(logs, "".join(c if c.isalnum() else "-" for c in name) + ".pcap")]
        with open(logpath, "w") as log:
            con = Console(qemu + net + ["-append", "panic=shutdown lockdep=panic %s %s" %
                                        (opts, a.append)], log)
            problem = None
            try:
                con.expect("root@eunuchs:/# ", 20)
                script(con, ports)
            except Exception as e:
                problem = "%s: %s" % (type(e).__name__, e)
            con.close()
        ok = problem is None
        results.append((name, ok, time.time() - t0))
        print("  %-22s %s  (%.1fs)" % (name, "ok" if ok else "FAILED: " + problem, time.time() - t0),
              flush=True)
        if not ok:
            print("    --- last lines of %s ---" % os.path.relpath(logpath, ROOT))
            for line in open(logpath).read().splitlines()[-25:]:
                print("    " + line)

    def cmd(con, line, want=None, timeout=10):
        if line.endswith("&"):                  # its output comes after the next prompt
            con.line(line)
            con.expect(want, timeout)
            return
        con.line(line)
        if want:
            con.expect(want, timeout)
        con.expect("@eunuchs:", timeout)
        con.expect(" ")

    def bulk_to_guest(con, ports, size):
        """Send `size` random bytes from the Mac to `nc -l 5000 | crc32` in the guest."""
        import socket
        import zlib
        blob = os.urandom(size)
        con.line("nc -l 5000 | crc32")
        con.expect("crc32")
        for _ in range(50):                     # until the listener is up
            try:
                so = socket.create_connection(("127.0.0.1", ports["data"]), timeout=60)
                so.sendall(blob)
                so.shutdown(socket.SHUT_WR)
                so.recv(1)
                so.close()
                break
            except (ConnectionResetError, BrokenPipeError, ConnectionRefusedError):
                time.sleep(0.2)
        t0 = time.time()
        con.expect("%08x %d" % (zlib.crc32(blob), size), 60)
        con.log.write("\n[harness: %d bytes in %.1f s]\n" % (size, time.time() - t0))
        con.expect("@eunuchs:", 10)

    def net_basics(con, ports):
        import socket
        import urllib.request
        cmd(con, "ping -c 2 gateway", "0% packet loss")
        cmd(con, "ping -c 2 localhost", "0% packet loss")
        cmd(con, "httpd &", "serving /www")
        cmd(con, "echod &", "echoing")
        page = urllib.request.urlopen("http://127.0.0.1:%d/" % ports["http"], timeout=10).read()
        assert b"Hello from Eunuchs" in page, "unexpected page %r" % page[:80]
        tcp = urllib.request.urlopen("http://127.0.0.1:%d/proc/net/tcp" % ports["http"],
                                     timeout=10).read()
        assert b"ESTABLISHED" in tcp, "own connection missing from /proc/net/tcp"
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.settimeout(5)
        u.sendto(b"udp echo test", ("127.0.0.1", ports["udp"]))
        assert u.recvfrom(100)[0] == b"udp echo test", "no UDP echo"
        bulk_to_guest(con, ports, 1 << 20)
        # guest -> Mac: the guest connects out to 10.0.2.2 (our 127.0.0.1)
        import zlib
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        srv.settimeout(30)
        con.line("crc32 /bin/sh; nc host %d < /bin/sh" % srv.getsockname()[1])
        c, _ = srv.accept()
        data = b""
        c.settimeout(30)
        while True:
            d = c.recv(65536)
            if not d:
                break
            data += d
        c.close()                               # nc waits for our FIN too
        con.expect("%08x %d /bin/sh" % (zlib.crc32(data), len(data)), 10)
        con.expect("@eunuchs:", 10)
        cmd(con, "netstat -s", "retransmits")

    def net_lossy(con, ports):
        """With net.drop=N every received packet (on lo too) is dropped with
        probability N%: our TCP at both ends must still deliver every byte."""
        import re
        cmd(con, "cat /bin/sh /bin/eutils /bin/utests /bin/nc /bin/ls > /tmp/big; crc32 /tmp/big",
            "/tmp/big")
        want = re.findall(r"([0-9a-f]{8} \d+) /tmp/big", con.log_text())[-1]
        cmd(con, "nc -l 6000 | crc32 > /tmp/got &", "[1]")
        cmd(con, "nc localhost 6000 < /tmp/big; sleep 1; cat /tmp/got", want, 60)
        bulk_to_guest(con, ports, 128 << 10)    # QEMU's TCP is the sender here: keep it short
        cmd(con, "netstat -s", "dropped on purpose")

    print("Eunuchs test scenario (logs in build/test-logs):")
    boot("kernel+user tests", "ktest utest tracedump=build/ktest-trace.bin",
         expect=["KTEST RESULT: PASS", "UTEST RESULT: PASS"], forbid=["filesystem error"])
    fsck("fsck after tests", expect=["clean"], forbid=["note: log holds"])
    boot("persistence", "utest=persist-check", expect=["UTEST RESULT: PASS"])
    # Power failure just after the commit point: recovery must replay it.
    boot("crash after commit", "utest=crash-commit-write", expect_rc=3,
         expect=["simulating a power failure"])
    fsck("fsck (committed log)", expect=["clean", "note: log holds a committed transaction"])
    boot("recovery replays", "utest=crash-commit-check",
         expect=["recovering: replaying committed transaction", "UTEST RESULT: PASS"])
    fsck("fsck after replay", expect=["clean"], forbid=["note: log holds"])
    # Power failure before the commit point: the transaction must vanish.
    boot("crash before commit", "utest=crash-log-write", expect_rc=3,
         expect=["simulating a power failure"])
    fsck("fsck (uncommitted)", expect=["clean"], forbid=["note: log holds"])
    boot("recovery discards", "utest=crash-log-check", expect=["UTEST RESULT: PASS"],
         forbid=["recovering: replaying"])
    fsck("fsck final", expect=["clean"])
    # Many processes on every CPU at once, with lockdep=panic and the lockup
    # watchdog armed (make stress runs the same for longer).
    boot("smp stress", "utest=stress", expect=["STRESS RESULT: PASS", "UTEST RESULT: PASS"],
         forbid=["LOCKDEP", "HUNG THREAD", "filesystem error"])
    fsck("fsck after stress", expect=["clean"], forbid=["note: log holds"])

    # The shell, driven like a user at the console.
    interactive("interactive shell", [
        ("echo hello | cat", "hello"),
        ("ls / | grep bin > /tmp/x; cat /tmp/x", "bin\r\n"),
        ("echo abc > /tmp/f; wc -c /tmp/f", "4"),
        ("cat", None, False), (b"\x03", None),             # ^C interrupts cat
        ("echo status=$?", "status=130"),
        ("sleep 30", None, False), (b"\x1a", "esh: suspended  sleep 30"),  # ^Z
        ("jobs", "[1]  + suspended  sleep 30"),
        ("kill %1", "[1]  + terminated  sleep 30"),    # reported as soon as it happens
        ("sleep 1 &", "[1] "), ("wait", None),
        ("V=hi sh -c 'echo inner=$V'", "inner=hi"),
        ("false || echo or-ok && echo and-ok", "and-ok"),
        ("nosuch", "esh: command not found: nosuch"),
        ("segv null", "esh: segmentation fault (core dumped)  segv null"),
        ("x=(a b c); echo ${#x} $x[2] ${(U)x[3]}", "3 b C"),
        ("f() { echo f:$1; return 3 }; f arg; echo rc=$?", "rc=3"),
        ("for i in {1..3}; do print -n $i; done; echo", "123\r\n"),
        ("echo $(( 6 * 7 )) ${HOME:-none} ~", "42 / /"),
        ("[[ abc == a* ]] && echo cond-ok", "cond-ok"),
        ("cat <<EOF", None, False), ("one $((1+1))", None, False), ("EOF", "one 2"),
        ("su guest", None, False), (None, "guest@eunuchs:~% ", False),
        ("id", "uid=1000(guest)"),
        ("touch /etc/nope", "Permission denied"),
        ("exit", None),
        ("cat /proc/self/status | grep pgid", "pgid"),
    ])

    # Networking through QEMU's user-mode network: ping, HTTP from the Mac,
    # UDP, 1 MiB each way with CRC checks; then with 5% packet loss (over
    # loopback, so our TCP is at both ends; and a short transfer from the Mac).
    network("network", "", net_basics)
    network("network, 5% loss", "net.drop=5", net_lossy)

    trace = os.path.join(build, "ktest-trace.bin")
    if os.path.exists(trace):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools/trace/eutrace.py"), "perfetto",
                        trace, os.path.join(build, "ktest-trace.json")])

    failed = [n for n, ok, _ in results if not ok]
    print("%d stages, %d passed, %d failed%s" % (len(results), len(results) - len(failed),
                                                 len(failed), ": " + ", ".join(failed) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
