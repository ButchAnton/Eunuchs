#!/usr/bin/env python3
"""euprof.py: turn a Eunuchs profile dump into reports and flamegraphs.

    euprof.py report   PROFILE.bin              top functions (kernel and user)
    euprof.py folded   PROFILE.bin OUT.folded   one line per stack ("a;b;c count")
    euprof.py svg      PROFILE.bin OUT.svg      interactive flamegraph (open in a browser)

Options: --kernel ELF (default build/kernel.elf), --user-dir DIR (default
build/user/bin: unstripped programs, found by process name; eutils applets
fall back to eutils), --no-idle, --cpu N, --pid N, --kernel-only.

Dumps come from `profile dump` (monitor or shell, via semihosting) or
`eu-profile-dump` in GDB. Stacks are rooted at the process (or kernel
thread) name, then user frames, then "[kernel]" and the kernel frames.
"""
import argparse
import html
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "trace"))
from eutrace import Elf, ROOT  # noqa: E402

REC = struct.Struct("<QIIHBBB3x16s")        # struct prof_rec
HDR = struct.Struct("<8sIIQIIQ")            # struct prof_file_hdr
IDLE, USER, IN_IRQ = 1, 2, 4


class Profile:
    def __init__(self, raw):
        magic, version, ncpus, timebase, hz, rec_hdr, dropped = HDR.unpack_from(raw, 0)
        if magic != b"EUPROF1\0":
            raise ValueError("not a Eunuchs profile dump")
        if rec_hdr != REC.size:
            raise ValueError("record header is %d bytes, expected %d" % (rec_hdr, REC.size))
        self.ncpus, self.timebase, self.hz, self.dropped = ncpus, timebase, hz, dropped
        self.samples = []
        off = HDR.size
        for _ in range(ncpus):
            n, = struct.unpack_from("<Q", raw, off)
            off += 8
            end = off + n
            while off < end:
                ts, tid, pid, cpu, nk, nu, flags, comm = REC.unpack_from(raw, off)
                off += REC.size
                pcs = struct.unpack_from("<%dQ" % (nk + nu), raw, off)
                off += 8 * (nk + nu)
                self.samples.append(dict(ts=ts, tid=tid, pid=pid, cpu=cpu, flags=flags,
                                         comm=comm.split(b"\0")[0].decode("utf-8", "replace"),
                                         kernel=pcs[:nk], user=pcs[nk:]))
        self.samples.sort(key=lambda s: s["ts"])


class Symbolizer:
    def __init__(self, kernel, user_dir):
        self.kernel = Elf(kernel)
        self.user_dir = user_dir
        self.user = {}

    def _user_elf(self, comm):
        if comm not in self.user:
            elf = None
            for name in (comm, "eutils"):
                path = os.path.join(self.user_dir, name)
                if os.path.exists(path):
                    try:
                        elf = Elf(path)
                        break
                    except ValueError:
                        pass
            self.user[comm] = elf
        return self.user[comm]

    @staticmethod
    def _name(elf, pc):
        s = elf.symbolize(pc)
        return s.split("+")[0] if not s.startswith("0x") else s

    def stack(self, s, kernel_only=False):
        """Root-first list of frame names."""
        frames = [s["comm"] or "?"]
        if s["user"] and not kernel_only:
            elf = self._user_elf(s["comm"])
            user = []
            for i, pc in enumerate(s["user"]):
                user.append(self._name(elf, pc - (1 if i else 0)) if elf else "%#x" % pc)
            frames += reversed(user)
        if s["kernel"]:
            frames.append("[kernel]")
            frames += reversed([self._name(self.kernel, pc - (1 if i else 0))
                                for i, pc in enumerate(s["kernel"])])
        return frames


def select(prof, a):
    out = []
    for s in prof.samples:
        if a.no_idle and s["flags"] & IDLE:
            continue
        if a.cpu is not None and s["cpu"] != a.cpu:
            continue
        if a.pid is not None and s["pid"] != a.pid:
            continue
        out.append(s)
    return out


def folded(prof, sym, a):
    counts = {}
    for s in select(prof, a):
        key = ";".join(sym.stack(s, a.kernel_only))
        counts[key] = counts.get(key, 0) + 1
    return counts


