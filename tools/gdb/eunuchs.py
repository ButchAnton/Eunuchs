"""Eunuchs kernel GDB helpers.

Loaded by tools/gdb/init.gdb (`make gdb`). All commands are prefixed `eu-`;
`eu-help` lists them. They read kernel data structures directly, so they work
even when the kernel is wedged or has panicked.
"""

import os
import struct
import sys

import gdb

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "trace"))
import eutrace  # noqa: E402

LOG_LEVELS = ["err", "warn", "info", "debug", "trace"]


def _inferior():
    return gdb.selected_inferior()


def _read(addr, size):
    return bytes(_inferior().read_memory(addr, size))


def _symbolize(addr):
    try:
        block = gdb.block_for_pc(addr)
        while block and not block.function:
            block = block.superblock
        if block and block.function:
            return "%s+%#x" % (block.function.name, addr - block.start)
    except RuntimeError:
        pass
    # Assembly routines have no debug blocks; fall back to the ELF symtab.
    info = gdb.execute("info symbol %#x" % addr, to_string=True).strip()
    if info.startswith("No symbol"):
        return ""
    name, _, rest = info.partition(" in section")
    return name.replace(" + ", "+")


def _eval_int(expr):
    return int(gdb.parse_and_eval(expr))


class EuCommand(gdb.Command):
    registry = []

    def __init__(self, name, summary):
        super().__init__(name, gdb.COMMAND_USER)
        EuCommand.registry.append((name, summary))


class EuHelp(EuCommand):
    """List Eunuchs GDB commands."""

    def __init__(self):
        super().__init__("eu-help", "list Eunuchs commands")

    def invoke(self, arg, from_tty):
        for name, summary in EuCommand.registry:
            gdb.write("  %-12s %s\n" % (name, summary))


class EuDmesg(EuCommand):
    """eu-dmesg [N]: print the kernel log ring (last N lines)."""

    def __init__(self):
        super().__init__("eu-dmesg", "kernel log ring [last N lines]")

    def invoke(self, arg, from_tty):
        buf = gdb.parse_and_eval("log_buf")
        size = buf.type.sizeof
        head = _eval_int("log_head")
        mem = _read(int(buf.address), size)
        if head <= size:
            data = mem[:head]
        else:
            start = head % size
            data = mem[start:] + mem[:start]
            data = data[data.find(b"\n") + 1:]   # drop partial oldest line
        lines = data.decode("utf-8", "replace").splitlines()
        if arg.strip():
            lines = lines[-int(arg):]
        for line in lines:
            gdb.write(line + "\n")


class EuLog(EuCommand):
    """eu-log [subsys level]: show or set per-subsystem log thresholds."""

    def __init__(self):
        super().__init__("eu-log", "show/set log thresholds [subsys level]")

    def invoke(self, arg, from_tty):
        n = _eval_int("sizeof(log_threshold)")
        names = [gdb.parse_and_eval("log_subsys_names[%d]" % i).string() for i in range(n)]
        args = arg.split()
        if len(args) == 2:
            subsys, level = args
            lvl = LOG_LEVELS.index(level) if level in LOG_LEVELS else int(level)
            targets = range(n) if subsys == "all" else [names.index(subsys)]
            for i in targets:
                gdb.execute("set var log_threshold[%d] = %d" % (i, lvl), to_string=True)
        for i, name in enumerate(names):
            gdb.write("  %-8s %s\n" % (name, LOG_LEVELS[_eval_int("log_threshold[%d]" % i)]))


REG_NAMES = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
             "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
             "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
             "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"]


