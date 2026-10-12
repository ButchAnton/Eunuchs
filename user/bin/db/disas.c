/*
 * db: a disassembler for RV64GC (I M A F D Zicsr Zifencei and C), printing
 * like objdump, with the usual pseudo-instructions (li mv ret j nop ...).
 */
#include "db.h"

#include <stdio.h>
#include <string.h>

static const char *const xr[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};
static const char *const fr[32] = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1", "fa2", "fa3",
    "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7", "fs8", "fs9", "fs10",
    "fs11", "ft8", "ft9", "ft10", "ft11",
};

static i64 sx(u64 v, int bits)
{
    u64 m = 1ULL << (bits - 1);
    return (i64)((v ^ m) - m);
}

static void target(char *out, size_t size, const char *mn, const char *pre, u64 t)
{
    char d[96];
    snprintf(out, size, "%s\t%s0x%llx <%s>", mn, pre, (unsigned long long)t, frame_desc(t, d, sizeof(d)));
}

static const char *csr_name(u32 csr)
{
    switch (csr) {
    case 0x001: return "fflags";
    case 0x002: return "frm";
    case 0x003: return "fcsr";
    case 0xc00: return "cycle";
    case 0xc01: return "time";
    case 0xc02: return "instret";
    }
    return 0;
}

static int compressed(u64 pc, u16 c, char *o, size_t n)
{
    int op = c & 3, f3 = c >> 13;
    int rd = (c >> 7) & 31, rs2 = (c >> 2) & 31;
    int rdp = 8 + ((c >> 2) & 7), rs1p = 8 + ((c >> 7) & 7);
    if (c == 0) {
        snprintf(o, n, "c.unimp");
        return 2;
    }
    if (op == 0) {
        switch (f3) {
        case 0: {
            u32 imm = ((c >> 7) & 0xf) << 6 | ((c >> 11) & 3) << 4 | ((c >> 5) & 1) << 3 | ((c >> 6) & 1) << 2;
            snprintf(o, n, "addi\t%s,sp,%u", xr[rdp], imm);
            return 2;
        }
        case 1: case 3: case 5: case 7: {
            u32 imm = ((c >> 10) & 7) << 3 | ((c >> 5) & 3) << 6;
            const char *mn = f3 == 1 ? "fld" : f3 == 3 ? "ld" : f3 == 5 ? "fsd" : "sd";
            snprintf(o, n, "%s\t%s,%u(%s)", mn, f3 == 1 || f3 == 5 ? fr[rdp] : xr[rdp], imm, xr[rs1p]);
            return 2;
        }
        case 2: case 6: {
            u32 imm = ((c >> 10) & 7) << 3 | ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 6;
            snprintf(o, n, "%s\t%s,%u(%s)", f3 == 2 ? "lw" : "sw", xr[rdp], imm, xr[rs1p]);
            return 2;
        }
        }
    } else if (op == 1) {
        i64 imm6 = sx(((c >> 12) & 1) << 5 | ((c >> 2) & 31), 6);
        switch (f3) {
        case 0:
            if (rd == 0)
                snprintf(o, n, "nop");
            else
                snprintf(o, n, "addi\t%s,%s,%lld", xr[rd], xr[rd], (long long)imm6);
            return 2;
        case 1:
            snprintf(o, n, "addiw\t%s,%s,%lld", xr[rd], xr[rd], (long long)imm6);
            return 2;
        case 2:
            snprintf(o, n, "li\t%s,%lld", xr[rd], (long long)imm6);
            return 2;
        case 3:
            if (rd == 2) {
                i64 imm = sx(((c >> 12) & 1) << 9 | ((c >> 3) & 3) << 7 | ((c >> 5) & 1) << 6 |
                             ((c >> 2) & 1) << 5 | ((c >> 6) & 1) << 4, 10);
                snprintf(o, n, "addi\tsp,sp,%lld", (long long)imm);
            } else {
                snprintf(o, n, "lui\t%s,0x%llx", xr[rd], (unsigned long long)(imm6 & 0xfffff));
            }
            return 2;
        case 4: {
            int f2 = (c >> 10) & 3;
            u32 sh = ((c >> 12) & 1) << 5 | ((c >> 2) & 31);
            if (f2 == 0)
                snprintf(o, n, "srli\t%s,%s,0x%x", xr[rs1p], xr[rs1p], sh);
            else if (f2 == 1)
                snprintf(o, n, "srai\t%s,%s,0x%x", xr[rs1p], xr[rs1p], sh);
            else if (f2 == 2)
                snprintf(o, n, "andi\t%s,%s,%lld", xr[rs1p], xr[rs1p], (long long)imm6);
            else {
                static const char *const m[8] = { "sub", "xor", "or", "and", "subw", "addw", "?", "?" };
                int k = ((c >> 12) & 1) << 2 | ((c >> 5) & 3);
                snprintf(o, n, "%s\t%s,%s,%s", m[k], xr[rs1p], xr[rs1p], xr[8 + ((c >> 2) & 7)]);
            }
            return 2;
        }
        case 5: {
            u64 imm = ((c >> 12) & 1) << 11 | ((c >> 11) & 1) << 4 | ((c >> 9) & 3) << 8 |
                      ((c >> 8) & 1) << 10 | ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 |
                      ((c >> 3) & 7) << 1 | ((c >> 2) & 1) << 5;
            target(o, n, "j", "", pc + sx(imm, 12));
            return 2;
        }
        case 6: case 7: {
            u64 imm = ((c >> 12) & 1) << 8 | ((c >> 10) & 3) << 3 | ((c >> 5) & 3) << 6 |
                      ((c >> 3) & 3) << 1 | ((c >> 2) & 1) << 5;
            char pre[16];
            snprintf(pre, sizeof(pre), "%s,", xr[rs1p]);
            target(o, n, f3 == 6 ? "beqz" : "bnez", pre, pc + sx(imm, 9));
            return 2;
        }
        }
    } else if (op == 2) {
        switch (f3) {
        case 0:
            snprintf(o, n, "slli\t%s,%s,0x%x", xr[rd], xr[rd], ((c >> 12) & 1) << 5 | rs2);
            return 2;
        case 1: case 3: {
            u32 imm = ((c >> 12) & 1) << 5 | ((c >> 5) & 3) << 3 | ((c >> 2) & 7) << 6;
            snprintf(o, n, "%s\t%s,%u(sp)", f3 == 1 ? "fld" : "ld", f3 == 1 ? fr[rd] : xr[rd], imm);
            return 2;
        }
        case 2: {
            u32 imm = ((c >> 12) & 1) << 5 | ((c >> 4) & 7) << 2 | ((c >> 2) & 3) << 6;
            snprintf(o, n, "lw\t%s,%u(sp)", xr[rd], imm);
            return 2;
        }
        case 4:
            if (!((c >> 12) & 1)) {
                if (rs2 == 0)
                    snprintf(o, n, rd == 1 ? "ret" : "jr\t%s", xr[rd]);
                else
                    snprintf(o, n, "mv\t%s,%s", xr[rd], xr[rs2]);
            } else {
                if (rd == 0 && rs2 == 0)
                    snprintf(o, n, "ebreak");
                else if (rs2 == 0)
                    snprintf(o, n, "jalr\t%s", xr[rd]);
                else
                    snprintf(o, n, "add\t%s,%s,%s", xr[rd], xr[rd], xr[rs2]);
            }
            return 2;
        case 5: case 7: {
            u32 imm = ((c >> 10) & 7) << 3 | ((c >> 7) & 7) << 6;
            snprintf(o, n, "%s\t%s,%u(sp)", f3 == 5 ? "fsd" : "sd", f3 == 5 ? fr[rs2] : xr[rs2], imm);
            return 2;
        }
        case 6: {
            u32 imm = ((c >> 9) & 15) << 2 | ((c >> 7) & 3) << 6;
            snprintf(o, n, "sw\t%s,%u(sp)", xr[rs2], imm);
            return 2;
        }
        }
    }
    snprintf(o, n, ".short\t0x%04x", c);
    return 2;
}