def report(prof, sym, a, out=sys.stdout):
    ss = select(prof, a)
    n = len(ss) or 1
    span = (ss[-1]["ts"] - ss[0]["ts"]) / prof.timebase if ss else 0
    out.write("%d samples at %d Hz on %d CPUs over %.2f s (%d dropped)\n" %
              (len(ss), prof.hz, prof.ncpus, span, prof.dropped))
    # CPU usage from every sample (--no-idle only affects the function tables)
    every = [s for s in prof.samples
             if (a.cpu is None or s["cpu"] == a.cpu) and (a.pid is None or s["pid"] == a.pid)]
    m = len(every) or 1
    idle = sum(1 for s in every if s["flags"] & IDLE)
    user = sum(1 for s in every if s["flags"] & USER)
    out.write("idle %.1f%%  user %.1f%%  kernel %.1f%%\n\n" %
              (100 * idle / m, 100 * user / m, 100 * (m - idle - user) / m))
    per_cpu = {}
    for s in every:
        per_cpu.setdefault(s["cpu"], [0, 0])
        per_cpu[s["cpu"]][0] += 1
        per_cpu[s["cpu"]][1] += bool(s["flags"] & IDLE)
    out.write("per CPU:  " + "  ".join("cpu%d %.0f%% busy" % (c, 100 * (t - i) / t)
                                         for c, (t, i) in sorted(per_cpu.items())) + "\n\n")
    self_c, total_c, comm_c = {}, {}, {}
    for s in ss:
        frames = sym.stack(s)
        comm_c[frames[0]] = comm_c.get(frames[0], 0) + 1
        leaf = frames[-1] if len(frames) > 1 else "?"
        self_c[leaf] = self_c.get(leaf, 0) + 1
        for f in set(frames[1:]):
            total_c[f] = total_c.get(f, 0) + 1
    out.write("by process / kernel thread:\n")
    for c, k in sorted(comm_c.items(), key=lambda x: -x[1])[:15]:
        out.write("  %6.1f%%  %s\n" % (100 * k / n, c))
    out.write("\nfunctions (kernel and user)           self    total\n")
    for f, k in sorted(self_c.items(), key=lambda x: -x[1])[:40]:
        out.write("  %-34s %6.1f%%  %6.1f%%\n" % (f[:34], 100 * k / n, 100 * total_c.get(f, k) / n))
    out.write("\nNote: samples cannot interrupt code running with interrupts off; that time is\n"
              "charged to where interrupts come back on (spin_unlock_irqrestore, intr_on, ...).\n")


# ---- SVG flamegraph ----

def build_tree(counts):
    root = {"name": "all", "value": 0, "children": {}}
    for stack, c in counts.items():
        node = root
        root["value"] += c
        for f in stack.split(";"):
            node = node["children"].setdefault(f, {"name": f, "value": 0, "children": {}})
            node["value"] += c
    return root


def color(name, depth):
    h = sum(ord(ch) for ch in name) % 40
    if name.startswith("[kernel]") or name in ("all",):
        return "rgb(200,200,200)"
    if depth == 1:
        return "rgb(160,160,220)"                  # process
    return "rgb(%d,%d,%d)" % (220, 110 + h * 2, 40 + h)


SVG_JS = """
<script><![CDATA[
var rects = document.querySelectorAll("g.f");
function zoom(g) {
  var x = +g.getAttribute("data-x"), w = +g.getAttribute("data-w"), d = +g.getAttribute("data-y");
  rects.forEach(function (r) {
    var rx = +r.getAttribute("data-x"), rw = +r.getAttribute("data-w"), rd = +r.getAttribute("data-y");
    var below = rd >= d && rx >= x - 1e-9 && rx + rw <= x + w + 1e-9;   /* the frame and its callees */
    var above = rd < d && rx <= x + 1e-9 && rx + rw >= x + w - 1e-9;    /* its callers */
    if (!below && !above) { r.style.display = "none"; return; }
    r.style.display = "";
    var nx = below ? (rx - x) / w : 0, nw = below ? rw / w : 1;
    var rect = r.querySelector("rect"), text = r.querySelector("text");
    rect.setAttribute("x", (nx * W).toFixed(2));
    rect.setAttribute("width", Math.max(nw * W - 0.5, 0.1).toFixed(2));
    text.setAttribute("x", (nx * W + 3).toFixed(2));
    var label = r.getAttribute("data-n"), chars = Math.floor(nw * W / 7);
    text.textContent = chars > 3 ? (label.length > chars ? label.substr(0, chars - 2) + ".." : label) : "";
  });
}
var W = %WIDTH%;
rects.forEach(function (r) { r.addEventListener("click", function () { zoom(r); }); });
document.getElementById("reset").addEventListener("click", function () { zoom(rects[0]); });
]]></script>
"""