class EuTrapFrame(EuCommand):
    """eu-tf [EXPR]: pretty-print a struct trap_frame (default: `tf` in scope)."""

    def __init__(self):
        super().__init__("eu-tf", "pretty-print a trap frame [expr, default tf]")

    def invoke(self, arg, from_tty):
        expr = arg.strip() or "tf"
        tf = gdb.parse_and_eval(expr)
        if tf.type.code != gdb.TYPE_CODE_PTR:
            tf = tf.address
        tf = tf.cast(gdb.lookup_type("struct trap_frame").pointer()).dereference()
        sepc = int(tf["sepc"])
        cause = int(tf["scause"])
        name = gdb.parse_and_eval("trap_cause_name(%d)" % cause).string()
        gdb.write("  sepc    %016x  %s\n" % (sepc, _symbolize(sepc)))
        gdb.write("  scause  %016x  %s\n" % (cause, name))
        gdb.write("  stval   %016x\n" % int(tf["stval"]))
        gdb.write("  sstatus %016x\n" % int(tf["sstatus"]))
        regs = tf["regs"]
        for i in range(0, 32, 4):
            gdb.write("  " + "  ".join("%-4s %016x" % (REG_NAMES[j], int(regs[j]) & (2**64 - 1))
                                       for j in range(i, i + 4)) + "\n")


class EuPlatform(EuCommand):
    """eu-platform: show hardware discovered from the device tree."""

    def __init__(self):
        super().__init__("eu-platform", "hardware discovered from the device tree")

    def invoke(self, arg, from_tty):
        p = gdb.parse_and_eval("platform")
        gdb.write("  model    %s\n" % p["model"].string())
        gdb.write("  harts    %d\n" % int(p["ncpus"]))
        gdb.write("  memory   %#x + %d MiB\n" % (int(p["mem_base"]), int(p["mem_size"]) >> 20))
        for dev in ("uart", "plic", "clint", "rtc", "test"):
            gdb.write("  %-8s %#x irq %d\n" % (dev, int(p[dev]["base"]), int(p[dev]["irq"])))
        for i in range(int(p["nvirtio"])):
            v = p["virtio"][i]
            gdb.write("  virtio%d  %#x irq %d\n" % (i, int(v["base"]), int(v["irq"])))
        gdb.write("  bootargs \"%s\"\n" % p["bootargs"].string())


KERNEL_BASE = 0xffffffc000000000
PAGE_SIZE = 4096


def _static(name):
    """Value of a (possibly file-static) kernel variable."""
    sym = gdb.lookup_static_symbol(name) or gdb.lookup_global_symbol(name)
    if sym is None:
        raise gdb.GdbError("symbol %s not found" % name)
    return sym.value()


def _addr(expr):
    """Evaluate an address expression: number, symbol (incl. functions), or C expr."""
    v = gdb.parse_and_eval(expr)
    if v.type.code == gdb.TYPE_CODE_FUNC:
        v = v.address
    return int(v.cast(gdb.lookup_type("unsigned long"))) & (2**64 - 1)


def _u64(addr):
    return int.from_bytes(_read(addr, 8), "little")


def _pte_flags(pte):
    return "".join(c if pte & (1 << i) else "-" for i, c in
                   ((1, "r"), (2, "w"), (3, "x"), (4, "u"), (5, "g"), (6, "a"), (7, "d")))


def _sext(va):
    return va | 0xffffff8000000000 if va & (1 << 38) else va


