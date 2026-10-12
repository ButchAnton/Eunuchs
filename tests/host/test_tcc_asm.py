#!/usr/bin/env python3
"""tcc's RISC-V assembler against GNU as.

Usage: test_tcc_asm.py XTCC TCCDIR CROSS-PREFIX

Each input is assembled by the host cross tcc (build/tcc/xtcc, the same
source as the guest's /bin/tcc) and by GNU as (rv64g, so that it does not
compress, and without relaxation); both objects are linked by GNU ld at the
same address, and the loaded bytes must be identical. The inputs:

  - our own assembly files (the kernel's and libc's .S);
  - a file of directives and pseudo-instructions (macros, .irp, .rept,
    .option, .section types, la/lla/call/tail, loads and stores of symbols);
  - every instruction form tcc knows, base and compressed, with random
    registers and immediates (fixed seeds, so a failure is reproducible).
"""
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

KFLAGS = ["-D__ASSEMBLY__", "-Ikernel/include", "-Ilib/include"]
UFLAGS = ["-D__ASSEMBLY__", "-Iuser/include"]
SOURCES = [("kernel/arch/riscv/" + n + ".S", KFLAGS)
           for n in ("boot", "fp", "safe", "switch", "trapentry", "uaccess")] + \
          [("user/lib/" + n + ".S", UFLAGS) for n in ("setjmp", "syscall")] + \
          [("user/lib/crt0.S", UFLAGS + ["-DNO_GP"])]

DIRECTIVES = r"""
#define FRAME 32
    .equ    SIZE, 64
    .set    OFF, 24
.macro SAVE reg, off=0
    sd      \reg, \off(sp)
.endm
.macro PAIR name, load, store
    .globl  \name
\name:
1:  \load   t0, 0(a0)
    \store  t0, 0(a1)
    bnez    t0, 1b
    j       2f
2:  ret
    .pushsection __ex_table, "a"
    .balign 8
    .dword  1b, 2b
    .popsection
.endm
.macro OUTER x
    .irp    r, a0, a1
    SAVE    \r, \x
    .endr
.endm
    .text
    .globl  start
start:
    .cfi_startproc
    SAVE    ra
    SAVE    s0, FRAME - 8
    PAIR    copy8, lbu, sb
    PAIR    copy64, ld, sd
    .irp    n, 1, 3, 4
    sd      x\n, \n*8(sp)
    .cfi_offset \n, \n*8 - FRAME
    .endr
    OUTER   16
    .rept   2
    nop
    .endr
    .cfi_endproc
1:  beqz    a0, 2f
    bnez    a1, 1b
    blt     a0, a1, start
    bgeu    a2, a3, later
    j       ext_func
    jal     ext_func
    jal     t0, start
    call    ext_func
    call    ext_func + 8
    tail    ext_func
    jump    later, t1
2:  ld      a0, var
    lw      a1, var + 4
    sd      a0, var, t2
    fld     fa0, var, t3
    fsd     fa1, var, t4
    lla     a0, var + OFF
    la      a1, ext_data
    .option push
    .option pic
    la      a2, ext_data
    .option pop
    la      a3, ext_data
    addi    a0, a0, SIZE
    mv      fp, sp
    csrs    sstatus, t1
    csrr    t0, satp
    fscsr   t1
    sfence.vma
    .option push
    .option arch, +d
    fsd     fs0, 0(a0)
    .option pop
later:
    ret
    .section .text.more
    sret
    .section .stacks, "aw", @nobits
    .space  4096
    .data
var:
    .dword  0x1122334455667788, start, later + 4
    .word   7
    .byte   1
    .half   2
    .short  3
    .2byte  4
    .4byte  5
    .8byte  6
    .long   7
"""


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