static const char *rm_name(int rm)
{
    static const char *const r[8] = { "rne", "rtz", "rdn", "rup", "rmm", "?", "?", "dyn" };
    return r[rm & 7];
}

int disassemble(u64 pc, u32 insn, char *o, size_t n)
{
    if ((insn & 3) != 3)
        return compressed(pc, (u16)insn, o, n);
    int opc = insn & 0x7f, rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31;
    int rs2 = (insn >> 20) & 31, f7 = insn >> 25;
    i64 immi = sx(insn >> 20, 12);
    i64 imms = sx(((insn >> 25) << 5) | ((insn >> 7) & 31), 12);
    switch (opc) {
    case 0x37:
        snprintf(o, n, "lui\t%s,0x%x", xr[rd], insn >> 12);
        return 4;
    case 0x17:
        snprintf(o, n, "auipc\t%s,0x%x", xr[rd], insn >> 12);
        return 4;
    case 0x6f: {
        u64 imm = ((insn >> 31) & 1) << 20 | ((insn >> 21) & 0x3ff) << 1 | ((insn >> 20) & 1) << 11 |
                  ((insn >> 12) & 0xff) << 12;
        u64 t = pc + sx(imm, 21);
        if (rd == 0)
            target(o, n, "j", "", t);
        else if (rd == 1)
            target(o, n, "jal", "", t);
        else {
            char pre[16];
            snprintf(pre, sizeof(pre), "%s,", xr[rd]);
            target(o, n, "jal", pre, t);
        }
        return 4;
    }
    case 0x67:
        if (rd == 0 && rs1 == 1 && immi == 0)
            snprintf(o, n, "ret");
        else if (rd == 0 && immi == 0)
            snprintf(o, n, "jr\t%s", xr[rs1]);
        else if (rd == 1 && immi == 0)
            snprintf(o, n, "jalr\t%s", xr[rs1]);
        else
            snprintf(o, n, "jalr\t%s,%lld(%s)", xr[rd], (long long)immi, xr[rs1]);
        return 4;
    case 0x63: {
        static const char *const m[8] = { "beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu" };
        u64 imm = ((insn >> 31) & 1) << 12 | ((insn >> 25) & 0x3f) << 5 | ((insn >> 8) & 15) << 1 |
                  ((insn >> 7) & 1) << 11;
        char pre[32];
        if (rs2 == 0 && (f3 == 0 || f3 == 1)) {
            snprintf(pre, sizeof(pre), "%s,", xr[rs1]);
            target(o, n, f3 ? "bnez" : "beqz", pre, pc + sx(imm, 13));
        } else {
            snprintf(pre, sizeof(pre), "%s,%s,", xr[rs1], xr[rs2]);
            target(o, n, m[f3], pre, pc + sx(imm, 13));
        }
        return 4;
    }
    case 0x03: {
        static const char *const m[8] = { "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "?" };
        snprintf(o, n, "%s\t%s,%lld(%s)", m[f3], xr[rd], (long long)immi, xr[rs1]);
        return 4;
    }
    case 0x23: {
        static const char *const m[8] = { "sb", "sh", "sw", "sd", "?", "?", "?", "?" };
        snprintf(o, n, "%s\t%s,%lld(%s)", m[f3], xr[rs2], (long long)imms, xr[rs1]);
        return 4;
    }
    case 0x13: {
        if (f3 == 0 && rd == 0 && rs1 == 0 && immi == 0) {
            snprintf(o, n, "nop");
            return 4;
        }
        if (f3 == 0 && rs1 == 0) {
            snprintf(o, n, "li\t%s,%lld", xr[rd], (long long)immi);
            return 4;
        }
        if (f3 == 0 && immi == 0) {
            snprintf(o, n, "mv\t%s,%s", xr[rd], xr[rs1]);
            return 4;
        }
        if (f3 == 1 || f3 == 5) {
            int sh = (insn >> 20) & 63;
            snprintf(o, n, "%s\t%s,%s,0x%x", f3 == 1 ? "slli" : (insn >> 30) & 1 ? "srai" : "srli",
                     xr[rd], xr[rs1], sh);
            return 4;
        }
        static const char *const m[8] = { "addi", "?", "slti", "sltiu", "xori", "?", "ori", "andi" };
        if (f3 == 4 && immi == -1)
            snprintf(o, n, "not\t%s,%s", xr[rd], xr[rs1]);
        else if (f3 == 3 && immi == 1)
            snprintf(o, n, "seqz\t%s,%s", xr[rd], xr[rs1]);
        else
            snprintf(o, n, "%s\t%s,%s,%lld", m[f3], xr[rd], xr[rs1], (long long)immi);
        return 4;
    }
    case 0x1b:
        if (f3 == 0) {
            if (immi == 0)
                snprintf(o, n, "sext.w\t%s,%s", xr[rd], xr[rs1]);
            else
                snprintf(o, n, "addiw\t%s,%s,%lld", xr[rd], xr[rs1], (long long)immi);
        } else {
            snprintf(o, n, "%s\t%s,%s,0x%x", f3 == 1 ? "slliw" : (insn >> 30) & 1 ? "sraiw" : "srliw", xr[rd],
                     xr[rs1], rs2);
        }
        return 4;
    case 0x33:
    case 0x3b: {
        int w = opc == 0x3b;
        const char *mn = "?";
        if (f7 == 1) {
            static const char *const m[8] = { "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu" };
            static const char *const mw[8] = { "mulw", "?", "?", "?", "divw", "divuw", "remw", "remuw" };
            mn = w ? mw[f3] : m[f3];
        } else {
            static const char *const m[8] = { "add", "sll", "slt", "sltu", "xor", "srl", "or", "and" };
            static const char *const mw[8] = { "addw", "sllw", "?", "?", "?", "srlw", "?", "?" };
            mn = w ? mw[f3] : m[f3];
            if (f7 == 0x20)
                mn = f3 == 0 ? (w ? "subw" : "sub") : f3 == 5 ? (w ? "sraw" : "sra") : "?";
        }
        if (!strcmp(mn, "sub") && rs1 == 0)
            snprintf(o, n, "neg\t%s,%s", xr[rd], xr[rs2]);
        else if (!strcmp(mn, "sltu") && rs1 == 0)
            snprintf(o, n, "snez\t%s,%s", xr[rd], xr[rs2]);
        else
            snprintf(o, n, "%s\t%s,%s,%s", mn, xr[rd], xr[rs1], xr[rs2]);
        return 4;
    }
    case 0x0f:
        snprintf(o, n, f3 == 1 ? "fence.i" : "fence");
        return 4;
    case 0x73:
        if (insn == 0x00000073) { snprintf(o, n, "ecall"); return 4; }
        if (insn == 0x00100073) { snprintf(o, n, "ebreak"); return 4; }
        if (insn == 0x10200073) { snprintf(o, n, "sret"); return 4; }
        if (insn == 0x10500073) { snprintf(o, n, "wfi"); return 4; }
        if (f3) {
            static const char *const m[8] = { "?", "csrrw", "csrrs", "csrrc", "?", "csrrwi", "csrrsi", "csrrci" };
            u32 csr = insn >> 20;
            char cn[16];
            const char *nm = csr_name(csr);
            if (!nm) {
                snprintf(cn, sizeof(cn), "0x%x", csr);
                nm = cn;
            }
            if (f3 == 2 && rs1 == 0)
                snprintf(o, n, "csrr\t%s,%s", xr[rd], nm);
            else if (f3 >= 5)
                snprintf(o, n, "%s\t%s,%s,%d", m[f3], xr[rd], nm, rs1);
            else
                snprintf(o, n, "%s\t%s,%s,%s", m[f3], xr[rd], nm, xr[rs1]);
            return 4;
        }
        break;
    case 0x2f: {                                        /* atomics */
        static const char *const m[32] = {
            [0] = "amoadd", [1] = "amoswap", [2] = "lr", [3] = "sc", [4] = "amoxor", [8] = "amoor",
            [12] = "amoand", [16] = "amomin", [20] = "amomax", [24] = "amominu", [28] = "amomaxu",
        };
        int f5 = insn >> 27;
        const char *base = m[f5] ? m[f5] : "amo?";
        const char *sz = f3 == 2 ? ".w" : ".d";
        const char *ord = (insn >> 26) & 1 ? ((insn >> 25) & 1 ? ".aqrl" : ".aq") : (insn >> 25) & 1 ? ".rl" : "";
        if (f5 == 2)
            snprintf(o, n, "%s%s%s\t%s,(%s)", base, sz, ord, xr[rd], xr[rs1]);
        else
            snprintf(o, n, "%s%s%s\t%s,%s,(%s)", base, sz, ord, xr[rd], xr[rs2], xr[rs1]);
        return 4;
    }
    case 0x07:
        snprintf(o, n, "%s\t%s,%lld(%s)", f3 == 3 ? "fld" : "flw", fr[rd], (long long)immi, xr[rs1]);
        return 4;
    case 0x27:
        snprintf(o, n, "%s\t%s,%lld(%s)", f3 == 3 ? "fsd" : "fsw", fr[rs2], (long long)imms, xr[rs1]);
        return 4;
    case 0x43: case 0x47: case 0x4b: case 0x4f: {
        static const char *const m[4] = { "fmadd", "fmsub", "fnmsub", "fnmadd" };
        int rs3 = insn >> 27;
        snprintf(o, n, "%s.%s\t%s,%s,%s,%s", m[(opc >> 2) & 3], (f7 & 3) == 1 ? "d" : "s", fr[rd], fr[rs1],
                 fr[rs2], fr[rs3]);
        return 4;
    }
    case 0x53: {
        const char *t = (f7 & 3) == 1 ? "d" : "s";
        int f5 = f7 >> 2;
        switch (f5) {
        case 0: case 1: case 2: case 3: {
            static const char *const m[4] = { "fadd", "fsub", "fmul", "fdiv" };
            snprintf(o, n, "%s.%s\t%s,%s,%s", m[f5], t, fr[rd], fr[rs1], fr[rs2]);
            return 4;
        }
        case 4: {
            static const char *const m[3] = { "fsgnj", "fsgnjn", "fsgnjx" };
            if (rs1 == rs2)
                snprintf(o, n, "%s.%s\t%s,%s", f3 == 0 ? "fmv" : f3 == 1 ? "fneg" : "fabs", t, fr[rd], fr[rs1]);
            else
                snprintf(o, n, "%s.%s\t%s,%s,%s", f3 < 3 ? m[f3] : "?", t, fr[rd], fr[rs1], fr[rs2]);
            return 4;
        }
        case 5:
            snprintf(o, n, "%s.%s\t%s,%s,%s", f3 ? "fmax" : "fmin", t, fr[rd], fr[rs1], fr[rs2]);
            return 4;
        case 8:
            snprintf(o, n, "fcvt.%s.%s\t%s,%s", t, (f7 & 3) == 1 ? "s" : "d", fr[rd], fr[rs1]);
            return 4;
        case 11:
            snprintf(o, n, "fsqrt.%s\t%s,%s", t, fr[rd], fr[rs1]);
            return 4;
        case 20: {
            static const char *const m[3] = { "fle", "flt", "feq" };
            snprintf(o, n, "%s.%s\t%s,%s,%s", f3 < 3 ? m[f3] : "?", t, xr[rd], fr[rs1], fr[rs2]);
            return 4;
        }
        case 24: {
            static const char *const m[4] = { "w", "wu", "l", "lu" };
            snprintf(o, n, "fcvt.%s.%s\t%s,%s,%s", m[rs2 & 3], t, xr[rd], fr[rs1], rm_name(f3));
            return 4;
        }
        case 26: {
            static const char *const m[4] = { "w", "wu", "l", "lu" };
            snprintf(o, n, "fcvt.%s.%s\t%s,%s", t, m[rs2 & 3], fr[rd], xr[rs1]);
            return 4;
        }
        case 28:
            if (f3 == 0)
                snprintf(o, n, "fmv.x.%s\t%s,%s", (f7 & 3) == 1 ? "d" : "w", xr[rd], fr[rs1]);
            else
                snprintf(o, n, "fclass.%s\t%s,%s", t, xr[rd], fr[rs1]);
            return 4;
        case 30:
            snprintf(o, n, "fmv.%s.x\t%s,%s", (f7 & 3) == 1 ? "d" : "w", fr[rd], xr[rs1]);
            return 4;
        }
        break;
    }
    }
    snprintf(o, n, ".word\t0x%08x", insn);
    return 4;
}