class EuVm(EuCommand):
    """eu-vm [VA]: dump the active page table (coalesced), or walk one address."""

    def __init__(self):
        super().__init__("eu-vm", "page table dump, or walk [va]")

    def invoke(self, arg, from_tty):
        satp = int(gdb.parse_and_eval("$satp")) & (2**64 - 1)
        if satp >> 60 != 8:
            gdb.write("paging is off (satp=%#x)\n" % satp)
            return
        root = (satp & ((1 << 44) - 1)) << 12
        if arg.strip():
            self.walk(root, _addr(arg))
        else:
            self.dump(root)

    def walk(self, table_pa, va):
        for level in (2, 1, 0):
            idx = (va >> (12 + 9 * level)) & 0x1ff
            pte = _u64(KERNEL_BASE + table_pa + idx * 8)
            gdb.write("  L%d[%3d] pte %016x %s" % (level, idx, pte, _pte_flags(pte)))
            if not pte & 1:
                gdb.write(" -> not present\n")
                return
            pa = (pte >> 10) << 12
            if pte & 0xe:
                off = va & ((1 << (12 + 9 * level)) - 1)
                gdb.write(" -> pa %#x\n" % (pa + off))
                return
            gdb.write(" -> next table\n")
            table_pa = pa

    def dump(self, root):
        runs = []

        def leaf(va, pa, perm, size):
            if runs and runs[-1][1] == va and runs[-1][3] == perm and \
                    runs[-1][2] + (runs[-1][1] - runs[-1][0]) == pa:
                runs[-1][1] += size
            else:
                runs.append([va, va + size, pa, perm])

        def level(table_pa, lvl, base):
            data = _read(KERNEL_BASE + table_pa, PAGE_SIZE)
            for i in range(512):
                pte = int.from_bytes(data[i * 8:i * 8 + 8], "little")
                if not pte & 1:
                    continue
                va = _sext(base | (i << (12 + 9 * lvl)))
                pa = (pte >> 10) << 12
                if pte & 0xe or lvl == 0:
                    leaf(va, pa, pte & 0xfe, 1 << (12 + 9 * lvl))
                else:
                    level(pa, lvl - 1, va & ((1 << 39) - 1))

        level(root, 2, 0)
        for va0, va1, pa, perm in runs:
            gdb.write("  %016x-%016x -> %010x %8d KiB  %s\n" %
                      (va0, va1 - 1, pa, (va1 - va0) >> 10, _pte_flags(perm)))


class EuMeminfo(EuCommand):
    """eu-meminfo: physical memory allocator state."""

    def __init__(self):
        super().__init__("eu-meminfo", "physical memory allocator state")

    def invoke(self, arg, from_tty):
        zone = _static("zone")
        npages = int(zone["npages"])
        free = int(zone["free_pages"])
        gdb.write("  pages %d, free %d (%d KiB), corruptions %d\n" %
                  (npages, free, free * 4, int(_static("mm_corruptions"))))
        gdb.write("  free blocks by order: %s\n" % " ".join(
            "%d:%d" % (o, int(zone["nr_free"][o])) for o in range(11)))
        regions = _static("reserved_regions")
        for i in range(int(_static("nr_reserved_regions"))):
            r = regions[i]
            gdb.write("  reserved %#x-%#x  %s\n" % (int(r["start"]), int(r["end"]),
                                                    r["what"].string()))


class EuPage(EuCommand):
    """eu-page ADDR: struct page for a physical or direct-map virtual address."""

    def __init__(self):
        super().__init__("eu-page", "struct page for a pa or va")

    FLAGS = ["free", "reserved", "slab", "large", "pagetable", "tail"]

    def invoke(self, arg, from_tty):
        addr = _addr(arg)
        pa = addr - KERNEL_BASE if addr >= KERNEL_BASE else addr
        zone = _static("zone")
        idx = (pa >> 12) - int(zone["base_pfn"])
        if not 0 <= idx < int(zone["npages"]):
            gdb.write("not managed RAM\n")
            return
        pg = zone["pages"][idx]
        flags = int(pg["flags"])
        names = [n for i, n in enumerate(self.FLAGS) if flags & (1 << i)] or ["allocated"]
        gdb.write("  pa %#x  flags %s  order %d  refcount %d\n" %
                  (pa, ",".join(names), int(pg["order"]), int(pg["refcount"])))
        for f in ("alloc_site", "free_site"):
            site = int(pg[f])
            gdb.write("  %-10s %s\n" % (f, _symbolize(site) if site else "-"))


class EuSlab(EuCommand):
    """eu-slab: slab cache statistics."""

    def __init__(self):
        super().__init__("eu-slab", "slab cache statistics")

    def invoke(self, arg, from_tty):
        caches = _static("caches")
        gdb.write("  %-16s %6s %8s %6s %10s %10s\n" % ("cache", "objsz", "active", "slabs",
                                                       "allocs", "frees"))
        for i in range(int(_static("nr_caches"))):
            c = caches[i]
            gdb.write("  %-16s %6d %8d %6d %10d %10d\n" % (
                c["name"].string(), int(c["obj_size"]), int(c["active"]),
                int(c["nr_slabs"]), int(c["allocs"]), int(c["frees"])))