def svg(counts, title, out):
    root = build_tree(counts)
    width, row = 1200, 17
    depth = [0]

    def measure(node, d):
        depth[0] = max(depth[0], d)
        for c in node["children"].values():
            measure(c, d + 1)
    measure(root, 0)
    height = (depth[0] + 1) * row + 60
    total = root["value"] or 1
    parts = []

    def emit(node, x, d):
        w = node["value"] / total
        if w * width < 0.3:
            return
        y = height - 30 - (d + 1) * row
        label = node["name"]
        chars = int(w * width / 7)
        text = label if len(label) <= chars else (label[:chars - 2] + ".." if chars > 3 else "")
        pct = 100.0 * node["value"] / total
        parts.append('<g class="f" data-x="%.6f" data-w="%.6f" data-y="%d" data-n="%s">'
                     '<title>%s (%d samples, %.2f%%)</title>'
                     '<rect x="%.2f" y="%d" width="%.2f" height="%d" fill="%s" rx="2"/>'
                     '<text x="%.2f" y="%d">%s</text></g>' % (
                         x, w, d, html.escape(label, True), html.escape(label), node["value"], pct,
                         x * width, y, max(w * width - 0.5, 0.1), row - 1, color(label, d),
                         x * width + 3, y + row - 5, html.escape(text)))
        cx = x
        for c in sorted(node["children"].values(), key=lambda c: c["name"]):
            emit(c, cx, d + 1)
            cx += c["value"] / total
    emit(root, 0.0, 0)
    out.write('<?xml version="1.0" standalone="no"?>\n'
              '<svg version="1.1" width="%d" height="%d" xmlns="http://www.w3.org/2000/svg" '
              'font-family="Menlo, monospace" font-size="11">\n' % (width, height))
    out.write('<rect width="100%%" height="100%%" fill="#fdfdf5"/>\n'
              '<text x="10" y="20" font-size="15">%s</text>\n'
              '<text id="reset" x="%d" y="20" fill="blue" style="cursor:pointer">reset zoom</text>\n'
              '<text x="10" y="40" fill="#666">click a frame to zoom; hover for counts</text>\n' %
              (html.escape(title), width - 90))
    out.write('<style>g.f text{pointer-events:none} g.f:hover rect{stroke:black;stroke-width:0.5}'
              '</style>\n')
    out.write("\n".join(parts))
    out.write(SVG_JS.replace("%WIDTH%", str(width)))
    out.write("</svg>\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["report", "folded", "svg"])
    ap.add_argument("profile")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "kernel.elf"))
    ap.add_argument("--user-dir", default=os.path.join(ROOT, "build", "user", "bin"))
    ap.add_argument("--no-idle", action="store_true")
    ap.add_argument("--kernel-only", action="store_true")
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--pid", type=int)
    a = ap.parse_args()
    with open(a.profile, "rb") as f:
        prof = Profile(f.read())
    sym = Symbolizer(a.kernel, a.user_dir)
    if a.cmd == "report":
        report(prof, sym, a)
        return
    if not a.out:
        ap.error("%s needs an output file" % a.cmd)
    counts = folded(prof, sym, a)
    with open(a.out, "w") as out:
        if a.cmd == "folded":
            for k, v in sorted(counts.items()):
                out.write("%s %d\n" % (k, v))
        else:
            svg(counts, "Eunuchs profile: %d samples at %d Hz on %d CPUs" %
                (sum(counts.values()), prof.hz, prof.ncpus), out)
    print("%d stacks -> %s" % (len(counts), a.out))


if __name__ == "__main__":
    main()