class Checker:
    def __init__(self, xtcc, tccdir, cross, tmp):
        self.xtcc, self.tccdir, self.cross, self.tmp = xtcc, tccdir, cross, tmp
        self.failures = 0

    def image(self, obj, defsyms):
        # unwind tables differ (gas makes them from .cfi_*, tcc ignores
        # those) and would move the sections after them
        elf = obj + ".elf"
        run([self.cross + "objcopy", "-R", ".eh_frame", "-R", ".rela.eh_frame", obj])
        r = run([self.cross + "ld", "--no-relax", "-Ttext=0x80000000", "-e", "0"] +
                ["--defsym=%s=0x80010000" % s for s in defsyms] + [obj, "-o", elf])
        if r.returncode:
            raise RuntimeError(r.stderr.strip())
        run([self.cross + "objcopy", "-O", "binary", elf, elf + ".bin"])
        with open(elf + ".bin", "rb") as f:
            return f.read()

    def check(self, name, src, flags, march="rv64g", compare_text_only=False):
        base = os.path.join(self.tmp, name.replace("/", "_"))
        t = run([self.xtcc, "-B", self.tccdir] + flags + ["-c", src, "-o", base + ".t.o"], cwd=ROOT)
        g = run([self.cross + "gcc", "-march=" + march, "-mabi=lp64d", "-mno-relax"] + flags +
                ["-c", src, "-o", base + ".g.o"], cwd=ROOT)
        if g.returncode:
            print("FAIL %s: gas: %s" % (name, g.stderr.strip()))
            self.failures += 1
            return
        if t.returncode:
            print("FAIL %s: tcc: %s" % (name, t.stderr.strip()))
            self.failures += 1
            return
        if compare_text_only:
            imgs = []
            for o in (base + ".g.o", base + ".t.o"):
                run([self.cross + "objcopy", "-O", "binary", "-j", ".text", o, o + ".bin"])
                with open(o + ".bin", "rb") as f:
                    imgs.append(f.read())
        else:
            undef = run([self.cross + "nm", "-u", base + ".g.o"]).stdout.split()
            undef = [s for s in undef if s != "U"]
            try:
                imgs = [self.image(base + ".g.o", undef), self.image(base + ".t.o", undef)]
            except RuntimeError as e:
                print("FAIL %s: link: %s" % (name, e))
                self.failures += 1
                return
        a, b = imgs
        n = min(len(a), len(b))
        # tcc may pad the last section to its alignment
        if a[:n] == b[:n] and not a[n:].strip(b"\0") and not b[n:].strip(b"\0"):
            print("ok   %s (%d bytes)" % (name, len(a)))
            return
        off = next((i for i in range(n) if a[i] != b[i]), n)
        print("FAIL %s: first difference at offset %#x: gas %s, tcc %s" %
              (name, off, a[off:off + 4].hex(), b[off:off + 4].hex()))
        self.failures += 1