STATE = {0: "R", 1: "r", 2: "S", 3: "D"}


def _threads():
    """Yield gdb.Value struct thread for every thread in all_threads."""
    head = _static("all_threads")
    ttype = gdb.lookup_type("struct thread")
    off = [f.bitpos // 8 for f in ttype.fields() if f.name == "all_node"][0]
    node = head["next"]
    head_addr = int(head.address)
    while int(node) != head_addr:
        yield gdb.Value(int(node) - off).cast(ttype.pointer()).dereference()
        node = node["next"]


def _walk_fp(pc, fp, limit=32):
    frames = [pc]
    while fp and not fp & 7 and len(frames) < limit:
        try:
            ra = _u64(fp - 8)
            nxt = _u64(fp - 16)
        except gdb.MemoryError:
            break
        if not ra:
            break
        frames.append(ra)
        if nxt <= fp:
            break
        fp = nxt
    return frames


def _ncpus():
    return _eval_int("ncpus_online")


def _running():
    """thread address -> CPU id, for every CPU's current thread."""
    return {_eval_int("(unsigned long)cpus[%d].cur_thread" % c): c for c in range(_ncpus())}


def _gdb_thread_of(cpu):
    """QEMU's gdbstub shows each vCPU as a thread, numbered hart id + 1."""
    return _eval_int("cpus[%d].hartid" % cpu) + 1


class EuPs(EuCommand):
    """eu-ps: kernel threads (works on a halted or wedged kernel)."""

    def __init__(self):
        super().__init__("eu-ps", "kernel threads")

    def invoke(self, arg, from_tty):
        running = _running()
        gdb.write("  %4s %-16s %-2s %-4s %8s  %s\n" % ("TID", "NAME", "ST", "CPU", "SWITCHES",
                                                      "WAITING ON"))
        for t in _threads():
            wait = t["wait_what"]
            what = wait.string() if int(wait) else ""
            if int(t["blocked_on"]):
                what += " '%s'" % t["blocked_on"]["name"].string()
            cpu = running.get(int(t.address))
            mark = "*" if cpu is not None else " "
            gdb.write(" %s%4d %-16s %-2s %-4s %8d  %s\n" % (
                mark, int(t["tid"]), t["name"].string(), STATE.get(int(t["state"]), "?"),
                "%d" % int(t["cpu"]), int(t["switches"]), what))
        gdb.write("  (* = running now; CPU = where it runs or last ran; eu-cpus for CPUs)\n")


class EuCpus(EuCommand):
    """eu-cpus: every CPU, its current thread and held spinlocks, and its GDB thread."""

    def __init__(self):
        super().__init__("eu-cpus", "CPUs: current thread, locks, GDB thread")

    def invoke(self, arg, from_tty):
        gdb.write("  CPU HART GDB-THREAD THREAD               IRQ STOPPED IPI-PENDING  LOCKS HELD\n")
        for c in range(_ncpus()):
            cpu = gdb.parse_and_eval("cpus[%d]" % c)
            t = cpu["cur_thread"]
            name = "%d(%s)" % (int(t["tid"]), t["name"].string()) if int(t) else "-"
            held = [cpu["held"][i]["name"].string()
                    for i in range(min(int(cpu["nheld"]), 8)) if int(cpu["held"][i]["name"])]
            gdb.write("  %3d %4d %10d %-20s %3d %7s %11s  %s\n" % (
                c, int(cpu["hartid"]), _gdb_thread_of(c), name, int(cpu["irq_depth"]),
                "yes" if int(cpu["stopped"]) else "no", "%#x" % int(cpu["ipi_pending"]),
                ", ".join(held) or "-"))
        gdb.write("  'thread <GDB-THREAD>' then 'bt' shows what that CPU is doing\n")


class EuLockdep(EuCommand):
    """eu-lockdep: lock classes and the lock-order dependencies lockdep has seen."""

    def __init__(self):
        super().__init__("eu-lockdep", "lock classes and order dependencies")

    def invoke(self, arg, from_tty):
        n = int(_static("nclasses"))
        classes = _static("classes")
        deps = _static("deps")
        kinds = {0: "spinlock", 1: "mutex"}
        names = {}
        for i in range(1, n):
            names[i] = classes[i]["name"].string()
        for i in range(1, n):
            after = [("mutex:" if int(classes[b]["kind"]) == 1 else "") + names[b]
                     for b in range(1, n) if int(deps[i][b // 64]) >> (b % 64) & 1]
            gdb.write("  %-8s %-18s%s\n" % (kinds.get(int(classes[i]["kind"]), "?"), names[i],
                                            " -> " + " ".join(after) if after else ""))


class EuBt(EuCommand):
    """eu-bt TID: backtrace of any kernel thread (sleeping ones from their saved context)."""

    def __init__(self):
        super().__init__("eu-bt", "backtrace of a thread by tid")

    def invoke(self, arg, from_tty):
        tid = int(arg)
        running = _running()
        for t in _threads():
            if int(t["tid"]) != tid:
                continue
            cpu = running.get(int(t.address))
            if cpu is not None:
                gdb.write("thread %d is running on cpu%d: use 'thread %d' then 'bt'\n" %
                          (tid, cpu, _gdb_thread_of(cpu)))
                return
            frames = _walk_fp(int(t["ctx"]["ra"]), int(t["ctx"]["s"][0]))
            for i, pc in enumerate(frames):
                gdb.write("  #%-2d %016x  %s\n" % (i, pc, _symbolize(pc - (1 if i else 0))))
            return
        gdb.write("no thread %d\n" % tid)


def _trace_dump_bytes():
    """Build an EUTRACE1 dump from kernel memory (same format as trace dump), all CPUs."""
    ncpus = _ncpus()
    nrecs = int(gdb.parse_and_eval("sizeof(trace_bufs[0].recs) / sizeof(trace_bufs[0].recs[0])"))
    rec_size = eutrace.REC.size
    threads = list(_threads())
    out = bytearray(b"EUTRACE1")
    out += struct.pack("<IIQII", 1, ncpus, int(gdb.parse_and_eval("platform.timebase")), rec_size,
                       len(threads))
    for t in threads:
        name = bytes(_read(int(t["name"].address), 16))
        out += struct.pack("<II", int(t["tid"]), int(t["state"])) + name
    for c in range(ncpus):
        buf = gdb.parse_and_eval("trace_bufs[%d]" % c)
        head = int(buf["head"])
        count = min(head, nrecs)
        raw = _read(int(buf["recs"].address), nrecs * rec_size)
        start = (head - count) % nrecs
        recs = (raw[start * rec_size:] + raw[:start * rec_size])[:count * rec_size]
        out += struct.pack("<Q", count) + recs
    return bytes(out)


class EuTrace(EuCommand):
    """eu-trace [N]: decode the last N trace events (default 40) from kernel memory."""

    def __init__(self):
        super().__init__("eu-trace", "decode recent trace events [n]")

    def invoke(self, arg, from_tty):
        n = int(arg) if arg.strip() else 40
        elf = eutrace.Elf(gdb.current_progspace().filename)
        tr = eutrace.Trace(_trace_dump_bytes(), elf=elf)
        tr.records = tr.records[-n:]

        class W:
            def write(self, s):
                gdb.write(s)
        tr.text(W())


class EuTraceDump(EuCommand):
    """eu-trace-dump [PATH.bin]: save the trace and a Perfetto .json next to it."""

    def __init__(self):
        super().__init__("eu-trace-dump", "save trace (.bin + Perfetto .json)")

    def invoke(self, arg, from_tty):
        path = arg.strip() or "build/trace.bin"
        raw = _trace_dump_bytes()
        with open(path, "wb") as f:
            f.write(raw)
        elf = eutrace.Elf(gdb.current_progspace().filename)
        tr = eutrace.Trace(raw, elf=elf)
        out = os.path.splitext(path)[0] + ".json"
        import json
        with open(out, "w") as f:
            json.dump(tr.perfetto(), f)
        gdb.write("%d events -> %s, %s (open in https://ui.perfetto.dev)\n" %
                  (len(tr.records), path, out))


class EuProcs(EuCommand):
    """eu-procs: user processes (pid, parent, state, address space size)."""

    def __init__(self):
        super().__init__("eu-procs", "user processes")

    def invoke(self, arg, from_tty):
        head = _static("all_procs")
        ptype = gdb.lookup_type("struct proc")
        off = [f.bitpos // 8 for f in ptype.fields() if f.name == "all_node"][0]
        node, head_addr = head["next"], int(head.address)
        gdb.write("  %4s %5s %-16s %-7s %8s %8s\n" % ("PID", "PPID", "NAME", "STATE", "SYSCALLS",
                                                    "RSS KiB"))
        while int(node) != head_addr:
            p = gdb.Value(int(node) - off).cast(ptype.pointer()).dereference()
            parent = int(p["parent"]["pid"]) if int(p["parent"]) else 0
            rss = int(p["mm"]["rss"]) * 4 if int(p["mm"]) else 0
            gdb.write("  %4d %5d %-16s %-7s %8d %8d\n" % (
                int(p["pid"]), parent, p["name"].string(),
                "zombie" if int(p["state"]) else "alive", int(p["syscalls"]), rss))
            node = node["next"]
        gdb.write("  user symbols: add-symbol-file build/user/bin/<prog>\n")


class EuBcache(EuCommand):
    """eu-bcache: buffers that are referenced, pinned by the log, or locked."""

    def __init__(self):
        super().__init__("eu-bcache", "referenced/pinned/locked buffers")

    def invoke(self, arg, from_tty):
        bufs = _static("bufs")
        n = bufs.type.range()[1] + 1
        shown = 0
        for i in range(n):
            b = bufs[i]
            owner = b["lock"]["owner"]
            if not int(b["refcnt"]) and not int(b["pins"]) and not int(owner):
                continue
            shown += 1
            site = int(b["get_site"])
            gdb.write("  block %6d refcnt %d pins %d %s  taken by %s\n" % (
                int(b["blockno"]), int(b["refcnt"]), int(b["pins"]),
                ("locked by " + owner["name"].string()) if int(owner) else "unlocked",
                _symbolize(site) if site else "?"))
        gdb.write("  %d of %d buffers busy\n" % (shown, n))


class EuFiles(EuCommand):
    """eu-files PID: open file descriptors of a process."""

    def __init__(self):
        super().__init__("eu-files", "open files of a process")

    def invoke(self, arg, from_tty):
        pid = int(arg)
        head = _static("all_procs")
        ptype = gdb.lookup_type("struct proc")
        off = [f.bitpos // 8 for f in ptype.fields() if f.name == "all_node"][0]
        node, head_addr = head["next"], int(head.address)
        while int(node) != head_addr:
            p = gdb.Value(int(node) - off).cast(ptype.pointer()).dereference()
            node = node["next"]
            if int(p["pid"]) != pid:
                continue
            for fd in range(32):
                f = p["files"][fd]
                if int(f):
                    gdb.write("  %2d off %-8d refs %d ino %d  %s\n" % (
                        fd, int(f["off"]), int(f["refcnt"]), int(f["ip"]["ino"]),
                        f["path"].string()))
            return
        gdb.write("no process %d\n" % pid)


class EuProfileDump(EuCommand):
    """eu-profile-dump [PATH]: write the profiler's samples (build/profile.bin by default)
    straight from kernel memory, even from a hung machine; then `make flamegraph`."""

    def __init__(self):
        super().__init__("eu-profile-dump", "profiler samples -> build/profile.bin")

    def invoke(self, arg, from_tty):
        path = arg.strip() or "build/profile.bin"
        ncpus = _ncpus()
        prof = _static("prof")
        rec_hdr = int(gdb.parse_and_eval("sizeof(struct prof_rec)"))
        dropped = sum(int(prof[c]["dropped"]) for c in range(ncpus))
        out = bytearray(struct.pack("<8sIIQIIQ", b"EUPROF1\0", 1, ncpus,
                                    int(gdb.parse_and_eval("platform.timebase")),
                                    int(_static("profile_hz")), rec_hdr, dropped))
        total = 0
        for c in range(ncpus):
            used = int(prof[c]["used"]) if int(prof[c]["buf"]) else 0
            out += struct.pack("<Q", used)
            if used:
                out += _read(int(prof[c]["buf"]), used)
            total += int(prof[c]["samples"])
        with open(path, "wb") as f:
            f.write(out)
        gdb.write("%d samples (%d bytes) -> %s; now: make flamegraph\n" % (total, len(out), path))


TCP_STATES = ["CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
              "FIN_WAIT_2", "CLOSING", "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK"]


def _ip(v):
    v = int(v)
    return "%d.%d.%d.%d" % (v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)


def _list_entries(head_name, type_name, field):
    head = _static(head_name)
    t = gdb.lookup_type(type_name)
    off = [f.bitpos // 8 for f in t.fields() if f.name == field][0]
    node, head_addr = head["next"], int(head.address)
    while int(node) != head_addr:
        yield gdb.Value(int(node) - off).cast(t.pointer()).dereference()
        node = node["next"]


class EuNet(EuCommand):
    """eu-net: interfaces, TCP connections and sockets (from memory; works when wedged)."""

    def __init__(self):
        super().__init__("eu-net", "interfaces, TCP connections, sockets")

    def invoke(self, arg, from_tty):
        n = int(_static("nnetifs"))
        for i in range(n):
            d = _static("netifs")[i].dereference()
            st = d["stats"]
            gdb.write("  %-5s %-15s rx %d pkts (%d dropped)  tx %d pkts (%d dropped)\n" % (
                d["name"].string(), _ip(d["addr"]), int(st["rx_packets"]), int(st["rx_dropped"]),
                int(st["tx_packets"]), int(st["tx_dropped"])))
        owner = _static("net_lock")["owner"]
        if int(owner):
            gdb.write("  net_lock held by thread %d (%s)\n" % (int(owner["tid"]),
                                                             owner["name"].string()))
        gdb.write("  TCP:\n")
        for t in _list_entries("tcbs", "struct tcb", "node"):
            gdb.write("    %3d %s:%d -> %s:%d  %-11s sndq %d rcvq %d snd_wnd %d rto %d "
                      "retransmits %d%s\n" % (
                          int(t["id"]), _ip(t["laddr"]), int(t["lport"]), _ip(t["raddr"]),
                          int(t["rport"]), TCP_STATES[int(t["state"])], int(t["snd_len"]),
                          int(t["rcv_len"]), int(t["snd_wnd"]), int(t["rto"]),
                          int(t["retransmits"]), "" if int(t["owner"]) else "  (orphan)"))
        gdb.write("  sockets:\n")
        types = {1: "tcp", 2: "udp", 3: "raw"}
        for s in _list_entries("all_sockets", "struct socket", "node"):
            gdb.write("    #%d %s %s:%d pid %d%s\n" % (
                int(s["id"]), types.get(int(s["type"]), "?"), _ip(s["laddr"]), int(s["lport"]),
                int(s["owner_pid"]), "  queued %d" % int(s["rxq_len"]) if int(s["rxq_len"]) else ""))


for cls in (EuHelp, EuDmesg, EuLog, EuTrapFrame, EuPlatform, EuVm, EuMeminfo, EuPage, EuSlab,
            EuPs, EuCpus, EuLockdep, EuBt, EuTrace, EuTraceDump, EuProcs, EuBcache, EuFiles,
            EuNet, EuProfileDump):
    cls()

# Stop in the debugger whenever the kernel panics or enters its monitor.
gdb.execute("break panic", to_string=True)
gdb.execute("break panic_trap", to_string=True)
gdb.write("Eunuchs GDB helpers loaded (eu-help). Breakpoints set on panic, panic_trap.\n")
