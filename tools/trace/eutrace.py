#!/usr/bin/env python3
"""Decode Eunuchs kernel trace dumps.

    eutrace.py text     TRACE.bin [--elf build/kernel.elf]
    eutrace.py perfetto TRACE.bin OUT.json [--elf build/kernel.elf]

Dumps come from the monitor (`trace dump`, via QEMU semihosting), from
`make test` (build/ktest-trace.bin), or from GDB (`eu-trace-dump`).
Open the JSON in https://ui.perfetto.dev (or chrome://tracing).

The event table is parsed from kernel/include/trace_events.h, so this stays
in sync with the kernel. With --elf, code addresses are symbolized and string
pointers (mutex names, trace_mark messages) are resolved.
"""

import argparse
import bisect
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EVENTS_H = os.path.join(ROOT, "kernel", "include", "trace_events.h")
SYSCALL_H = os.path.join(ROOT, "lib", "include", "uapi", "syscall.h")
REC = struct.Struct("<QHHI3Q")          # struct trace_rec (40 bytes)
STATES = {0: "R", 1: "r", 2: "S", 3: "D"}
CAUSES = {1: "software", 5: "timer", 9: "external"}


def load_events(path=EVENTS_H):
    """[(name, group, [arg0, arg1, arg2])] in enum order."""
    events = []
    pat = re.compile(r'^\s*X\((\w+),\s*(\w+),\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\)')
    in_list = False
    with open(path) as f:
        for line in f:
            if line.startswith("#define TRACE_EVENTS"):
                in_list = True
                continue
            m = pat.match(line) if in_list else None
            if m:
                events.append((m.group(1), m.group(2), [m.group(3), m.group(4), m.group(5)]))
    return events


def load_syscalls(path=SYSCALL_H):
    """{number: name} from the SYSCALLS X-macro."""
    names = {}
    pat = re.compile(r'^\s*X\((\d+),\s*(\w+),')
    with open(path) as f:
        for line in f:
            m = pat.match(line)
            if m:
                names[int(m.group(1))] = m.group(2)
    return names


SYSCALLS = load_syscalls() if os.path.exists(SYSCALL_H) else {}


SIGNALS = {1: "SIGHUP", 2: "SIGINT", 3: "SIGQUIT", 4: "SIGILL", 5: "SIGTRAP", 6: "SIGABRT",
           7: "SIGBUS", 8: "SIGFPE", 9: "SIGKILL", 10: "SIGUSR1", 11: "SIGSEGV", 12: "SIGUSR2",
           13: "SIGPIPE", 14: "SIGALRM", 15: "SIGTERM", 17: "SIGCHLD", 18: "SIGCONT",
           19: "SIGSTOP", 20: "SIGTSTP", 21: "SIGTTIN", 22: "SIGTTOU", 28: "SIGWINCH"}


def signed(v):
    return v - (1 << 64) if v & (1 << 63) else v