def forms(rng):
    """(base forms, compressed forms): functions returning one instruction."""
    X = lambda: "x%d" % rng.randrange(32)
    XN = lambda: "x%d" % rng.randrange(1, 32)
    F = lambda: "f%d" % rng.randrange(32)
    C3 = lambda: rng.choice("s0 s1 a0 a1 a2 a3 a4 a5".split())
    F3 = lambda: "f%d" % rng.randrange(8, 16)
    imm = lambda lo, hi, al=1: rng.randrange(lo // al, hi // al + 1) * al
    RM = lambda: rng.choice(["", ", rne", ", rtz", ", rdn", ", rup", ", rmm", ", dyn"])
    AQ = lambda: rng.choice(["", ".aq", ".rl", ".aqrl"])
    b = []
    for m in "lb lh lw lbu lhu ld lwu sb sh sw sd".split():
        b.append(lambda m=m: "%s %s, %d(%s)" % (m, X(), imm(-2048, 2047), X()))
    for m in "flw fld fsw fsd".split():
        b.append(lambda m=m: "%s %s, %d(%s)" % (m, F(), imm(-2048, 2047), X()))
    for m in ("sll srl sra sllw srlw sraw add sub addw subw xor or and slt sltu mul mulh "
              "mulhsu mulhu div divu rem remu mulw divw divuw remw remuw").split():
        b.append(lambda m=m: "%s %s, %s, %s" % (m, X(), X(), X()))
    for m in "slli srli srai".split():
        b.append(lambda m=m: "%s %s, %s, %d" % (m, X(), X(), imm(0, 63)))
    for m in "slliw srliw sraiw".split():
        b.append(lambda m=m: "%s %s, %s, %d" % (m, X(), X(), imm(0, 31)))
    for m in "addi xori ori andi slti sltiu addiw".split():
        b.append(lambda m=m: "%s %s, %s, %d" % (m, X(), X(), imm(-2048, 2047)))
    b.append(lambda: "lui %s, %d" % (X(), imm(0, 0xfffff)))
    b.append(lambda: "auipc %s, %d" % (X(), imm(0, 0xfffff)))
    for m in "beq bne blt bge bltu bgeu bgt ble bgtu bleu".split():
        b.append(lambda m=m: "%s %s, %s, .%+d" % (m, X(), X(), imm(-4096, 4094, 2)))
    for m in "beqz bnez bgez bltz blez bgtz".split():
        b.append(lambda m=m: "%s %s, .%+d" % (m, X(), imm(-4096, 4094, 2)))
    b.append(lambda: "jal %s, .%+d" % (X(), imm(-1048576, 1048574, 2)))
    b.append(lambda: "j .%+d" % imm(-1048576, 1048574, 2))
    b.append(lambda: "jalr %s, %d(%s)" % (X(), imm(-2048, 2047), X()))
    b.append(lambda: "jr %d(%s)" % (imm(-2048, 2047), X()))
    b.append(lambda: "fence %s, %s" % (rng.choice(["r", "w", "rw", "i", "o", "io", "iorw", "ow"]),
                                       rng.choice(["r", "w", "rw", "iorw", "o"])))
    for t in "sd":
        for m in "fadd fsub fmul fdiv".split():
            b.append(lambda m=m, t=t: "%s.%s %s, %s, %s%s" % (m, t, F(), F(), F(), RM()))
        for m in "fsgnj fsgnjn fsgnjx fmin fmax".split():
            b.append(lambda m=m, t=t: "%s.%s %s, %s, %s" % (m, t, F(), F(), F()))
        for m in "fmadd fmsub fnmadd fnmsub".split():
            b.append(lambda m=m, t=t: "%s.%s %s, %s, %s, %s%s" % (m, t, F(), F(), F(), F(), RM()))
        b.append(lambda t=t: "fsqrt.%s %s, %s%s" % (t, F(), F(), RM()))
        for m in "feq flt fle".split():
            b.append(lambda m=m, t=t: "%s.%s %s, %s, %s" % (m, t, X(), F(), F()))
        b.append(lambda t=t: "fclass.%s %s, %s" % (t, X(), F()))
        for i in "w wu l lu".split():
            b.append(lambda i=i, t=t: "fcvt.%s.%s %s, %s%s" % (i, t, X(), F(), RM()))
            exact = t == "d" and i in ("w", "wu")
            b.append(lambda i=i, t=t, exact=exact: "fcvt.%s.%s %s, %s%s" %
                     (t, i, F(), X(), "" if exact else RM()))
    b.append(lambda: "fcvt.s.d %s, %s%s" % (F(), F(), RM()))
    b.append(lambda: "fcvt.d.s %s, %s" % (F(), F()))
    for m, d, s in (("fmv.x.w", X, F), ("fmv.w.x", F, X), ("fmv.x.d", X, F), ("fmv.d.x", F, X)):
        b.append(lambda m=m, d=d, s=s: "%s %s, %s" % (m, d(), s()))
    for m in "csrrw csrrs csrrc".split():
        b.append(lambda m=m: "%s %s, %s, %s" % (m, X(), rng.choice(["sstatus", "sepc", "satp", "0x7c0",
                                                                    str(imm(0, 4095))]), X()))
    for m in "csrrwi csrrsi csrrci".split():
        b.append(lambda m=m: "%s %s, %d, %d" % (m, X(), imm(0, 4095), imm(0, 31)))
    b.append(lambda: "li %s, %#x" % (X(), rng.getrandbits(64)))
    b.append(lambda: "li %s, %d" % (X(), imm(-5000, 5000)))
    for m in "amoadd amoswap amoxor amoor amoand amomin amomax amominu amomaxu sc".split():
        b.append(lambda m=m: "%s.%s%s %s, %s, (%s)" % (m, rng.choice("wd"), AQ(), X(), X(), X()))
    b.append(lambda: "lr.%s%s %s, (%s)" % (rng.choice("wd"), AQ(), X(), X()))
    b.append(lambda: "sfence.vma %s, %s" % (X(), X()))
    for m in "nop ret ecall ebreak wfi sret mret unimp fence.i fence.tso".split():
        b.append(lambda m=m: m)

    c = []
    c.append(lambda: "c.addi4spn %s, sp, %d" % (C3(), imm(4, 1020, 4)))
    for m in "c.lw c.sw".split():
        c.append(lambda m=m: "%s %s, %d(%s)" % (m, C3(), imm(0, 124, 4), C3()))
    for m in "c.ld c.sd".split():
        c.append(lambda m=m: "%s %s, %d(%s)" % (m, C3(), imm(0, 248, 8), C3()))
    for m in "c.fld c.fsd".split():
        c.append(lambda m=m: "%s %s, %d(%s)" % (m, F3(), imm(0, 248, 8), C3()))
    c.append(lambda: "c.addi %s, %d" % (XN(), rng.choice([imm(-32, -1), imm(1, 31)])))
    c.append(lambda: "c.addiw %s, %d" % (XN(), imm(-32, 31)))
    c.append(lambda: "c.li %s, %d" % (XN(), imm(-32, 31)))
    c.append(lambda: "c.addi16sp sp, %d" % rng.choice([imm(-512, -16, 16), imm(16, 496, 16)]))
    c.append(lambda: "c.lui x%d, %d" % (rng.choice([1] + list(range(3, 32))),
                                        rng.choice([imm(1, 31), imm(0xfffe0, 0xfffff)])))
    for m in "c.srli c.srai".split():
        c.append(lambda m=m: "%s %s, %d" % (m, C3(), imm(1, 63)))
    c.append(lambda: "c.andi %s, %d" % (C3(), imm(-32, 31)))
    for m in "c.sub c.xor c.or c.and c.subw c.addw".split():
        c.append(lambda m=m: "%s %s, %s" % (m, C3(), C3()))
    c.append(lambda: "c.j .%+d" % imm(-2048, 2046, 2))
    for m in "c.beqz c.bnez".split():
        c.append(lambda m=m: "%s %s, .%+d" % (m, C3(), imm(-256, 254, 2)))
    c.append(lambda: "c.slli %s, %d" % (XN(), imm(1, 63)))
    c.append(lambda: "c.lwsp %s, %d(sp)" % (XN(), imm(0, 252, 4)))
    c.append(lambda: "c.ldsp %s, %d(sp)" % (XN(), imm(0, 504, 8)))
    c.append(lambda: "c.fldsp %s, %d(sp)" % (F(), imm(0, 504, 8)))
    c.append(lambda: "c.swsp %s, %d(sp)" % (X(), imm(0, 252, 4)))
    c.append(lambda: "c.sdsp %s, %d(sp)" % (X(), imm(0, 504, 8)))
    c.append(lambda: "c.fsdsp %s, %d(sp)" % (F(), imm(0, 504, 8)))
    for m in "c.jr c.jalr".split():
        c.append(lambda m=m: "%s %s" % (m, XN()))
    for m in "c.mv c.add".split():
        c.append(lambda m=m: "%s %s, %s" % (m, XN(), XN()))
    for m in "c.nop c.ebreak".split():
        c.append(lambda m=m: m)
    return b, c


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    xtcc, tccdir, cross = (os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3])
    with tempfile.TemporaryDirectory() as tmp:
        c = Checker(xtcc, tccdir, cross, tmp)
        for src, flags in SOURCES:
            c.check(src, src, flags)
        path = os.path.join(tmp, "directives.S")
        with open(path, "w") as f:
            f.write(DIRECTIVES)
        c.check("directives and pseudo-instructions", path, [])
        for seed in (1, 2):
            rng = random.Random(seed)
            base, comp = forms(rng)
            for name, fs, march, n in (("base", base, "rv64g", 12), ("compressed", comp, "rv64gc", 25)):
                lines = [f() for f in fs for _ in range(n)]
                rng.shuffle(lines)
                path = os.path.join(tmp, "%s%d.S" % (name, seed))
                with open(path, "w") as f:
                    f.write("\t.text\n" + "".join("\t%s\n" % l for l in lines))
                c.check("%d random %s instructions (seed %d)" % (len(lines), name, seed), path, [],
                        march=march, compare_text_only=True)
        if c.failures:
            sys.exit("%d check(s) failed" % c.failures)


if __name__ == "__main__":
    main()