class Elf:
    """Minimal ELF64 reader: function symbols and memory reads by address."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2:
            raise ValueError("not an ELF64 file: %s" % path)
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        secs = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize)
            secs.append(dict(name=name, type=typ, addr=addr, off=off, size=size, link=link,
                             entsize=entsize, flags=flags))
        self.sections = secs
        syms = []
        for s in secs:
            if s["type"] != 2:                      # SHT_SYMTAB
                continue
            strtab = secs[s["link"]]
            for j in range(s["size"] // 24):
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from("<IBBHQQ", d, s["off"] + j * 24)
                # functions, and global assembly labels in code (STT_NOTYPE)
                is_code = st_shndx < len(secs) and secs[st_shndx]["flags"] & 4
                if st_value and (st_info & 0xF == 2 or
                                 (st_info & 0xF == 0 and st_info >> 4 == 1 and is_code)):
                    end = d.index(b"\0", strtab["off"] + st_name)
                    syms.append((st_value, d[strtab["off"] + st_name:end].decode()))
        syms.sort()
        self.addrs = [a for a, _ in syms]
        self.names = [n for _, n in syms]

    def symbolize(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return "%#x" % addr
        off = addr - self.addrs[i]
        return self.names[i] + ("+%#x" % off if off else "")

    def read_cstr(self, addr, limit=64):
        for s in self.sections:
            if s["type"] == 1 and s["addr"] <= addr < s["addr"] + s["size"]:  # PROGBITS
                start = s["off"] + addr - s["addr"]
                end = self.data.find(b"\0", start, start + limit)
                return self.data[start:end if end >= 0 else start + limit].decode("utf-8", "replace")
        return None


class Trace:
    def __init__(self, raw, events=None, elf=None):
        self.events = events or load_events()
        self.elf = elf
        if raw[:8] != b"EUTRACE1":
            raise ValueError("not a Eunuchs trace dump")
        version, ncpus, timebase, rec_size, nthreads = struct.unpack_from("<IIQII", raw, 8)
        if rec_size != REC.size:
            raise ValueError("record size %d, expected %d" % (rec_size, REC.size))
        self.timebase = timebase or 10_000_000
        off = 32
        self.threads = {}
        for _ in range(nthreads):
            tid, state = struct.unpack_from("<II", raw, off)
            name = raw[off + 8:off + 24].split(b"\0")[0].decode("utf-8", "replace")
            self.threads[tid] = name
            off += 24
        self.records = []
        for _ in range(ncpus):
            count, = struct.unpack_from("<Q", raw, off)
            off += 8
            for _ in range(count):
                self.records.append(REC.unpack_from(raw, off))
                off += REC.size
        self.records.sort(key=lambda r: r[0])
        # Thread names also arrive via thread_create events (threads may be gone).
        for r in self.records:
            ev = self.event(r)
            if ev and ev[0] == "thread_create":
                self.threads.setdefault(r[4], self.name16(r))
        self.t0 = self.records[0][0] if self.records else 0

    @classmethod
    def from_file(cls, path, elf=None):
        with open(path, "rb") as f:
            return cls(f.read(), elf=elf)

    def event(self, r):
        return self.events[r[1]] if r[1] < len(self.events) else None

    @staticmethod
    def name16(r):
        return struct.pack("<QQ", r[5], r[6]).split(b"\0")[0].decode("utf-8", "replace")

    def tname(self, tid):
        return self.threads.get(tid, "?")

    def us(self, ts):
        return (ts - self.t0) * 1_000_000 / self.timebase

    def fmt_arg(self, name, v, r):
        if name == "nr":
            return SYSCALLS.get(v, "nr=%d" % v)
        if name == "sig":
            return SIGNALS.get(v, "sig=%d" % v)
        if name in ("ret", "result", "code", "status", "sector", "len", "us", "seq", "blocks",
                    "write") or name.endswith("pid"):
            return "%s=%d" % (name, signed(v))
        if name == "text24":
            return '"%s"' % struct.pack("<QQQ", r[4], r[5], r[6]).split(b"\0")[0].decode("utf-8", "replace")
        if name.endswith("tid"):
            return "%s=%d(%s)" % (name, v, self.tname(v))
        if name.endswith("state"):
            return "%s=%s" % (name, STATES.get(v, "?"))
        if name.endswith("pc") or name.endswith("site"):
            return "%s=%s" % (name, self.elf.symbolize(v) if self.elf else "%#x" % v)
        if name == "name16":
            return "name=%s" % self.name16(r)
        if name == "str":
            s = self.elf.read_cstr(v) if self.elf else None
            return '"%s"' % s if s is not None else "str=%#x" % v
        if name == "cause":
            return CAUSES.get(v, str(v))
        return "%s=%#x" % (name, v)

    def describe(self, r):
        ev = self.event(r)
        if not ev:
            return "event%d" % r[1], ""
        name, _, args = ev
        parts = []
        for i, a in enumerate(args):
            if args[0] == "text24" and i > 0:
                continue
            if a and not (i == 2 and args[1] == "name16") and not (a == "irq" and r[4 + i] == 0):
                parts.append(self.fmt_arg(a, r[4 + i], r))
        return name, " ".join(parts)

    def text(self, out):
        for r in self.records:
            name, args = self.describe(r)
            out.write("%12.3f us  c%d %3d %-10s %-15s %s\n" %
                      (self.us(r[0]), r[2], r[3], self.tname(r[3])[:10], name, args))

    def perfetto(self):
        """Chrome/Perfetto JSON: per-CPU tracks of who ran, per-thread tracks, IRQs."""
        ev = []
        CPU_PID, THR_PID = 1, 2
        ev.append({"ph": "M", "name": "process_name", "pid": CPU_PID, "args": {"name": "CPUs"}})
        ev.append({"ph": "M", "name": "process_name", "pid": THR_PID, "args": {"name": "Threads"}})
        for tid, name in self.threads.items():
            ev.append({"ph": "M", "name": "thread_name", "pid": THR_PID, "tid": tid,
                       "args": {"name": "%s (%d)" % (name, tid)}})
        cpus = sorted({r[2] for r in self.records}) or [0]
        for c in cpus:
            ev.append({"ph": "M", "name": "thread_name", "pid": CPU_PID, "tid": c,
                       "args": {"name": "cpu%d" % c}})
            ev.append({"ph": "M", "name": "thread_name", "pid": CPU_PID, "tid": 100 + c,
                       "args": {"name": "cpu%d irq" % c}})

        running = {}        # cpu -> (start_us, tid)
        irq_start = {}      # cpu -> start_us
        sys_start = {}      # tid -> (start_us, nr, a0, a1)
        for r in self.records:
            ts, typ, cpu, tid = r[0], r[1], r[2], r[3]
            t = self.us(ts)
            name, args = self.describe(r)
            if cpu not in running:
                running[cpu] = (self.us(self.t0), tid)
            if name == "sched_switch":
                start, cur = running[cpu]
                prev, nxt = r[4], r[5]
                for pid, track in ((CPU_PID, cpu), (THR_PID, prev)):
                    ev.append({"ph": "X", "pid": pid, "tid": track, "ts": start, "dur": t - start,
                               "name": "%s (%d)" % (self.tname(prev), prev) if pid == CPU_PID
                               else "running on cpu%d" % cpu,
                               "args": {"ended_in_state": STATES.get(r[6], "?")}})
                running[cpu] = (t, nxt)
            elif name == "syscall_enter":
                sys_start[tid] = (t, r[4], r[5], r[6])
            elif name == "syscall_exit" and tid in sys_start:
                start, nr, a0, a1 = sys_start.pop(tid)
                ev.append({"ph": "X", "pid": THR_PID, "tid": tid, "ts": start,
                           "dur": max(t - start, 0.01), "name": SYSCALLS.get(nr, "syscall %d" % nr),
                           "args": {"a0": hex(a0), "a1": hex(a1), "ret": signed(r[5])}})
            elif name == "irq_entry":
                irq_start[cpu] = t
            elif name == "irq_exit":
                start = irq_start.pop(cpu, t)
                label = CAUSES.get(r[4], "irq")
                if r[5]:
                    label += " (plic %d)" % r[5]
                ev.append({"ph": "X", "pid": CPU_PID, "tid": 100 + cpu, "ts": start,
                           "dur": max(t - start, 0.01), "name": label})
            else:
                track = r[4] if name in ("sched_wakeup", "thread_create", "thread_exit") else tid
                if name == "syscall_exit":
                    continue
                ev.append({"ph": "i", "s": "t", "pid": THR_PID, "tid": track, "ts": t,
                           "name": name, "args": {"detail": args, "by": "%d(%s)" %
                                                  (tid, self.tname(tid))}})
        end = self.us(self.records[-1][0]) if self.records else 0
        for cpu, (start, tid) in running.items():
            ev.append({"ph": "X", "pid": CPU_PID, "tid": cpu, "ts": start, "dur": end - start,
                       "name": "%s (%d)" % (self.tname(tid), tid)})
        return {"traceEvents": ev, "displayTimeUnit": "us",
                "metadata": {"source": "Eunuchs kernel trace", "timebase": self.timebase}}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["text", "perfetto"])
    ap.add_argument("dump")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "kernel.elf"))
    a = ap.parse_args()

    elf = Elf(a.elf) if a.elf and os.path.exists(a.elf) else None
    tr = Trace.from_file(a.dump, elf=elf)
    if a.mode == "text":
        tr.text(sys.stdout)
    else:
        out = a.out or os.path.splitext(a.dump)[0] + ".json"
        with open(out, "w") as f:
            json.dump(tr.perfetto(), f)
        print("%s: %d events, %d threads -> %s (open in https://ui.perfetto.dev)" %
              (a.dump, len(tr.records), len(tr.threads), out))


if __name__ == "__main__":
    main()
