/*************************************************************/
/*
 *  RISCV64 assembler for TCC
 *
 */

#ifdef TARGET_DEFS_ONLY

#define CONFIG_TCC_ASM
/* 32 general purpose + 32 floating point registers */
#define NB_ASM_REGS 64

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

/*************************************************************/
#else
/*************************************************************/
#define USING_GLOBALS
#include "tcc.h"

enum {
    OPT_REG,
    OPT_IM12S,
    OPT_IM32,
    OPT_SYM,
};
// Registers go from 0 to 31. We use next bit to choose general/float
#define REG_FLOAT_MASK 0x20
#define REG_IS_FLOAT(register_index) ((register_index) & REG_FLOAT_MASK)
#define REG_VALUE(register_index)    ((register_index) & (REG_FLOAT_MASK-1))
#define C_ENCODE_RS1(register_index) (REG_VALUE(register_index) << 7)
#define C_ENCODE_RS2(register_index) (REG_VALUE(register_index) << 2)
#define ENCODE_RD(register_index)  (REG_VALUE(register_index) << 7)
#define ENCODE_RS1(register_index) (REG_VALUE(register_index) << 15)
#define ENCODE_RS2(register_index) (REG_VALUE(register_index) << 20)
#define NTH_BIT(b, n) ((b >> n) & 1)
#define OP_IM12S (1 << OPT_IM12S)
#define OP_IM32 (1 << OPT_IM32)
#define OP_REG (1 << OPT_REG)
/* a reference to a symbol (plus e.v): only pseudo-instructions, jumps and
   branches accept one, and they emit the relocations */
#define OP_SYM (1 << OPT_SYM)

typedef struct Operand {
    uint32_t type;
    union {
        uint8_t reg;
        uint16_t regset;
        ExprValue e;
    };
} Operand;

static const Operand zero = { OP_REG, { 0 }};
static const Operand ra = { OP_REG, { 1 }};
static const Operand zimm = { OP_IM12S };

static void asm_binary_opcode(TCCState* s1, int token);
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str);
ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs, const uint8_t *clobber_regs, int *pout_reg);
static void asm_emit_a(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *rd1, int aq, int rl);
static void asm_emit_b(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *imm);
static void asm_emit_i(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2);
static void asm_emit_j(int token, uint32_t opcode, const Operand *rd, const Operand *rs2);
static void asm_emit_opcode(uint32_t opcode);
static void asm_emit_r(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2);
static void asm_emit_s(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *imm);
static void asm_emit_u(int token, uint32_t opcode, const Operand *rd, const Operand *rs2);
static void asm_emit_f(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2);
static void asm_emit_fb(int token, uint32_t opcode, const Operand *rd, const Operand *rs);
static void asm_emit_fq(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2, const Operand *rs3);
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output, uint8_t *clobber_regs, int out_reg);
static void asm_nullary_opcode(TCCState *s1, int token);
ST_FUNC void asm_opcode(TCCState *s1, int token);
static int asm_parse_csrvar(int t);
static uint32_t asm_csr_num(int token, const Operand *op);
static int asm_fcvt_rm(TCCState *s1);
static void asm_fcsr_write_opcode(TCCState *s1, int token, int csr);
ST_FUNC int asm_parse_regvar(int t);
static void asm_ternary_opcode(TCCState *s1, int token);
static void asm_unary_opcode(TCCState *s1, int token);
static void asm_branch_opcode(TCCState *s1, int token, int argc);
ST_FUNC void gen_expr32(ExprValue *pe);
static void parse_operand(TCCState *s1, Operand *op);
static void parse_branch_offset_operand(TCCState *s1, Operand *op);
static void parse_operands(TCCState *s1, Operand *ops, int count);
static void parse_mem_access_operands(TCCState *s1, Operand* ops);
ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier);
static void asm_c_opcode(TCCState *s1, int token);

/* XXX: make it faster ? */
ST_FUNC void g(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    ind = ind1;
}

ST_FUNC void gen_le16 (int i)
{
    g(i);
    g(i>>8);
}

ST_FUNC void gen_le32 (int i)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 4;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind++] = i & 0xFF;
    cur_text_section->data[ind++] = (i >> 8) & 0xFF;
    cur_text_section->data[ind++] = (i >> 16) & 0xFF;
    cur_text_section->data[ind++] = (i >> 24) & 0xFF;
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
    gen_le32(pe->v);
}

static void asm_emit_opcode(uint32_t opcode) {
    gen_le32(opcode);
}

static void asm_nullary_opcode(TCCState *s1, int token)
{
    switch (token) {
    // Sync instructions

    case TOK_ASM_fence_i: // I
        asm_emit_opcode((0x3 << 2) | 3| (1 << 12));
        return;

    // System calls

    case TOK_ASM_ecall: // I (pseudo)
        asm_emit_opcode((0x1C << 2) | 3 | (0 << 12));
        return;
    case TOK_ASM_ebreak: // I (pseudo)
        asm_emit_opcode((0x1C << 2) | 3 | (0 << 12) | (1 << 20));
        return;

    // Other

    case TOK_ASM_nop:
        asm_emit_i(token, (4 << 2) | 3, &zero, &zero, &zimm);
        return;

    case TOK_ASM_wfi:
        asm_emit_opcode((0x1C << 2) | 3 | (0x105 << 20));
        return;
    case TOK_ASM_sret:
        asm_emit_opcode((0x1C << 2) | 3 | (0x102 << 20));
        return;
    case TOK_ASM_mret:
        asm_emit_opcode((0x1C << 2) | 3 | (0x302 << 20));
        return;
    case TOK_ASM_unimp:
        /* csrrw zero, cycle, zero: always illegal */
        asm_emit_opcode((0x1C << 2) | 3 | (1 << 12) | (0xc00 << 20));
        return;

    /* Pseudoinstructions */
    case TOK_ASM_ret:
        /* jalr zero, x1, 0 */
        asm_emit_opcode( 0x67 | (0 << 12) | ENCODE_RS1(1) );
        return;

    default:
        expect("nullary instruction");
    }
}

/* Parse a text containing operand and store the result in OP */
static void parse_operand(TCCState *s1, Operand *op)
{
    ExprValue e = {0};
    int8_t reg;

    op->type = 0;

    if ((reg = asm_parse_regvar(tok)) != -1) {
        next(); // skip register name
        op->type = OP_REG;
        op->reg = (uint8_t) reg;
        return;
    } else if (tok == '$') {
        /* constant value */
        next(); // skip '#' or '$'
    } else if ((e.v = asm_parse_csrvar(tok)) != -1) {
        next();
    } else {
        asm_expr(s1, &e);
    }
    op->e = e;
    if (e.sym)
        op->type = OP_SYM;
    else if ((int64_t) e.v >= -0x1000 && (int64_t) e.v < 0x1000)
        op->type = OP_IM12S;
    else
        op->type = OP_IM32;
}

/* branch and jump targets: like any operand, but never registers */
static void parse_branch_offset_operand(TCCState *s1, Operand *op){
    ExprValue e = {0};

    asm_expr(s1, &e);
    op->e = e;
    if (e.sym)
        op->type = OP_SYM;
    else if ((int64_t) e.v >= -0x1000 && (int64_t) e.v < 0x1000)
        op->type = OP_IM12S;
    else
        op->type = OP_IM32;
}

#define parse_jump_offset_operand parse_branch_offset_operand

/* The pc-relative offset to a symbol operand that is already defined, local,
   and in the current section; otherwise emit relocation TYPE for it at the
   current position and return 0 (the linker fills the field in). */
static int64_t asm_sym_offset(const Operand *op, int type)
{
    Sym *sym = op->e.sym;
    ElfSym *esym = elfsym(sym);
    if ((sym->type.t & VT_STATIC) && esym && esym->st_shndx == cur_text_section->sh_num)
        return (int64_t)(esym->st_value + op->e.v) - ind;
    greloca(cur_text_section, sym, ind, type, op->e.v);
    return 0;
}

/* Relocations for an auipc at the current position and the instruction
   after it that refer to op's symbol: HITYPE on the auipc, and LOTYPE on the
   next instruction, pointing back at the auipc through a local label as the
   psABI requires. */
static void asm_pcrel_relocs(const Operand *op, int hitype, int lotype)
{
    Sym label = {0};
    greloca(cur_text_section, op->e.sym, ind, hitype, hitype == R_RISCV_GOT_HI20 ? 0 : op->e.v);
    label.type.t = VT_VOID | VT_STATIC;
    put_extern_sym(&label, cur_text_section, ind, 0);
    greloca(cur_text_section, &label, ind + 4, lotype, 0);
}

static void asm_need_sym(int token, const Operand *op)
{
    if (op->type != OP_SYM)
        tcc_error("'%s': expected a symbol", get_tok_str(token, NULL));
}

static void parse_operands(TCCState *s1, Operand* ops, int count){
    int i;
    for (i = 0; i < count; i++) {
        if ( i != 0 )
            skip(',');
        parse_operand(s1, &ops[i]);
    }
}

/* parse `X, imm(Y)` to {X, Y, imm} operands */
static void parse_mem_access_operands(TCCState *s1, Operand* ops){

    Operand op;

    parse_operand(s1, &ops[0]);
    skip(',');
    if ( tok == '(') {
        /* `X, (Y)` case*/
        next();
        parse_operand(s1, &ops[1]);
        skip(')');
        ops[2] = zimm;
    } else {
        parse_operand(s1, &ops[2]);
        if ( tok == '('){
            /* `X, imm(Y)` case*/
            next();
            parse_operand(s1, &ops[1]);
            skip(')');
        } else {
            /* `X, Y` case*/
            /* we parsed Y thinking it was imm, swap and default imm to zero */
            op = ops[2];
            ops[1] = ops[2];
            ops[2] = op;
            ops[2] = zimm;
        }
    }
}

/* This is special: First operand is optional */
static void asm_jal_opcode(TCCState *s1, int token){
    Operand ops[2];

    if (token == TOK_ASM_j ){
        ops[0] = zero; // j offset
    } else if (asm_parse_regvar(tok) == -1) {
        ops[0] = ra;   // jal offset
    } else {
        // jal reg, offset
        parse_operand(s1, &ops[0]);
        if ( tok == ',') next(); else expect("','");
    }
    parse_jump_offset_operand(s1, &ops[1]);
    asm_emit_j(token, 0x6f, &ops[0], &ops[1]);
}

/* The target of jr/jalr: "rs", "rs, imm", "imm(rs)" or "(rs)". */
static void parse_jump_target(TCCState *s1, Operand *rs, Operand *imm)
{
    *imm = zimm;
    if (tok == '(') {
        next();
        parse_operand(s1, rs);
        skip(')');
    } else if (asm_parse_regvar(tok) != -1) {
        parse_operand(s1, rs);
        if (tok == ',') {
            next();
            parse_operand(s1, imm);
        }
    } else {
        parse_operand(s1, imm);
        skip('(');
        parse_operand(s1, rs);
        skip(')');
    }
}

/* jalr target (rd = ra) or jalr rd, target */
static void asm_jalr_opcode(TCCState *s1, int token){
    Operand rd = ra, rs, imm;
    if (asm_parse_regvar(tok) != -1) {
        parse_operand(s1, &rd);
        if (tok != ',') {
            /* jalr rs */
            asm_emit_i(token, 0x67, &ra, &rd, &zimm);
            return;
        }
        next();
    }
    parse_jump_target(s1, &rs, &imm);
    asm_emit_i(token, 0x67, &rd, &rs, &imm);
}

static void asm_unary_opcode(TCCState *s1, int token)
{
    uint32_t opcode = (0x1C << 2) | 3 | (2 << 12);
    Operand op;

    parse_operands(s1, &op, 1);
    /* Note: Those all map to CSR--so they are pseudo-instructions. */
    opcode |= ENCODE_RD(op.reg);

    switch (token) {
    /* pseudoinstructions */
    case TOK_ASM_rdcycle:
        asm_emit_opcode(opcode | (0xC00 << 20));
        return;
    case TOK_ASM_rdcycleh:
        asm_emit_opcode(opcode | (0xC80 << 20));
        return;
    case TOK_ASM_rdtime:
        asm_emit_opcode(opcode | (0xC01 << 20) | ENCODE_RD(op.reg));
        return;
    case TOK_ASM_rdtimeh:
        asm_emit_opcode(opcode | (0xC81 << 20) | ENCODE_RD(op.reg));
        return;
    case TOK_ASM_rdinstret:
        asm_emit_opcode(opcode | (0xC02 << 20) | ENCODE_RD(op.reg));
        return;
    case TOK_ASM_rdinstreth:
        asm_emit_opcode(opcode | (0xC82 << 20) | ENCODE_RD(op.reg));
        return;
    case TOK_ASM_frflags:
        asm_emit_opcode(opcode | (0x001 << 20) | ENCODE_RD(op.reg));
        return;
    case TOK_ASM_frrm:
        asm_emit_opcode(opcode | (0x002 << 20) | ENCODE_RD(op.reg));
        return;
    case TOK_ASM_frcsr:
        asm_emit_opcode(opcode | (0x003 << 20) | ENCODE_RD(op.reg));
        return;

    case TOK_ASM_call:
        asm_need_sym(token, &op);
        /* auipc ra, 0 */
        greloca(cur_text_section, op.e.sym, ind, R_RISCV_CALL_PLT, op.e.v);
        asm_emit_opcode(3 | (5 << 2) | ENCODE_RD(1));
        /* jalr ra, 0(ra) */
        asm_emit_opcode(0x67 | (0 << 12) | ENCODE_RD(1) | ENCODE_RS1(1));
        return;
    case TOK_ASM_tail:
        asm_need_sym(token, &op);
        /* auipc x6, 0 */
        greloca(cur_text_section, op.e.sym, ind, R_RISCV_CALL_PLT, op.e.v);
        asm_emit_opcode(3 | (5 << 2) | ENCODE_RD(6));
        /* jalr zero, 0(x6) */
        asm_emit_opcode(0x67 | (0 << 12) | ENCODE_RS1(6));
        return;

    default:
        expect("unary instruction");
    }
}

static void asm_emit_u(int token, uint32_t opcode, const Operand* rd, const Operand* rs2)
{
    if (rd->type != OP_REG) {
        tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_IM12S && rs2->type != OP_IM32) {
        tcc_error("'%s': Expected second source operand that is an immediate value", get_tok_str(token, NULL));
    } else if (rs2->e.v >= 0x100000) {
        tcc_error("'%s': Expected second source operand that is an immediate value between 0 and 0xfffff", get_tok_str(token, NULL));
    }
    /* U-type instruction:
	      31...12 imm[31:12]
	      11...7 rd
	      6...0 opcode */
    gen_le32(opcode | ENCODE_RD(rd->reg) | (rs2->e.v << 12));
}

/* a fence operand: some of i, o, r, w (in that order) */
static int parse_fence_operand(){
    static const char iorw[] = "iorw";
    const char *p = get_tok_str(tok, NULL), *order = iorw;
    int bits = 0;
    if (tok < TOK_IDENT)
        return -1;
    for (; *p; p++) {
        const char *c = strchr(order, *p);
        if (!c)
            return -1;
        bits |= 8 >> (c - iorw);
        order = c + 1;
    }
    next();
    return bits;
}

static void asm_fence_opcode(TCCState *s1, int token){
    // `fence` is both an instruction and a pseudoinstruction:
    // `fence` expands to `fence iorw, iorw`
    int succ = 0xF, pred = 0xF;
    if (tok != TOK_LINEFEED && tok != ';' && tok != CH_EOF){
        pred = parse_fence_operand();
        if ( pred > 0xF || pred < 0) {
            tcc_error("'%s': Expected first operand that is a valid predecessor operand", get_tok_str(token, NULL));
        }
        skip(',');
        succ = parse_fence_operand();
        if ( succ > 0xF || succ < 0) {
            tcc_error("'%s': Expected second operand that is a valid successor operand", get_tok_str(token, NULL));
        }
    }
    asm_emit_opcode((0x3 << 2) | 3 | (0 << 12) | succ<<20 | pred<<24);
}

/* fence.tso: fence rw, rw with fm = 1000 */
static void asm_fence_tso_opcode(void){
    asm_emit_opcode((0x3 << 2) | 3 | (0x8u << 28) | 0x3 << 24 | 0x3 << 20);
}

/* li rd, VAL: the same sequence as GNU as (load_const): lui and/or addiw
   for a sign-extended 32-bit value, otherwise the upper part built
   recursively, shifted into place, plus the low 12 bits. A small value on
   its own is addi rd, zero, val; inside a longer sequence gas uses addiw. */
static void asm_emit_li(int token, const Operand *rd, int64_t val, int nested)
{
    Operand imm = { OP_IM12S };
    int64_t lo = (int64_t)((uint64_t)val << 52) >> 52;
    uint64_t up = (uint64_t)val - lo;
    int shift = 12;

    if (val != (int32_t)val) {
        while (!((up >> shift) & 1))
            shift++;
        asm_emit_li(token, rd, (int64_t)up >> shift, 1);
        imm.e.v = shift;
        /* slli rd, rd, shift */
        asm_emit_i(token, (4 << 2) | 3 | (1 << 12), rd, rd, &imm);
        if (lo) {
            imm.e.v = lo;
            /* addi rd, rd, lo */
            asm_emit_i(token, (4 << 2) | 3, rd, rd, &imm);
        }
        return;
    }
    if (up) {
        imm.e.v = (uint32_t)up >> 12;
        /* lui rd, up */
        asm_emit_u(token, (0xD << 2) | 3, rd, &imm);
    }
    if (lo || !up) {
        imm.e.v = lo;
        /* addiw rd, rd, lo (or addi rd, zero, lo) */
        asm_emit_i(token, (up || nested ? 6 << 2 : 4 << 2) | 3, rd, up ? rd : &zero, &imm);
    }
}

static void asm_binary_opcode(TCCState* s1, int token)
{
    Operand imm = { OP_IM12S };
    Operand ops[2];
    int32_t lo;
    uint32_t hi;

    parse_operands(s1, &ops[0], 2);
    switch (token) {
    case TOK_ASM_lui:
        asm_emit_u(token, (0xD << 2) | 3, &ops[0], &ops[1]);
        return;
    case TOK_ASM_auipc:
        asm_emit_u(token, (0x05 << 2) | 3, &ops[0], &ops[1]);
        return;

    /* F/D extension */
    case TOK_ASM_fsqrt_d:
        asm_emit_fb(token, 0x53 | (11 << 27) | (1 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1);
        return;
    case TOK_ASM_fmv_x_w:
    case TOK_ASM_fmv_w_x:
    case TOK_ASM_fmv_x_d:
    case TOK_ASM_fmv_d_x: {
        int to_int = token == TOK_ASM_fmv_x_w || token == TOK_ASM_fmv_x_d;
        int dbl = token == TOK_ASM_fmv_x_d || token == TOK_ASM_fmv_d_x;
        if (ops[0].type != OP_REG || ops[1].type != OP_REG ||
            !REG_IS_FLOAT(ops[0].reg) == !to_int || !REG_IS_FLOAT(ops[1].reg) == to_int)
            tcc_error("'%s': wrong register types", get_tok_str(token, NULL));
        asm_emit_opcode(0x53 | ((to_int ? 0x70 : 0x78) + dbl) << 25 | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg));
        return;
    }
    case TOK_ASM_fsqrt_s:
        asm_emit_fb(token, 0x53 | (11 << 27) | (0 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1);
        return;

    /* pseudoinstructions */
    /* rd, sym */
    case TOK_ASM_la:
    case TOK_ASM_lla:
        /* auipc rd, %pcrel_hi(sym); addi rd, rd, %pcrel_lo(.) -- or with
           .option pic, la loads the address from the GOT:
           auipc rd, %got_pcrel_hi(sym); ld rd, %pcrel_lo(.)(rd) */
        asm_need_sym(token, &ops[1]);
        if (ops[0].type != OP_REG || REG_IS_FLOAT(ops[0].reg))
            tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
        imm.e.v = 0;
        if (token == TOK_ASM_la && riscv_asm_pic) {
            asm_pcrel_relocs(&ops[1], R_RISCV_GOT_HI20, R_RISCV_PCREL_LO12_I);
            asm_emit_u(token, 3 | (5 << 2), ops, &imm);
            asm_emit_i(token, 3 | (3 << 12), ops, ops, &imm);
        } else {
            asm_pcrel_relocs(&ops[1], R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I);
            asm_emit_u(token, 3 | (5 << 2), ops, &imm);
            asm_emit_i(token, 3 | (4 << 2), ops, ops, &imm);
        }
        return;
    case TOK_ASM_li:
        if (ops[0].type != OP_REG || REG_IS_FLOAT(ops[0].reg))
            tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
        if (ops[1].type != OP_IM32 && ops[1].type != OP_IM12S)
            tcc_error("'%s': Expected an immediate value", get_tok_str(token, NULL));
        asm_emit_li(token, &ops[0], ops[1].e.v, 0);
        return;
    case TOK_ASM_mv:
        /* addi rd, rs, 0 */
        asm_emit_i(token, 3 | (4 << 2), &ops[0], &ops[1], &imm);
        return;
    case TOK_ASM_not:
        /* xori rd, rs, -1 */
        imm.e.v = -1;
        asm_emit_i(token, (0x4 << 2) | 3 | (4 << 12), &ops[0], &ops[1], &imm);
        return;
    case TOK_ASM_neg:
        /* sub rd, x0, rs2 */
        asm_emit_r(token, (0xC << 2) | 3 | (32 << 25), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_negw:
        /* subw rd, x0, rs2 */
        asm_emit_r(token, (0xE << 2) | 3 | (32 << 25), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_sext_w:
        /* addiw rd, rs, 0 */
        asm_emit_i(token, 0x1b, &ops[0], &ops[1], &zimm);
        return;
    case TOK_ASM_fneg_s:
        /* fsgnjn.s rd, rs, rs */
        asm_emit_f(token, 0x53 | (1 << 12) | (0 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]);
        return;
    case TOK_ASM_fneg_d:
        /* fsgnjn.d rd, rs, rs */
        asm_emit_f(token, 0x53 | (1 << 12) | (1 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]);
        return;
    case TOK_ASM_fmv_s:
        /* fsgnj.s rd, rs, rs */
        asm_emit_f(token, 0x53 | (0 << 12) | (0 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]);
        return;
    case TOK_ASM_fmv_d:
        /* fsgnj.d rd, rs, rs */
        asm_emit_f(token, 0x53 | (0 << 12) | (1 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]);
        return;
    /* FCVT instructions now handled by asm_fcvt_opcode() with
       optional rounding mode operand (GNU as syntax:
       fcvt.w.s rd, rs1 [, rtz/rne/...) */

    case TOK_ASM_jump:
        /* jump sym, temp: auipc temp, 0; jalr zero, 0(temp) */
        asm_need_sym(token, &ops[0]);
        if (ops[1].type != OP_REG || REG_IS_FLOAT(ops[1].reg))
            tcc_error("'%s': expected a temporary register", get_tok_str(token, NULL));
        greloca(cur_text_section, ops[0].e.sym, ind, R_RISCV_CALL_PLT, ops[0].e.v);
        asm_emit_opcode(3 | (5 << 2) | ENCODE_RD(ops[1].reg));
        asm_emit_opcode(0x67 | (0 << 12) | ENCODE_RS1(ops[1].reg));
        return;
    case TOK_ASM_seqz:
        /* sltiu rd, rs, 1 */
        imm.e.v = 1;
        asm_emit_i(token, (0x4 << 2) | 3 | (3 << 12), &ops[0], &ops[1], &imm);
        return;
    case TOK_ASM_snez:
        /* sltu rd, zero, rs */
        imm.e.v = 1;
        asm_emit_r(token, (0xC << 2) | 3 | (3 << 12), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_sltz:
        /* slt rd, rs, zero */
        asm_emit_r(token, (0xC << 2) | 3 | (2 << 12), &ops[0], &ops[1], &zero);
        return;
    case TOK_ASM_sgtz:
        /* slt rd, zero, rs */
        asm_emit_r(token, (0xC << 2) | 3 | (2 << 12), &ops[0], &zero, &ops[1]);
        return;

    case TOK_ASM_fabs_d:
        /* fsgnjx.d rd, rs, rs */
        asm_emit_f(token, 0x53 | (4 << 27) | (1 << 25) | (2 << 12), &ops[0], &ops[1], &ops[1]);
        return;
    case TOK_ASM_fabs_s:
        /* fsgnjx.s rd, rs, rs */
        asm_emit_f(token, 0x53 | (4 << 27) | (0 << 25) | (2 << 12), &ops[0], &ops[1], &ops[1]);
        return;

    /* CSR pseudo-instructions */
    case TOK_ASM_csrr:
        /* csrrs rd, csr, x0 */
        asm_emit_opcode(0x73 | (2 << 12) | (asm_csr_num(token, &ops[1]) << 20) | ENCODE_RD(ops[0].reg));
        return;
    case TOK_ASM_csrw:
    case TOK_ASM_csrs:
    case TOK_ASM_csrc: {
        /* csrr{w,s,c} x0, csr, rs; with a 5-bit immediate (as gas allows,
           and as an "rK" asm operand gives) csrr{w,s,c}i x0, csr, uimm */
        int f3 = token == TOK_ASM_csrw ? 1 : token == TOK_ASM_csrs ? 2 : 3;
        uint32_t src;
        if (ops[1].type == OP_REG && !REG_IS_FLOAT(ops[1].reg)) {
            src = ops[1].reg;
        } else if (ops[1].type == OP_IM12S && ops[1].e.v <= 31) {
            src = ops[1].e.v;
            f3 += 4;
        } else {
            tcc_error("'%s': expected a register or an immediate 0-31", get_tok_str(token, NULL));
        }
        asm_emit_opcode(0x73 | (f3 << 12) | (asm_csr_num(token, &ops[0]) << 20) | ENCODE_RS1(src));
        return;
    }
    case TOK_ASM_csrwi:
        /* csrrwi x0, csr, uimm */
        asm_emit_opcode(0x73 | (5 << 12) | (asm_csr_num(token, &ops[0]) << 20) | ENCODE_RS1(ops[1].e.v));
        return;
    case TOK_ASM_csrsi:
        /* csrrsi x0, csr, uimm */
        asm_emit_opcode(0x73 | (6 << 12) | (asm_csr_num(token, &ops[0]) << 20) | ENCODE_RS1(ops[1].e.v));
        return;
    case TOK_ASM_csrci:
        /* csrrci x0, csr, uimm */
        asm_emit_opcode(0x73 | (7 << 12) | (asm_csr_num(token, &ops[0]) << 20) | ENCODE_RS1(ops[1].e.v));
        return;
    default:
        expect("binary instruction");
    }
}

/* caller: Add funct3, funct7 into opcode */
static void asm_emit_r(int token, uint32_t opcode, const Operand* rd, const Operand* rs1, const Operand* rs2)
{
    if (rd->type != OP_REG) {
        tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
    }
    if (rs1->type != OP_REG) {
        tcc_error("'%s': Expected first source operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_REG) {
        tcc_error("'%s': Expected second source operand that is a register or immediate", get_tok_str(token, NULL));
    }
    /* R-type instruction:
	     31...25 funct7
	     24...20 rs2
	     19...15 rs1
	     14...12 funct3
	     11...7 rd
	     6...0 opcode */
    gen_le32(opcode | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg));
}

/* caller: Add rounding mode, fmt, funct5 to opcode */
static void asm_emit_f(int token, uint32_t opcode, const Operand* rd, const Operand* rs1, const Operand* rs2)
{
    if (rd->type != OP_REG || !REG_IS_FLOAT(rd->reg)) {
        tcc_error("'%s': Expected destination operand that is a floating-point register", get_tok_str(token, NULL));
    }
    if (rs1->type != OP_REG || !REG_IS_FLOAT(rs1->reg)) {
        tcc_error("'%s': Expected first source operand that is a floating-point register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_REG || !REG_IS_FLOAT(rs2->reg)) {
        tcc_error("'%s': Expected second source operand that is a floating-point register", get_tok_str(token, NULL));
    }
    /* F-type instruction:
	     31...27 funct5
	     26...25 fmt
	     24...20 rs2
	     19...15 rs1
	     14...12 rm
	     11...7 rd
	     6...0 opcode = OP-FP */
    gen_le32(opcode | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg));
}
/* caller: Add rounding mode, fmt, funct5 to opcode */
static void asm_emit_fb(int token, uint32_t opcode, const Operand* rd, const Operand* rs)
{
    if (rd->type != OP_REG || !REG_IS_FLOAT(rd->reg)) {
        tcc_error("'%s': Expected destination operand that is a floating-point register", get_tok_str(token, NULL));
    }
    if (rs->type != OP_REG || !REG_IS_FLOAT(rs->reg)) {
        tcc_error("'%s': Expected source operand that is a floating-point register", get_tok_str(token, NULL));
    }
    /* F-type instruction:
	     31...27 funct5
	     26...25 fmt
	     24...20 rs2 = 0
	     19...15 rs1 = rs
	     14...12 rm
	     11...7 rd
	     6...0 opcode = OP-FP */
    gen_le32(opcode | ENCODE_RD(rd->reg) | ENCODE_RS1(rs->reg) | ENCODE_RS2(0));
}
/* caller: Add rounding mode, fmt to opcode */
static void asm_emit_fq(int token, uint32_t opcode, const Operand* rd, const Operand* rs1, const Operand* rs2, const Operand* rs3)
{
    if (rd->type != OP_REG || !REG_IS_FLOAT(rd->reg)) {
        tcc_error("'%s': Expected destination operand that is a floating-point register", get_tok_str(token, NULL));
    }
    if (rs1->type != OP_REG || !REG_IS_FLOAT(rs1->reg)) {
        tcc_error("'%s': Expected first source operand that is a floating-point register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_REG || !REG_IS_FLOAT(rs2->reg)) {
        tcc_error("'%s': Expected second source operand that is a floating-point register", get_tok_str(token, NULL));
    }
    if (rs3->type != OP_REG || !REG_IS_FLOAT(rs3->reg)) {
        tcc_error("'%s': Expected third source operand that is a floating-point register", get_tok_str(token, NULL));
    }
    /* F-type instruction:
	     31...27 rs3
	     26...25 fmt
	     24...20 rs2
	     19...15 rs1
	     14...12 rm
	     11...7 rd
	     6...0 opcode */
    gen_le32(opcode | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) | (REG_VALUE(rs3->reg) << 27));
}

/* caller: Add funct3 into opcode */
static void asm_emit_i(int token, uint32_t opcode, const Operand* rd, const Operand* rs1, const Operand* rs2)
{
    if (rd->type != OP_REG) {
        tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
    }
    if (rs1->type != OP_REG) {
        tcc_error("'%s': Expected first source operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_IM12S) {
        tcc_error("'%s': Expected second source operand that is an immediate value between 0 and 8191", get_tok_str(token, NULL));
    }
    /* I-type instruction:
	     31...20 imm[11:0]
	     19...15 rs1
	     14...12 funct3
	     11...7 rd
	     6...0 opcode */

    gen_le32(opcode | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) | (rs2->e.v << 20));
}

static void asm_emit_j(int token, uint32_t opcode, const Operand* rd, const Operand* rs2)
{
    uint32_t imm;

    if (rd->type != OP_REG) {
        tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type == OP_SYM) {
        imm = asm_sym_offset(rs2, R_RISCV_JAL);
    } else if (rs2->type != OP_IM12S && rs2->type != OP_IM32) {
        tcc_error("'%s': Expected second source operand that is an immediate value", get_tok_str(token, NULL));
    } else {
        imm = rs2->e.v;
    }

    /* even offsets in a +- 1 MiB range */
    if ((int)imm > (1 << 20) -1 || (int)imm < -(1 << 20)) {
        tcc_error("'%s': Expected second source operand that is an immediate value between 0 and 0x1fffff", get_tok_str(token, NULL));
    }

    if (imm & 1) {
        tcc_error("'%s': Expected second source operand that is an even immediate value", get_tok_str(token, NULL));
    }
    /* J-type instruction:
    31      imm[20]
    30...21 imm[10:1]
    20      imm[11]
    19...12 imm[19:12]
    11...7  rd
    6...0   opcode */
    gen_le32(opcode | ENCODE_RD(rd->reg) | (((imm >> 20) & 1) << 31) | (((imm >> 1) & 0x3ff) << 21) | (((imm >> 11) & 1) << 20) | (((imm >> 12) & 0xff) << 12));
}

static void asm_mem_access_opcode(TCCState *s1, int token)
{

    Operand ops[3];
    parse_mem_access_operands(s1, &ops[0]);

    /* Pseudoinstructions with a symbol:
     *   l{b,h,w,d}[u] rd, sym      auipc rd, %pcrel_hi(sym); l rd, %pcrel_lo(rd)
     *   fld/s{b,h,w,d}/fsd r, sym, rt  auipc rt, ...; s r, %pcrel_lo(rt)
     */
    if (ops[1].type == OP_SYM) {
        Operand sym = ops[1], base = ops[0];
        int is_store = token == TOK_ASM_sb || token == TOK_ASM_sh || token == TOK_ASM_sw ||
                       token == TOK_ASM_sd || token == TOK_ASM_fsd || token == TOK_ASM_fsw;
        if (is_store || REG_IS_FLOAT(ops[0].reg)) {
            skip(',');
            parse_operand(s1, &base);
        }
        if (base.type != OP_REG || REG_IS_FLOAT(base.reg))
            tcc_error("'%s': expected an integer register for the address", get_tok_str(token, NULL));
        ops[1] = base;
        ops[2].type = OP_IM12S;
        ops[2].e.v  = 0;
        ops[2].e.sym = NULL;
        asm_pcrel_relocs(&sym, R_RISCV_PCREL_HI20,
                         is_store ? R_RISCV_PCREL_LO12_S : R_RISCV_PCREL_LO12_I);
        /* auipc base, 0 */
        asm_emit_u(token, (0x05 << 2) | 3, &base, &ops[2]);
    }

    switch (token) {
    // l{b|h|w|d}[u] rd, imm(rs1); I-format
    case TOK_ASM_lb:
         asm_emit_i(token, (0x0 << 2) | 3, &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_lh:
         asm_emit_i(token, (0x0 << 2) | 3 | (1 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_lw:
         asm_emit_i(token, (0x0 << 2) | 3 | (2 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_ld:
         asm_emit_i(token, (0x0 << 2) | 3 | (3 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_lbu:
         asm_emit_i(token, (0x0 << 2) | 3 | (4 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_lhu:
         asm_emit_i(token, (0x0 << 2) | 3 | (5 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_lwu:
         asm_emit_i(token, (0x0 << 2) | 3 | (6 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_fld:
         asm_emit_i(token, (0x1 << 2) | 3 | (3 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_flw:
         asm_emit_i(token, (0x1 << 2) | 3 | (2 << 12), &ops[0], &ops[1], &ops[2]);
         return;

    // s{b|h|w|d} rs2, imm(rs1); S-format (with rsX swapped)
    case TOK_ASM_sb:
         asm_emit_s(token, (0x8 << 2) | 3 | (0 << 12), &ops[1], &ops[0], &ops[2]);
         return;
    case TOK_ASM_sh:
         asm_emit_s(token, (0x8 << 2) | 3 | (1 << 12), &ops[1], &ops[0], &ops[2]);
         return;
    case TOK_ASM_sw:
         asm_emit_s(token, (0x8 << 2) | 3 | (2 << 12), &ops[1], &ops[0], &ops[2]);
         return;
    case TOK_ASM_sd:
         asm_emit_s(token, (0x8 << 2) | 3 | (3 << 12), &ops[1], &ops[0], &ops[2]);
         return;
    case TOK_ASM_fsd:
         asm_emit_s(token, (0x9 << 2) | 3 | (3 << 12), &ops[1], &ops[0], &ops[2]);
         return;
    case TOK_ASM_fsw:
         asm_emit_s(token, (0x9 << 2) | 3 | (2 << 12), &ops[1], &ops[0], &ops[2]);
         return;
    }
}

static void asm_branch_opcode(TCCState *s1, int token, int argc)
{
    Operand ops[3];
    parse_operands(s1, &ops[0], argc-1);
    skip(',');
    parse_branch_offset_operand(s1, &ops[argc-1]);

    switch(token){
    /* branch (RS1, RS2, IMM); B-format */
    case TOK_ASM_beq:
        asm_emit_b(token, 0x63 | (0 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_bne:
        asm_emit_b(token, 0x63 | (1 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_blt:
        asm_emit_b(token, 0x63 | (4 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_bge:
        asm_emit_b(token, 0x63 | (5 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_bltu:
        asm_emit_b(token, 0x63 | (6 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_bgeu:
        asm_emit_b(token, 0x63 | (7 << 12), ops, ops + 1, ops + 2);
        return;
    /* related pseudoinstructions */
    case TOK_ASM_bgt:
        asm_emit_b(token, 0x63 | (4 << 12), ops + 1, ops, ops + 2);
        return;
    case TOK_ASM_ble:
        asm_emit_b(token, 0x63 | (5 << 12), ops + 1, ops, ops + 2);
        return;
    case TOK_ASM_bgtu:
        asm_emit_b(token, 0x63 | (6 << 12), ops + 1, ops, ops + 2);
        return;
    case TOK_ASM_bleu:
        asm_emit_b(token, 0x63 | (7 << 12), ops + 1, ops, ops + 2);
        return;
    /* shorter pseudoinstructions */
    case TOK_ASM_bnez:
        /* bne rs, zero, offset */
        asm_emit_b(token, 0x63 | (1 << 12), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_beqz:
        /* bne rs, zero, offset */
        asm_emit_b(token, 0x63 | (0 << 12), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_blez:
        /* bge zero, rs, offset */
        asm_emit_b(token, 0x63 | (5 << 12), &zero, &ops[0], &ops[1]);
        return;
    case TOK_ASM_bgez:
        /* bge rs, zero, offset */
        asm_emit_b(token, 0x63 | (5 << 12), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_bltz:
        /* blt rs, zero, offset */
        asm_emit_b(token, 0x63 | (4 << 12), &ops[0], &zero, &ops[1]);
        return;
    case TOK_ASM_bgtz:
        /* blt zero, rs, offset */
        asm_emit_b(token, 0x63 | (4 << 12), &zero, &ops[0], &ops[1]);
        return;
    }
}

static void asm_ternary_opcode(TCCState *s1, int token)
{
    Operand ops[3];
    parse_operands(s1, &ops[0], 3);

    switch (token) {
    case TOK_ASM_sll:
        asm_emit_r(token, (0xC << 2) | 3 | (1 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_slli:
        asm_emit_i(token, (4 << 2) | 3 | (1 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_srl:
        asm_emit_r(token, (0xC << 2) | 3 | (5 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_srli:
        asm_emit_i(token, (0x4 << 2) | 3 | (5 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_sra:
        asm_emit_r(token, (0xC << 2) | 3 | (5 << 12) | (32 << 25), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_srai:
        asm_emit_i(token, (0x4 << 2) | 3 | (5 << 12) | (16 << 26), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_sllw:
        asm_emit_r(token, (0xE << 2) | 3 | (1 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_slliw:
        asm_emit_i(token, (6 << 2) | 3 | (1 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_srlw:
        asm_emit_r(token, (0xE << 2) | 3 | (5 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_srliw:
        asm_emit_i(token, (0x6 << 2) | 3 | (5 << 12), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_sraw:
        asm_emit_r(token, (0xE << 2) | 3 | (5 << 12) | (32 << 25), &ops[0], &ops[1], &ops[2]);
        return;
    case TOK_ASM_sraiw:
        asm_emit_i(token, (0x6 << 2) | 3 | (5 << 12) | (32 << 25), &ops[0], &ops[1], &ops[2]);
        return;

    // Arithmetic (RD,RS1,(RS2|IMM)); R-format, I-format or U-format

    case TOK_ASM_add:
         asm_emit_r(token, (0xC << 2) | 3, &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_addi:
         asm_emit_i(token, (4 << 2) | 3, &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_sub:
         asm_emit_r(token, (0xC << 2) | 3 | (32 << 25), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_addw:
         asm_emit_r(token, (0xE << 2) | 3 | (0 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_addiw: // 64 bit
         asm_emit_i(token, (0x6 << 2) | 3 | (0 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_subw:
         asm_emit_r(token, (0xE << 2) | 3 | (0 << 12) | (32 << 25), &ops[0], &ops[1], &ops[2]);
         return;

    // Logical (RD,RS1,(RS2|IMM)); R-format or I-format

    case TOK_ASM_xor:
         asm_emit_r(token, (0xC << 2) | 3 | (4 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_xori:
         asm_emit_i(token, (0x4 << 2) | 3 | (4 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_or:
         asm_emit_r(token, (0xC << 2) | 3 | (6 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_ori:
         asm_emit_i(token, (0x4 << 2) | 3 | (6 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_and:
         asm_emit_r(token, (0xC << 2) | 3 | (7 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_andi:
         asm_emit_i(token, (0x4 << 2) | 3 | (7 << 12), &ops[0], &ops[1], &ops[2]);
         return;

    // Compare (RD,RS1,(RS2|IMM)); R-format or I-format

    case TOK_ASM_slt:
         asm_emit_r(token, (0xC << 2) | 3 | (2 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_slti:
         asm_emit_i(token, (0x4 << 2) | 3 | (2 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_sltu:
         asm_emit_r(token, (0xC << 2) | 3 | (3 << 12), &ops[0], &ops[1], &ops[2]);
         return;
    case TOK_ASM_sltiu:
         asm_emit_i(token, (0x4 << 2) | 3 | (3 << 12), &ops[0], &ops[1], &ops[2]);
         return;

    /* M extension */
    case TOK_ASM_div:
        asm_emit_r(token, 0x33 | (4 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_divu:
        asm_emit_r(token, 0x33 | (5 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_divuw:
        asm_emit_r(token, 0x3b | (5 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_divw:
        asm_emit_r(token, 0x3b | (4 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_mul:
        asm_emit_r(token, 0x33 | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_mulh:
        asm_emit_r(token, 0x33 | (1 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_mulhsu:
        asm_emit_r(token, 0x33 | (2 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_mulhu:
        asm_emit_r(token, 0x33 | (3 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_mulw:
        asm_emit_r(token, 0x3b | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_rem:
        asm_emit_r(token, 0x33 | (6 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_remu:
        asm_emit_r(token, 0x33 | (7 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_remuw:
        asm_emit_r(token, 0x3b | (7 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_remw:
        asm_emit_r(token, 0x3b | (6 << 12) | (1 << 25), ops, ops + 1, ops + 2);
        return;

    /* Zicsr extension; (rd, csr, rs/uimm) */
    case TOK_ASM_csrrc:
        asm_emit_i(token, 0x73 | (3 << 12), ops, ops + 2, ops + 1);
        return;
    case TOK_ASM_csrrci:
        /* using rs1 field for uimmm */
        ops[2].type = OP_REG;
        asm_emit_i(token, 0x73 | (7 << 12), ops, ops + 2, ops + 1);
        return;
    case TOK_ASM_csrrs:
        asm_emit_i(token, 0x73 | (2 << 12), ops, ops + 2, ops + 1);
        return;
    case TOK_ASM_csrrsi:
        ops[2].type = OP_REG;
        asm_emit_i(token, 0x73 | (6 << 12), ops, ops + 2, ops + 1);
        return;
    case TOK_ASM_csrrw:
        asm_emit_i(token, 0x73 | (1 << 12), ops, ops + 2, ops + 1);
        return;
    case TOK_ASM_csrrwi:
        ops[2].type = OP_REG;
        asm_emit_i(token, 0x73 | (5 << 12), ops, ops + 2, ops + 1);
        return;

    /* F/D extension */
    case TOK_ASM_fsgnjn_s:
    case TOK_ASM_fsgnjn_d:
    case TOK_ASM_fsgnjx_s:
    case TOK_ASM_fsgnjx_d:
        asm_emit_f(token, 0x53 | (4 << 27) | ((token == TOK_ASM_fsgnjn_d || token == TOK_ASM_fsgnjx_d) << 25)
                   | ((token == TOK_ASM_fsgnjn_s || token == TOK_ASM_fsgnjn_d ? 1 : 2) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fadd_d:
        asm_emit_f(token, 0x53 | (0 << 27) | (1 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fadd_s:
        asm_emit_f(token, 0x53 | (0 << 27) | (0 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fsub_d:
        asm_emit_f(token, 0x53 | (1 << 27) | (1 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fsub_s:
        asm_emit_f(token, 0x53 | (1 << 27) | (0 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fmul_d:
        asm_emit_f(token, 0x53 | (2 << 27) | (1 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fmul_s:
        asm_emit_f(token, 0x53 | (2 << 27) | (0 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fdiv_d:
        asm_emit_f(token, 0x53 | (3 << 27) | (1 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fdiv_s:
        asm_emit_f(token, 0x53 | (3 << 27) | (0 << 25) | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fsgnj_d:
        asm_emit_f(token, 0x53 | (4 << 27) | (1 << 25) | (0 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fsgnj_s:
        asm_emit_f(token, 0x53 | (4 << 27) | (0 << 25) | (0 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fmax_d:
        asm_emit_f(token, 0x53 | (5 << 27) | (1 << 25) | (1 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fmax_s:
        asm_emit_f(token, 0x53 | (5 << 27) | (0 << 25) | (1 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fmin_d:
        asm_emit_f(token, 0x53 | (5 << 27) | (1 << 25) | (0 << 12), ops, ops + 1, ops + 2);
        return;
    case TOK_ASM_fmin_s:
        asm_emit_f(token, 0x53 | (5 << 27) | (0 << 25) | (0 << 12), ops, ops + 1, ops + 2);
        return;

    /* F/D comparison: produce integer result, encode manually */
    case TOK_ASM_feq_s:
        asm_emit_opcode(0x53 | (0x14 << 27) | (0 << 25) | (2 << 12) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg) | ENCODE_RS2(ops[2].reg));
        return;
    case TOK_ASM_feq_d:
        asm_emit_opcode(0x53 | (0x14 << 27) | (1 << 25) | (2 << 12) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg) | ENCODE_RS2(ops[2].reg));
        return;
    case TOK_ASM_flt_s:
        asm_emit_opcode(0x53 | (0x14 << 27) | (0 << 25) | (1 << 12) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg) | ENCODE_RS2(ops[2].reg));
        return;
    case TOK_ASM_flt_d:
        asm_emit_opcode(0x53 | (0x14 << 27) | (1 << 25) | (1 << 12) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg) | ENCODE_RS2(ops[2].reg));
        return;
    case TOK_ASM_fle_s:
        asm_emit_opcode(0x53 | (0x14 << 27) | (0 << 25) | (0 << 12) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg) | ENCODE_RS2(ops[2].reg));
        return;
    case TOK_ASM_fle_d:
        asm_emit_opcode(0x53 | (0x14 << 27) | (1 << 25) | (0 << 12) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg) | ENCODE_RS2(ops[2].reg));
        return;

    default:
        expect("ternary instruction");
    }
}

static void asm_quaternary_opcode(TCCState *s1, int token)
{
    Operand ops[4];
    parse_operands(s1, &ops[0], 4);

    switch (token) {
    case TOK_ASM_fmadd_s:  case TOK_ASM_fmadd_d:
    case TOK_ASM_fmsub_s:  case TOK_ASM_fmsub_d:
    case TOK_ASM_fnmsub_s: case TOK_ASM_fnmsub_d:
    case TOK_ASM_fnmadd_s: case TOK_ASM_fnmadd_d: {
        static const struct { int tok; uint32_t op; } fma[] = {
            { TOK_ASM_fmadd_s, 0x43 }, { TOK_ASM_fmadd_d, 0x43 | 1 << 25 },
            { TOK_ASM_fmsub_s, 0x47 }, { TOK_ASM_fmsub_d, 0x47 | 1 << 25 },
            { TOK_ASM_fnmsub_s, 0x4b }, { TOK_ASM_fnmsub_d, 0x4b | 1 << 25 },
            { TOK_ASM_fnmadd_s, 0x4f }, { TOK_ASM_fnmadd_d, 0x4f | 1 << 25 },
        };
        int i;
        for (i = 0; fma[i].tok != token; i++)
            ;
        asm_emit_fq(token, fma[i].op | (asm_fcvt_rm(s1) << 12), ops, ops + 1, ops + 2, ops + 3);
        return;
    }

    default:
        expect("quaternary instruction");
    }
}

static void asm_atomic_opcode(TCCState *s1, int token)
{
    Operand ops[3];

    parse_operand(s1, &ops[0]);
    skip(',');

    if ( token <= TOK_ASM_lr_d_aqrl && token >= TOK_ASM_lr_w ) {
        ops[1] = zero;
    } else {
        parse_operand(s1, &ops[1]);
        skip(',');
    }

    skip('(');
    parse_operand(s1, &ops[2]);
    skip(')');

    switch(token){
        case TOK_ASM_lr_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 0, 0);
            break;
        case TOK_ASM_lr_w_aq:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 1, 0);
            break;
        case TOK_ASM_lr_w_rl:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 0, 1);
            break;
        case TOK_ASM_lr_w_aqrl:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 1, 1);
            break;

        case TOK_ASM_lr_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 0, 0);
            break;
        case TOK_ASM_lr_d_aq:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 1, 0);
            break;
        case TOK_ASM_lr_d_rl:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 0, 1);
            break;
        case TOK_ASM_lr_d_aqrl:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x2<<27, &ops[0], &ops[1], &ops[2], 1, 1);
            break;

        case TOK_ASM_sc_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 0, 0);
            break;
        case TOK_ASM_sc_w_aq:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 1, 0);
            break;
        case TOK_ASM_sc_w_rl:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 0, 1);
            break;
        case TOK_ASM_sc_w_aqrl:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 1, 1);
            break;

        case TOK_ASM_sc_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 0, 0);
            break;
        case TOK_ASM_sc_d_aq:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 1, 0);
            break;
        case TOK_ASM_sc_d_rl:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 0, 1);
            break;
        case TOK_ASM_sc_d_aqrl:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x3<<27, &ops[0], &ops[1], &ops[2], 1, 1);
            break;

        /* AMO instructions (base, aq=0 rl=0) */
        case TOK_ASM_amoadd_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoadd_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoswap_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoswap_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoand_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0xC<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoand_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0xC<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoor_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoor_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoxor_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amoxor_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amomax_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amomax_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amomaxu_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x1C<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amomaxu_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x1C<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amomin_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amomin_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amominu_w:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;
        case TOK_ASM_amominu_d:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 0, 0); break;

        /* AMO aq/rl variants */
        case TOK_ASM_amoadd_w_aq:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoadd_w_rl:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoadd_w_aqrl:
            asm_emit_a(token, 0x2F | 0x2<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoadd_d_aq:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoadd_d_rl:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoadd_d_aqrl:
            asm_emit_a(token, 0x2F | 0x3<<12 | 0x0<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;

        case TOK_ASM_amoswap_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoswap_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoswap_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoswap_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoswap_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoswap_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x1<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoand_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0xc<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoand_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0xc<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoand_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0xc<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoand_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0xc<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoand_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0xc<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoand_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0xc<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoor_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoor_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoor_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoor_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoor_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoor_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x8<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoxor_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoxor_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoxor_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amoxor_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amoxor_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amoxor_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x4<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amomax_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amomax_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amomax_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amomax_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amomax_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amomax_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x14<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amomaxu_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x1c<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amomaxu_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x1c<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amomaxu_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x1c<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amomaxu_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x1c<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amomaxu_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x1c<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amomaxu_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x1c<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amomin_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amomin_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amomin_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amomin_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amomin_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amomin_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x10<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amominu_w_aq:
            asm_emit_a(token, 0x2F | 2<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amominu_w_rl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amominu_w_aqrl:
            asm_emit_a(token, 0x2F | 2<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
        case TOK_ASM_amominu_d_aq:
            asm_emit_a(token, 0x2F | 3<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 1, 0); break;
        case TOK_ASM_amominu_d_rl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 0, 1); break;
        case TOK_ASM_amominu_d_aqrl:
            asm_emit_a(token, 0x2F | 3<<12 | 0x18<<27, &ops[0], &ops[1], &ops[2], 1, 1); break;
    }
}

/* caller: Add funct3 and func5 to opcode */
static void asm_emit_a(int token, uint32_t opcode, const Operand *rd1, const Operand *rs2, const Operand *rs1, int aq, int rl)
{
    if (rd1->type != OP_REG)
        tcc_error("'%s': Expected first destination operand that is a register", get_tok_str(token, NULL));
    if (rs2->type != OP_REG)
        tcc_error("'%s': Expected second source operand that is a register", get_tok_str(token, NULL));
    if (rs1->type != OP_REG)
        tcc_error("'%s': Expected third source operand that is a register", get_tok_str(token, NULL));
        /* A-type instruction:
	        31...27 funct5
	        26      aq
	        25      rl
	        24...20 rs2
	        19...15 rs1
	        14...11 funct3
	        11...7  rd
	        6...0 opcode
        opcode always fixed pos. */
    gen_le32(opcode | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) | ENCODE_RD(rd1->reg) | aq << 26 | rl << 25);
}

/* caller: Add funct3 to opcode */
static void asm_emit_s(int token, uint32_t opcode, const Operand* rs1, const Operand* rs2, const Operand* imm)
{
    if (rs1->type != OP_REG) {
        tcc_error("'%s': Expected first source operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_REG) {
        tcc_error("'%s': Expected second source operand that is a register", get_tok_str(token, NULL));
    }
    if (imm->type != OP_IM12S) {
        tcc_error("'%s': Expected third operand that is an immediate value between 0 and 8191", get_tok_str(token, NULL));
    }
    {
        uint16_t v = imm->e.v;
        /* S-type instruction:
	        31...25 imm[11:5]
	        24...20 rs2
	        19...15 rs1
	        14...12 funct3
	        11...7 imm[4:0]
	        6...0 opcode
        opcode always fixed pos. */
        gen_le32(opcode | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) | ((v & 0x1F) << 7) | ((v >> 5) << 25));
    }
}

static void asm_emit_b(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *imm)
{
    uint32_t offset;

    if (rs1->type != OP_REG) {
        tcc_error("'%s': Expected first source operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_REG) {
        tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
    }
    if (imm->type == OP_SYM) {
        /* a label: resolved here if it is behind us in this section,
           otherwise by the linker (a +-4 KiB range either way) */
        int64_t off = asm_sym_offset(imm, R_RISCV_BRANCH);
        if (off < -0x1000 || off >= 0x1000)
            tcc_error("'%s': branch target out of range", get_tok_str(token, NULL));
        offset = off;
    } else if (imm->type != OP_IM12S) {
        tcc_error("'%s': Expected second source operand that is an immediate value between 0 and 8191", get_tok_str(token, NULL));
    } else {
        offset = imm->e.v;
    }

    /* B-type instruction:
    31      imm[12]
    30...25 imm[10:5]
    24...20 rs2
    19...15 rs1
    14...12 funct3
    8...11  imm[4:1]
    7       imm[11]
    6...0   opcode */
    asm_emit_opcode(opcode | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) | (((offset >> 1) & 0xF) << 8) | (((offset >> 5) & 0x3f) << 25) | (((offset >> 11) & 1) << 7) | (((offset >> 12) & 1) << 31));
}

/* FCVT helper: parse optional rounding mode operand (GNU as syntax:
   fcvt.w.s rd, rs1 [, rtz/rne/rdn/rup/rmm]) */
static int asm_fcvt_rm(TCCState *s1)
{
    int rm = 7; /* dynamic */
    if (tok == ',') {
        next();
        switch (tok) {
            case TOK_ASM_rne: rm = 0; next(); break;
            case TOK_ASM_rtz: rm = 1; next(); break;
            case TOK_ASM_rdn: rm = 2; next(); break;
            case TOK_ASM_rup: rm = 3; next(); break;
            case TOK_ASM_rmm: rm = 4; next(); break;
            case TOK_ASM_dyn: rm = 7; next(); break;
            default: expect("rounding mode"); break;
        }
    }
    return rm;
}

/* fcvt + fclass handler (all are binary with optional rounding operand) */
static void asm_fcvt_opcode(TCCState *s1, int token)
{
    Operand ops[2];
    int rm;
    uint32_t enc = 0;

    parse_operand(s1, &ops[0]);
    skip(',');
    parse_operand(s1, &ops[1]);

    switch (token) {
        case TOK_ASM_fcvt_w_s:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12); break;
        case TOK_ASM_fcvt_wu_s: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_l_s:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_lu_s: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_s_w: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x68 << 25) | (rm << 12); break;
        case TOK_ASM_fcvt_s_wu: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x68 << 25) | (rm << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_s_l: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x68 << 25) | (rm << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_s_lu: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x68 << 25) | (rm << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_w_d:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12); break;
        case TOK_ASM_fcvt_wu_d: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_l_d:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_lu_d: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_d_w: enc = 0x53 | (0x69 << 25) /* exact: rm = 0 */; break;
        case TOK_ASM_fcvt_d_wu: enc = 0x53 | (0x69 << 25) /* exact: rm = 0 */ | (1 << 20); break;
        case TOK_ASM_fcvt_d_l: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x69 << 25) | (rm << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_d_lu: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x69 << 25) | (rm << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_s_d: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x20 << 25) | (rm << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_d_s: enc = 0x53 | (0x21 << 25) /* exact: rm = 0 */; break;
        case TOK_ASM_fclass_s:  enc = 0x53 | (0x70 << 25) | (1 << 12); break;
        case TOK_ASM_fclass_d:  enc = 0x53 | (0x71 << 25) | (1 << 12); break;
        default: expect("fcvt/fclass instruction"); return;
    }
    asm_emit_opcode(enc | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg));
}

ST_FUNC void asm_opcode(TCCState *s1, int token)
{
    /* the c.* tokens are contiguous in riscv64-tok.h */
    if (token >= TOK_ASM_c_nop && token <= TOK_ASM_c_ebreak) {
        asm_c_opcode(s1, token);
        return;
    }
    switch (token) {
    case TOK_ASM_ebreak:
    case TOK_ASM_ecall:
    case TOK_ASM_fence_i:
    case TOK_ASM_hrts:
    case TOK_ASM_mrth:
    case TOK_ASM_mrts:
    case TOK_ASM_wfi:
    case TOK_ASM_sret:
    case TOK_ASM_mret:
    case TOK_ASM_unimp:
        asm_nullary_opcode(s1, token);
        return;

    case TOK_ASM_sfence_vma: {
        /* sfence.vma [vaddr[, asid]] */
        Operand ops[2] = { zero, zero };
        if (tok != TOK_LINEFEED && tok != ';' && tok != TOK_EOF) {
            parse_operand(s1, &ops[0]);
            if (tok == ',') {
                next();
                parse_operand(s1, &ops[1]);
            }
        }
        if (ops[0].type != OP_REG || ops[1].type != OP_REG ||
            REG_IS_FLOAT(ops[0].reg) || REG_IS_FLOAT(ops[1].reg))
            tcc_error("'%s': expected integer registers", get_tok_str(token, NULL));
        asm_emit_opcode((0x1C << 2) | 3 | (0x09 << 25) | ENCODE_RS1(ops[0].reg) | ENCODE_RS2(ops[1].reg));
        return;
    }

    case TOK_ASM_fence:
        asm_fence_opcode(s1, token);
        return;
    case TOK_ASM_fence_tso:
        asm_fence_tso_opcode();
        return;

    case TOK_ASM_rdcycle:
    case TOK_ASM_rdcycleh:
    case TOK_ASM_rdtime:
    case TOK_ASM_rdtimeh:
    case TOK_ASM_rdinstret:
    case TOK_ASM_rdinstreth:
        asm_unary_opcode(s1, token);
        return;

    case TOK_ASM_lui:
    case TOK_ASM_auipc:
    case TOK_ASM_fsqrt_s:
    case TOK_ASM_fsqrt_d:
    case TOK_ASM_fmv_x_w:
    case TOK_ASM_fmv_w_x:
    case TOK_ASM_fmv_x_d:
    case TOK_ASM_fmv_d_x:
        asm_binary_opcode(s1, token);
        return;

    /* fcvt/fclass — separate handler for optional rounding operand */
    case TOK_ASM_fcvt_w_s:
    case TOK_ASM_fcvt_wu_s:
    case TOK_ASM_fcvt_l_s:
    case TOK_ASM_fcvt_lu_s:
    case TOK_ASM_fcvt_s_w:
    case TOK_ASM_fcvt_s_wu:
    case TOK_ASM_fcvt_s_l:
    case TOK_ASM_fcvt_s_lu:
    case TOK_ASM_fcvt_w_d:
    case TOK_ASM_fcvt_wu_d:
    case TOK_ASM_fcvt_l_d:
    case TOK_ASM_fcvt_lu_d:
    case TOK_ASM_fcvt_d_w:
    case TOK_ASM_fcvt_d_wu:
    case TOK_ASM_fcvt_d_l:
    case TOK_ASM_fcvt_d_lu:
    case TOK_ASM_fcvt_s_d:
    case TOK_ASM_fcvt_d_s:
    case TOK_ASM_fclass_s:
    case TOK_ASM_fclass_d:
        asm_fcvt_opcode(s1, token);
        return;

    case TOK_ASM_lb:
    case TOK_ASM_lh:
    case TOK_ASM_lw:
    case TOK_ASM_ld:
    case TOK_ASM_fld:
    case TOK_ASM_lbu:
    case TOK_ASM_lhu:
    case TOK_ASM_lwu:
    case TOK_ASM_sb:
    case TOK_ASM_sh:
    case TOK_ASM_sw:
    case TOK_ASM_sd:
    case TOK_ASM_fsd:
    case TOK_ASM_flw:
    case TOK_ASM_fsw:
        asm_mem_access_opcode(s1, token);
        break;

    case TOK_ASM_jalr:
        asm_jalr_opcode(s1, token); /* it can be a pseudo instruction too*/
        break;
    case TOK_ASM_j:
        asm_jal_opcode(s1, token); /* jal zero, offset*/
        return;
    case TOK_ASM_jal:
        asm_jal_opcode(s1, token); /* it can be a pseudo instruction too*/
        break;

    case TOK_ASM_add:
    case TOK_ASM_addi:
    case TOK_ASM_addiw:
    case TOK_ASM_addw:
    case TOK_ASM_and:
    case TOK_ASM_andi:
    case TOK_ASM_or:
    case TOK_ASM_ori:
    case TOK_ASM_sll:
    case TOK_ASM_slli:
    case TOK_ASM_slliw:
    case TOK_ASM_sllw:
    case TOK_ASM_slt:
    case TOK_ASM_slti:
    case TOK_ASM_sltiu:
    case TOK_ASM_sltu:
    case TOK_ASM_sra:
    case TOK_ASM_srai:
    case TOK_ASM_sraiw:
    case TOK_ASM_sraw:
    case TOK_ASM_srl:
    case TOK_ASM_srli:
    case TOK_ASM_srliw:
    case TOK_ASM_srlw:
    case TOK_ASM_sub:
    case TOK_ASM_subw:
    case TOK_ASM_xor:
    case TOK_ASM_xori:
    /* M extension */
    case TOK_ASM_div:
    case TOK_ASM_divu:
    case TOK_ASM_divuw:
    case TOK_ASM_divw:
    case TOK_ASM_mul:
    case TOK_ASM_mulh:
    case TOK_ASM_mulhsu:
    case TOK_ASM_mulhu:
    case TOK_ASM_mulw:
    case TOK_ASM_rem:
    case TOK_ASM_remu:
    case TOK_ASM_remuw:
    case TOK_ASM_remw:
    /* Zicsr extension */
    case TOK_ASM_csrrc:
    case TOK_ASM_csrrci:
    case TOK_ASM_csrrs:
    case TOK_ASM_csrrsi:
    case TOK_ASM_csrrw:
    case TOK_ASM_csrrwi:
    /* F/D extension */
    case TOK_ASM_fsgnjn_s:
    case TOK_ASM_fsgnjn_d:
    case TOK_ASM_fsgnjx_s:
    case TOK_ASM_fsgnjx_d:
    case TOK_ASM_fadd_s:
    case TOK_ASM_fadd_d:
    case TOK_ASM_fsub_s:
    case TOK_ASM_fsub_d:
    case TOK_ASM_fmul_s:
    case TOK_ASM_fmul_d:
    case TOK_ASM_fdiv_s:
    case TOK_ASM_fdiv_d:
    case TOK_ASM_fsgnj_d:
    case TOK_ASM_fsgnj_s:
    case TOK_ASM_fmax_s:
    case TOK_ASM_fmax_d:
    case TOK_ASM_fmin_s:
    case TOK_ASM_fmin_d:
    case TOK_ASM_feq_s:
    case TOK_ASM_feq_d:
    case TOK_ASM_flt_s:
    case TOK_ASM_flt_d:
    case TOK_ASM_fle_s:
    case TOK_ASM_fle_d:
        asm_ternary_opcode(s1, token);
        return;
    case TOK_ASM_fmadd_d:
    case TOK_ASM_fmadd_s:
    case TOK_ASM_fmsub_s:
    case TOK_ASM_fmsub_d:
    case TOK_ASM_fnmsub_s:
    case TOK_ASM_fnmsub_d:
    case TOK_ASM_fnmadd_s:
    case TOK_ASM_fnmadd_d:
        asm_quaternary_opcode(s1, token);
        return;

    /* Branches */
    case TOK_ASM_beq:
    case TOK_ASM_bge:
    case TOK_ASM_bgeu:
    case TOK_ASM_blt:
    case TOK_ASM_bltu:
    case TOK_ASM_bne:
        asm_branch_opcode(s1, token, 3);
        break;

    /* pseudoinstructions */
    case TOK_ASM_nop:
    case TOK_ASM_ret:
        asm_nullary_opcode(s1, token);
        return;

    case TOK_ASM_jr: {
        /* jalr zero, target */
        Operand rs, imm;
        parse_jump_target(s1, &rs, &imm);
        asm_emit_i(token, 0x67, &zero, &rs, &imm);
        return;
    }
    case TOK_ASM_call:
    case TOK_ASM_tail:
    case TOK_ASM_frflags:
    case TOK_ASM_frrm:
    case TOK_ASM_frcsr:
        asm_unary_opcode(s1, token);
        return;

    case TOK_ASM_la:
    case TOK_ASM_lla:
    case TOK_ASM_li:
    case TOK_ASM_jump:
    case TOK_ASM_seqz:
    case TOK_ASM_snez:
    case TOK_ASM_sltz:
    case TOK_ASM_sgtz:
    case TOK_ASM_mv:
    case TOK_ASM_not:
    case TOK_ASM_neg:
    case TOK_ASM_negw:
    case TOK_ASM_sext_w:
    case TOK_ASM_fabs_s:
    case TOK_ASM_fabs_d:
    case TOK_ASM_fmv_s:
    case TOK_ASM_fmv_d:
    case TOK_ASM_fneg_s:
    case TOK_ASM_fneg_d:
    case TOK_ASM_csrc:
    case TOK_ASM_csrs:
    case TOK_ASM_csrr:
    case TOK_ASM_csrw:
    case TOK_ASM_csrwi:
    case TOK_ASM_csrsi:
    case TOK_ASM_csrci:
        asm_binary_opcode(s1, token);
        return;
    case TOK_ASM_fscsr:
        asm_fcsr_write_opcode(s1, token, 3);
        return;
    case TOK_ASM_fsrm:
        asm_fcsr_write_opcode(s1, token, 2);
        return;
    case TOK_ASM_fsflags:
        asm_fcsr_write_opcode(s1, token, 1);
        return;

    case TOK_ASM_bnez:
    case TOK_ASM_beqz:
    case TOK_ASM_blez:
    case TOK_ASM_bgez:
    case TOK_ASM_bltz:
    case TOK_ASM_bgtz:
        asm_branch_opcode(s1, token, 2);
        return;

    case TOK_ASM_bgt:
    case TOK_ASM_bgtu:
    case TOK_ASM_ble:
    case TOK_ASM_bleu:
        asm_branch_opcode(s1, token, 3);
        return;

    /* Atomic operations */
    case TOK_ASM_lr_w:
    case TOK_ASM_lr_w_aq:
    case TOK_ASM_lr_w_rl:
    case TOK_ASM_lr_w_aqrl:
    case TOK_ASM_lr_d:
    case TOK_ASM_lr_d_aq:
    case TOK_ASM_lr_d_rl:
    case TOK_ASM_lr_d_aqrl:
    case TOK_ASM_sc_w:
    case TOK_ASM_sc_w_aq:
    case TOK_ASM_sc_w_rl:
    case TOK_ASM_sc_w_aqrl:
    case TOK_ASM_sc_d:
    case TOK_ASM_sc_d_aq:
    case TOK_ASM_sc_d_rl:
    case TOK_ASM_sc_d_aqrl:
    /* AMO instructions */
    case TOK_ASM_amoadd_w:
    case TOK_ASM_amoadd_d:
    case TOK_ASM_amoswap_w:
    case TOK_ASM_amoswap_d:
    case TOK_ASM_amoand_w:
    case TOK_ASM_amoand_d:
    case TOK_ASM_amoor_w:
    case TOK_ASM_amoor_d:
    case TOK_ASM_amoxor_w:
    case TOK_ASM_amoxor_d:
    case TOK_ASM_amomax_w:
    case TOK_ASM_amomax_d:
    case TOK_ASM_amomaxu_w:
    case TOK_ASM_amomaxu_d:
    case TOK_ASM_amomin_w:
    case TOK_ASM_amomin_d:
    case TOK_ASM_amominu_w:
    case TOK_ASM_amominu_d:
        
    /* AMO aq/rl */
    case TOK_ASM_amoadd_w_aq:
    case TOK_ASM_amoadd_w_rl:
    case TOK_ASM_amoadd_w_aqrl:
    case TOK_ASM_amoadd_d_aq:
    case TOK_ASM_amoadd_d_rl:
    case TOK_ASM_amoadd_d_aqrl:
    /* AMO aq/rl (all ops) */
    case TOK_ASM_amoswap_w_aq:
    case TOK_ASM_amoswap_w_rl:
    case TOK_ASM_amoswap_w_aqrl:
    case TOK_ASM_amoswap_d_aq:
    case TOK_ASM_amoswap_d_rl:
    case TOK_ASM_amoswap_d_aqrl:
    case TOK_ASM_amoand_w_aq:
    case TOK_ASM_amoand_w_rl:
    case TOK_ASM_amoand_w_aqrl:
    case TOK_ASM_amoand_d_aq:
    case TOK_ASM_amoand_d_rl:
    case TOK_ASM_amoand_d_aqrl:
    case TOK_ASM_amoor_w_aq:
    case TOK_ASM_amoor_w_rl:
    case TOK_ASM_amoor_w_aqrl:
    case TOK_ASM_amoor_d_aq:
    case TOK_ASM_amoor_d_rl:
    case TOK_ASM_amoor_d_aqrl:
    case TOK_ASM_amoxor_w_aq:
    case TOK_ASM_amoxor_w_rl:
    case TOK_ASM_amoxor_w_aqrl:
    case TOK_ASM_amoxor_d_aq:
    case TOK_ASM_amoxor_d_rl:
    case TOK_ASM_amoxor_d_aqrl:
    case TOK_ASM_amomax_w_aq:
    case TOK_ASM_amomax_w_rl:
    case TOK_ASM_amomax_w_aqrl:
    case TOK_ASM_amomax_d_aq:
    case TOK_ASM_amomax_d_rl:
    case TOK_ASM_amomax_d_aqrl:
    case TOK_ASM_amomaxu_w_aq:
    case TOK_ASM_amomaxu_w_rl:
    case TOK_ASM_amomaxu_w_aqrl:
    case TOK_ASM_amomaxu_d_aq:
    case TOK_ASM_amomaxu_d_rl:
    case TOK_ASM_amomaxu_d_aqrl:
    case TOK_ASM_amomin_w_aq:
    case TOK_ASM_amomin_w_rl:
    case TOK_ASM_amomin_w_aqrl:
    case TOK_ASM_amomin_d_aq:
    case TOK_ASM_amomin_d_rl:
    case TOK_ASM_amomin_d_aqrl:
    case TOK_ASM_amominu_w_aq:
    case TOK_ASM_amominu_w_rl:
    case TOK_ASM_amominu_w_aqrl:
    case TOK_ASM_amominu_d_aq:
    case TOK_ASM_amominu_d_rl:
    case TOK_ASM_amominu_d_aqrl:
asm_atomic_opcode(s1, token);
        break;

    default:
        expect("known instruction");
    }
}

static int asm_parse_csrvar(int t)
{
    switch (t) {
    case TOK_ASM_cycle:
        return 0xc00;
    case TOK_ASM_fcsr:
        return 3;
    case TOK_ASM_fflags:
        return 1;
    case TOK_ASM_frm:
        return 2;
    case TOK_ASM_instret:
        return 0xc02;
    case TOK_ASM_time:
        return 0xc01;
    case TOK_ASM_cycleh:
        return 0xc80;
    case TOK_ASM_instreth:
        return 0xc82;
    case TOK_ASM_timeh:
        return 0xc81;
    default:
        if (t >= TOK_IDENT) {
            /* the other standard CSRs, by name */
            static const struct { const char *name; short num; } csrs[] = {
                { "sstatus", 0x100 }, { "sie", 0x104 }, { "stvec", 0x105 },
                { "scounteren", 0x106 }, { "senvcfg", 0x10a }, { "sscratch", 0x140 },
                { "sepc", 0x141 }, { "scause", 0x142 }, { "stval", 0x143 },
                { "sbadaddr", 0x143 }, { "sip", 0x144 }, { "stimecmp", 0x14d },
                { "satp", 0x180 }, { "sptbr", 0x180 },
                { "mstatus", 0x300 }, { "misa", 0x301 }, { "medeleg", 0x302 },
                { "mideleg", 0x303 }, { "mie", 0x304 }, { "mtvec", 0x305 },
                { "mcounteren", 0x306 }, { "menvcfg", 0x30a }, { "mscratch", 0x340 },
                { "mepc", 0x341 }, { "mcause", 0x342 }, { "mtval", 0x343 },
                { "mip", 0x344 }, { "pmpcfg0", 0x3a0 }, { "pmpaddr0", 0x3b0 },
                { "mcycle", 0xb00 }, { "minstret", 0xb02 }, { "mvendorid", 0xf11 },
                { "marchid", 0xf12 }, { "mimpid", 0xf13 }, { "mhartid", 0xf14 },
            };
            const char *name = get_tok_str(t, NULL);
            int i;
            for (i = 0; i < countof(csrs); i++)
                if (!strcmp(name, csrs[i].name))
                    return csrs[i].num;
        }
        return -1;
    }
}

/* The CSR number in operand OP (a name or a constant). */
static uint32_t asm_csr_num(int token, const Operand *op)
{
    if ((op->type != OP_IM12S && op->type != OP_IM32) || op->e.v > 0xfff)
        tcc_error("'%s': expected a CSR name or number", get_tok_str(token, NULL));
    return op->e.v;
}

/* fscsr/fsrm/fsflags [rd,] rs: csrrw rd, CSR, rs (rd defaults to zero) */
static void asm_fcsr_write_opcode(TCCState *s1, int token, int csr)
{
    Operand ops[2];
    parse_operand(s1, &ops[0]);
    if (tok == ',') {
        next();
        parse_operand(s1, &ops[1]);
    } else {
        ops[1] = ops[0];
        ops[0] = zero;
    }
    if (ops[0].type != OP_REG || ops[1].type != OP_REG ||
        REG_IS_FLOAT(ops[0].reg) || REG_IS_FLOAT(ops[1].reg))
        tcc_error("'%s': expected integer registers", get_tok_str(token, NULL));
    asm_emit_opcode(0x73 | (1 << 12) | (csr << 20) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg));
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    int r, reg, val;

    r = sv->r;
    if ((r & VT_VALMASK) == VT_CONST) {
        if (!(r & VT_LVAL) && modifier != 'c' && modifier != 'n' &&
            modifier != 'P') {
            //cstr_ccat(add_str, '#');
        }
        if (r & VT_SYM) {
            const char *name = get_tok_str(sv->sym->v, NULL);
            if (sv->sym->v >= SYM_FIRST_ANOM) {
                /* In case of anonymous symbols ("L.42", used
                   for static data labels) we can't find them
                   in the C symbol table when later looking up
                   this name.  So enter them now into the asm label
                   list when we still know the symbol.  */
                get_asm_sym(tok_alloc(name, strlen(name))->tok, sv->sym);
            }
            if (tcc_state->leading_underscore)
                cstr_ccat(add_str, '_');
            cstr_cat(add_str, name, -1);
            if ((uint32_t) sv->c.i == 0)
                goto no_offset;
            cstr_ccat(add_str, '+');
        }
        val = sv->c.i;
        if (modifier == 'n')
            val = -val;
        if (modifier == 'z' && val == 0) {
            cstr_cat(add_str, "zero", -1);
        } else {
            cstr_printf(add_str, "%d", val);
        }
      no_offset:;
    } else if ((r & VT_VALMASK) == VT_LOCAL) {
        cstr_printf(add_str, "%d", (int) sv->c.i);
    } else if (r & VT_LVAL) {
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            tcc_internal_error("");
        if ((sv->type.t & VT_BTYPE) == VT_FLOAT ||
            (sv->type.t & VT_BTYPE) == VT_DOUBLE) {
            /* floating point register */
            reg = TOK_ASM_f0 + REG_VALUE(reg);
        } else {
            /* general purpose register */
            reg = TOK_ASM_x0 + reg;
        }
        cstr_cat(add_str, get_tok_str(reg, NULL), -1);
    } else {
        /* register case */
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            tcc_internal_error("");
        if ((sv->type.t & VT_BTYPE) == VT_FLOAT ||
            (sv->type.t & VT_BTYPE) == VT_DOUBLE) {
            /* floating point register */
            reg = TOK_ASM_f0 + REG_VALUE(reg);
        } else {
            /* general purpose register */
            reg = TOK_ASM_x0 + reg;
        }
        cstr_cat(add_str, get_tok_str(reg, NULL), -1);
    }
}

/* TCC does not use RISC-V register numbers internally, it uses 0-8 for
 * integers and 8-16 for floats instead */
static int tcc_ireg(int r){
    return REG_VALUE(r) - 10;
}
static int tcc_freg(int r){
    return REG_VALUE(r) - 10 + 8;
}

/* Move an asm operand between memory and hardware register REG (integer
   0-31, float 32-63). tcc's code generator only has a0-a7 and fa0-fa7;
   another register (a register variable's, like s1) is loaded or stored
   directly from the variable's stack slot. */
static void asm_move_operand(int reg, SValue *sv, int is_store)
{
    int fp = REG_IS_FLOAT(reg), size, align, f3;
    int64_t off = (int64_t)sv->c.i;
    if (REG_VALUE(reg) >= 10 && REG_VALUE(reg) <= 17) {
        int r = fp ? tcc_freg(reg) : tcc_ireg(reg);
        if (is_store)
            store(r, sv);
        else
            load(r, sv);
        return;
    }
    size = type_size(&sv->type, &align);
    if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_LOCAL | VT_LVAL)
        || off < -2048 || off > 2047 || (size & (size - 1)) || size > 8
        || (fp && size < 4) || REG_VALUE(reg) == 8)
        tcc_error("asm operand in %s%d: only a local variable (not in s0) can be used",
                  fp ? "f" : "x", REG_VALUE(reg));
    f3 = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    if (!is_store && !fp && size < 4 && (sv->type.t & VT_UNSIGNED))
        f3 |= 4;                                /* lbu, lhu */
    if (is_store)   /* s{b,h,w,d} / fs{w,d} reg, off(s0) */
        gen_le32((fp ? 0x27 : 0x23) | (f3 << 12) | ENCODE_RS1(8) | ENCODE_RS2(reg)
                 | ((off & 0x1f) << 7) | ((off >> 5 & 0x7f) << 25));
    else            /* l{b,h,w,d}[u] / fl{w,d} reg, off(s0) */
        gen_le32((fp ? 0x07 : 0x03) | (f3 << 12) | ENCODE_RD(reg) | ENCODE_RS1(8)
                 | ((off & 0xfff) << 20));
}

/* generate prolog and epilog code for asm statement */
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands,
                         int nb_outputs, int is_output,
                         uint8_t *clobber_regs,
                         int out_reg)
{
    uint8_t regs_allocated[NB_ASM_REGS];
    ASMOperand *op;
    int i, reg;

    static const uint8_t reg_saved[] = {
        // General purpose regs
        8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
        // Float regs
        40, 41, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59
    };

    /* mark all used registers */
    memcpy(regs_allocated, clobber_regs, sizeof(regs_allocated));
    for(i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0) {
            regs_allocated[op->reg] = 1;
        }
    }

    if(!is_output) {
        /* generate reg save code */
        for(i = 0; i < sizeof(reg_saved)/sizeof(reg_saved[0]); i++) {
            reg = reg_saved[i];
            if (regs_allocated[reg]) {
                /* push */
                /* addi sp, sp, -offset */
                gen_le32((4 << 2) | 3 |
                        ENCODE_RD(2) | ENCODE_RS1(2) | (unsigned)-8 << 20);
                if (REG_IS_FLOAT(reg)){
                    /* fsd reg, offset(sp) */
                    gen_le32( 0x27 | (3 << 12) |
                            ENCODE_RS2(reg) | ENCODE_RS1(2) );
                } else {
                    /* sd reg, offset(sp) */
                    gen_le32((0x8 << 2) | 3 | (3 << 12) |
                            ENCODE_RS2(reg) | ENCODE_RS1(2) );
                }
            }
        }

        /* generate load code */
        for(i = 0; i < nb_operands; i++) {
            op = &operands[i];
            if (op->reg >= 0) {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL &&
                    op->is_memory) {
                    /* memory reference case (for both input and
                       output cases) */
                    SValue sv;
                    sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
                    sv.type.t = VT_PTR;
                    load(tcc_ireg(op->reg), &sv);
                } else if (i >= nb_outputs || op->is_rw) {
                    /* load value in register */
                    asm_move_operand(op->reg, op->vt, 0);
                    /* RV64: long long fits in a single 64-bit register;
                       the load/store above already handles it correctly */
                }
            }
        }
    } else {
        /* generate save code */
        for(i = 0 ; i < nb_outputs; i++) {
            op = &operands[i];
            if (op->reg >= 0) {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                    if (!op->is_memory) {
                        SValue sv;
                        sv = *op->vt;
                        sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
                        sv.type.t = VT_PTR;
                        load(tcc_ireg(out_reg), &sv);

                        sv = *op->vt;
                        sv.r = (sv.r & ~VT_VALMASK) | out_reg;
                        store(tcc_ireg(op->reg), &sv);
                    }
                } else {
                    asm_move_operand(op->reg, op->vt, 1);
                    /* RV64: long long fits in a single 64-bit register */
                }
            }
        }
        /* generate reg restore code for floating point registers */
        for(i = sizeof(reg_saved)/sizeof(reg_saved[0]) - 1; i >= 0; i--) {
            reg = reg_saved[i];
            if (regs_allocated[reg]) {
                /* pop */
                if (REG_IS_FLOAT(reg)){
                    /* fld reg, offset(sp) */
                    gen_le32(7 | (3 << 12) |
                            ENCODE_RD(reg) | ENCODE_RS1(2) | 0);
                } else {
                    /* ld reg, offset(sp) */
                    gen_le32(3 | (3 << 12) |
                            ENCODE_RD(reg) | ENCODE_RS1(2) | 0);
                }
                /* addi sp, sp, offset */
                gen_le32((4 << 2) | 3 |
                        ENCODE_RD(2) | ENCODE_RS1(2) | 8 << 20);
            }
        }
    }
}

/* return the constraint priority (we allocate first the lowest
   numbered constraints) */
static inline int constraint_priority(const char *str)
{
    // TODO: How is this chosen??
    int priority, c, pr;

    /* we take the lowest priority */
    priority = 0;
    for(;;) {
        c = *str;
        if (c == '\0')
            break;
        str++;
        switch(c) {
        case 'A': // address that is held in a general-purpose register.
        case 'S': // constraint that matches an absolute symbolic address.
        case 'f': // register [float]
        case 'r': // register [general]
        case 'p': // valid memory address for load,store [general]
            pr = 3;
            break;
        case 'I': // 12 bit signed immedate
        case 'K': // 5 bit unsigned immediate (csrrwi, csrsi, ...)
        case 'i': // immediate integer operand, including symbolic constants [general]
        case 'm': // memory operand [general]
        case 'g': // general-purpose-register, memory, immediate integer [general]
            pr = 4;
            break;
        case 'v':
            tcc_error("unimp: constraint '%c'", c);
        default:
            tcc_error("unknown constraint '%d'", c);
        }
        if (pr > priority)
            priority = pr;
    }
    return priority;
}

static const char *skip_constraint_modifiers(const char *p)
{
    /* Constraint modifier:
        =   Operand is written to by this instruction
        +   Operand is both read and written to by this instruction
        %   Instruction is commutative for this operand and the following operand.

       Per-alternative constraint modifier:
        &   Operand is clobbered before the instruction is done using the input operands
    */
    while (*p == '=' || *p == '&' || *p == '+' || *p == '%')
        p++;
    return p;
}

#define REG_OUT_MASK 0x01
#define REG_IN_MASK  0x02

#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)

ST_FUNC void asm_compute_constraints(ASMOperand *operands,
                                    int nb_operands, int nb_outputs,
                                    const uint8_t *clobber_regs,
                                    int *pout_reg)
{
    /* TODO: Simple constraints
        whitespace  ignored
        o  memory operand that is offsetable
        V  memory but not offsetable
        <  memory operand with autodecrement addressing is allowed.  Restrictions apply.
        >  memory operand with autoincrement addressing is allowed.  Restrictions apply.
        n  immediate integer operand with a known numeric value
        E  immediate floating operand (const_double) is allowed, but only if target=host
        F  immediate floating operand (const_double or const_vector) is allowed
        s  immediate integer operand whose value is not an explicit integer
        X  any operand whatsoever
        0...9 (postfix); (can also be more than 1 digit number);  an operand that matches the specified operand number is allowed
    */

    /* TODO: RISCV constraints
        J   The integer 0.
        K   A 5-bit unsigned immediate for CSR access instructions.
        A   An address that is held in a general-purpose register.
        S   A constraint that matches an absolute symbolic address.
        vr  A vector register (if available)..
        vd  A vector register, excluding v0 (if available).
        vm  A vector register, only v0 (if available).
    */
    ASMOperand *op;
    int sorted_op[MAX_ASM_OPERANDS];
    int i, j, k, p1, p2, tmp, reg, c, reg_mask;
    const char *str;
    uint8_t regs_allocated[NB_ASM_REGS];

    /* init fields */
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        op->input_index = -1;
        op->ref_index = -1;
        op->reg = -1;
        op->is_memory = 0;
        op->is_rw = 0;
    }
    /* compute constraint priority and evaluate references to output
       constraints if input constraints */
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        str = op->constraint;
        str = skip_constraint_modifiers(str);
        if (isnum(*str) || *str == '[') {
            /* this is a reference to another constraint */
            k = find_constraint(operands, nb_operands, str, NULL);
            if ((unsigned) k >= i || i < nb_outputs)
                tcc_error("invalid reference in constraint %d ('%s')",
                          i, str);
            op->ref_index = k;
            if (operands[k].input_index >= 0)
                tcc_error("cannot reference twice the same operand");
            operands[k].input_index = i;
            op->priority = 5;
        } else if ((op->vt->r & VT_VALMASK) == VT_LOCAL
                   && op->vt->sym && op->vt->sym->a.regvar) {
            /* a register variable: its register */
            op->priority = 1;
            op->reg = asm_parse_regvar(op->vt->sym->asm_label);
        } else {
            op->priority = constraint_priority(str);
        }
    }

    /* sort operands according to their priority */
    for (i = 0; i < nb_operands; i++)
        sorted_op[i] = i;
    for (i = 0; i < nb_operands - 1; i++) {
        for (j = i + 1; j < nb_operands; j++) {
            p1 = operands[sorted_op[i]].priority;
            p2 = operands[sorted_op[j]].priority;
            if (p2 < p1) {
                tmp = sorted_op[i];
                sorted_op[i] = sorted_op[j];
                sorted_op[j] = tmp;
            }
        }
    }

    for (i = 0; i < NB_ASM_REGS; i++) {
        if (clobber_regs[i])
            regs_allocated[i] = REG_IN_MASK | REG_OUT_MASK;
        else
            regs_allocated[i] = 0;
    }

    /* allocate registers and generate corresponding asm moves */
    for (i = 0; i < nb_operands; i++) {
        j = sorted_op[i];
        op = &operands[j];
        str = op->constraint;
        /* no need to allocate references */
        if (op->ref_index >= 0)
            continue;
        /* select if register is used for output, input or both */
        if (op->input_index >= 0) {
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
        } else if (j < nb_outputs) {
            reg_mask = REG_OUT_MASK;
        } else {
            reg_mask = REG_IN_MASK;
        }
        if (op->reg >= 0) {
            if (is_reg_allocated(op->reg))
                tcc_error
                    ("asm regvar requests register that's taken already");
            reg = op->reg;
        }
      try_next:
        c = *str++;
        switch (c) {
        case '=': // Operand is written-to
            goto try_next;
        case '+': // Operand is both READ and written-to
            op->is_rw = 1;
            /* FALL THRU */
        case '&': // Operand is clobbered before the instruction is done using the input operands
            if (j >= nb_outputs)
                tcc_error("'%c' modifier can only be applied to outputs", c);
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
            goto try_next;
        case 'r': // general-purpose register
        case 'p': // loadable/storable address
            /* any general register */
            /* From a0 to a7 */
            if ((reg = op->reg) >= 0)
                goto reg_found;
            else for (reg = 10; reg <= 17; reg++) {
                if (!is_reg_allocated(reg))
                    goto reg_found;
            }
            goto try_next;
          reg_found:
            /* now we can reload in the register */
            op->is_llong = 0;
            op->reg = reg;
            regs_allocated[reg] |= reg_mask;
            break;
        case 'f': // floating pont register
            /* floating point register */
            /* From fa0 to fa7 */
            if ((reg = op->reg) >= 0)
                goto reg_found;
            else for (reg = 42; reg <= 49; reg++) {
                if (!is_reg_allocated(reg))
                    goto reg_found;
            }
            goto try_next;
        case 'I': // I-Type 12 bit signed immediate
        case 'i': // immediate integer operand, including symbolic constants
            if (!((op->vt->r & (VT_VALMASK | VT_LVAL)) == VT_CONST))
                goto try_next;
            break;
        case 'K': // 5 bit unsigned immediate
            if (!((op->vt->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
                || (uint64_t)op->vt->c.i > 31)
                goto try_next;
            break;
        case 'm': // memory operand
        case 'g': // any register
            /* nothing special to do because the operand is already in
               memory, except if the pointer itself is stored in a
               memory variable (VT_LLOCAL case) */
            /* XXX: fix constant case */
            /* if it is a reference to a memory zone, it must lie
               in a register, so we reserve the register in the
               input registers and a load will be generated
               later */
            if (j < nb_outputs || c == 'm') {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                    /* any general register: from a0 to a7 */
                    for (reg = 10; reg <= 17; reg++) {
                        if (!(regs_allocated[reg] & REG_IN_MASK))
                            goto reg_found1;
                    }
                    goto try_next;
                  reg_found1:
                    /* now we can reload in the register */
                    regs_allocated[reg] |= REG_IN_MASK;
                    op->reg = reg;
                    op->is_memory = 1;
                }
            }
            break;
        default:
            tcc_error("asm constraint %d ('%s') could not be satisfied",
                      j, op->constraint);
            break;
        }
        /* if a reference is present for that operand, we assign it too */
        if (op->input_index >= 0) {
            operands[op->input_index].reg = op->reg;
            operands[op->input_index].is_llong = op->is_llong;
        }
    }

    /* compute out_reg. It is used to store outputs registers to memory
       locations references by pointers (VT_LLOCAL case) */
    *pout_reg = -1;
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0 &&
            (op->vt->r & VT_VALMASK) == VT_LLOCAL && !op->is_memory) {
            if (REG_IS_FLOAT(op->reg)){
                /* From fa0 to fa7 */
                for (reg = 42; reg <= 50; reg++) {
                    if (!(regs_allocated[reg] & REG_OUT_MASK))
                        goto reg_found2;
                }
            } else {
                /* From a0 to a7 */
                for (reg = 10; reg <= 18; reg++) {
                    if (!(regs_allocated[reg] & REG_OUT_MASK))
                        goto reg_found2;
                }
            }
            tcc_error("could not find free output register for reloading");
          reg_found2:
            *pout_reg = reg;
            break;
        }
    }

    /* print sorted constraints */
#ifdef ASM_DEBUG
    for (i = 0; i < nb_operands; i++) {
        j = sorted_op[i];
        op = &operands[j];
        printf("%%%d [%s]: \"%s\" r=0x%04x reg=%d\n",
               j,
               op->id ? get_tok_str(op->id, NULL) : "",
               op->constraint, op->vt->r, op->reg);
    }
    if (*pout_reg >= 0)
        printf("out_reg=%d\n", *pout_reg);
#endif
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    int reg;
    TokenSym *ts;

    if (!strcmp(str, "memory") ||
        !strcmp(str, "cc") ||
        !strcmp(str, "flags"))
        return;
    ts = tok_alloc(str, strlen(str));
    reg = asm_parse_regvar(ts->tok);
    if (reg == -1) {
        tcc_error("invalid clobber register '%s'", str);
    }
    clobber_regs[reg] = 1;
}

ST_FUNC int asm_parse_regvar (int t)
{
    if (t == TOK_ASM_fp)
        return 8;
    /* PC register not implemented */
    if (t >= TOK_ASM_pc || t < TOK_ASM_x0)
        return -1;

    if (t < TOK_ASM_f0)
        return t - TOK_ASM_x0;

    if (t < TOK_ASM_zero)
        return t - TOK_ASM_f0 + 32; // Use higher 32 for floating point

    /* ABI mnemonic */
    if (t < TOK_ASM_ft0)
        return t - TOK_ASM_zero;

    return t - TOK_ASM_ft0 + 32; // Use higher 32 for floating point
}

/*************************************************************/
/* C extension (RV64C), in gas syntax: c.lw rd', off(rs1'), c.lwsp rd,
   off(sp), c.addi4spn rd', sp, imm, c.beqz rs1', label, ... Registers
   written r' must be x8-x15 (f8-f15 for c.fld/c.fsd). The RV32-only
   c.flw, c.fsw, c.flwsp, c.fswsp and c.jal share encodings with RV64's
   c.ld, c.sd, c.ldsp, c.sdsp and c.addiw, so they are rejected. */

static void c_error(int token, const char *what)
{
    tcc_error("'%s': %s", get_tok_str(token, NULL), what);
}

/* a full register number (0-31), integer or (fp) float */
static int c_reg(int token, const Operand *op, int fp)
{
    if (op->type != OP_REG || !REG_IS_FLOAT(op->reg) != !fp)
        c_error(token, fp ? "expected a floating-point register" : "expected an integer register");
    return REG_VALUE(op->reg);
}

/* a register x8-x15 (f8-f15) as its 3-bit number */
static int c_reg3(int token, const Operand *op, int fp)
{
    int r = c_reg(token, op, fp);
    if (r < 8 || r > 15)
        c_error(token, fp ? "expected a register f8-f15" : "expected a register x8-x15 (s0, s1, a0-a5)");
    return r - 8;
}

/* an immediate in [lo, hi], a multiple of align */
static int64_t c_imm(int token, const Operand *op, int64_t lo, int64_t hi, int align)
{
    int64_t v;
    if (op->type != OP_IM12S && op->type != OP_IM32)
        c_error(token, "expected an immediate value");
    v = (int64_t)op->e.v;
    if (v < lo || v > hi || (v & (align - 1)))
        tcc_error("'%s': immediate %lld not in [%lld, %lld]%s", get_tok_str(token, NULL),
                  (long long)v, (long long)lo, (long long)hi,
                  align > 1 ? (align == 2 ? " or odd" : " or misaligned") : "");
    return v;
}

/* a branch or jump target: a label (resolved here or by the linker) or an
   offset */
static int64_t c_target(int token, const Operand *op, int reloc, int64_t range)
{
    int64_t v;
    if (op->type == OP_SYM)
        v = asm_sym_offset(op, reloc);
    else if (op->type == OP_IM12S || op->type == OP_IM32)
        v = (int64_t)op->e.v;
    else
        c_error(token, "expected a label or offset");
    if (v < -range || v >= range || (v & 1))
        c_error(token, "branch target out of range");
    return v;
}

/* "rd, off(base)" or "rd, (base)" */
static void c_parse_mem(TCCState *s1, Operand *r, Operand *base, Operand *off)
{
    parse_operand(s1, r);
    skip(',');
    if (tok == '(')
        *off = zimm;
    else
        parse_operand(s1, off);
    skip('(');
    parse_operand(s1, base);
    skip(')');
}

static void asm_c_opcode(TCCState *s1, int token)
{
    Operand ops[3];
    int64_t v;
    int rd, rs, f3;

    switch (token) {
    case TOK_ASM_c_nop:
        gen_le16(0x0001);
        return;
    case TOK_ASM_c_ebreak:
        gen_le16(0x9002);
        return;

    /* quadrant 0 */
    case TOK_ASM_c_addi4spn:
        parse_operands(s1, ops, 3);
        rd = c_reg3(token, &ops[0], 0);
        if (c_reg(token, &ops[1], 0) != 2)
            c_error(token, "expected sp as the second operand");
        v = c_imm(token, &ops[2], 4, 1020, 4);
        gen_le16((((v >> 4) & 3) << 11) | (((v >> 6) & 15) << 7) | (((v >> 2) & 1) << 6)
                 | (((v >> 3) & 1) << 5) | (rd << 2));
        return;
    case TOK_ASM_c_lw:
    case TOK_ASM_c_sw:
        c_parse_mem(s1, &ops[0], &ops[1], &ops[2]);
        rd = c_reg3(token, &ops[0], 0);
        rs = c_reg3(token, &ops[1], 0);
        v = c_imm(token, &ops[2], 0, 124, 4);
        gen_le16(((token == TOK_ASM_c_lw ? 2 : 6) << 13) | (((v >> 3) & 7) << 10) | (rs << 7)
                 | (((v >> 2) & 1) << 6) | (((v >> 6) & 1) << 5) | (rd << 2));
        return;
    case TOK_ASM_c_ld:
    case TOK_ASM_c_sd:
    case TOK_ASM_c_fld:
    case TOK_ASM_c_fsd:
        c_parse_mem(s1, &ops[0], &ops[1], &ops[2]);
        rd = c_reg3(token, &ops[0], token == TOK_ASM_c_fld || token == TOK_ASM_c_fsd);
        rs = c_reg3(token, &ops[1], 0);
        v = c_imm(token, &ops[2], 0, 248, 8);
        f3 = token == TOK_ASM_c_fld ? 1 : token == TOK_ASM_c_ld ? 3 : token == TOK_ASM_c_fsd ? 5 : 7;
        gen_le16((f3 << 13) | (((v >> 3) & 7) << 10) | (rs << 7) | (((v >> 6) & 3) << 5) | (rd << 2));
        return;

    /* quadrant 1 */
    case TOK_ASM_c_addi:
    case TOK_ASM_c_addiw:
    case TOK_ASM_c_li:
        parse_operands(s1, ops, 2);
        rd = c_reg(token, &ops[0], 0);
        if (rd == 0 && token != TOK_ASM_c_addi)
            c_error(token, "the destination cannot be zero");
        v = c_imm(token, &ops[1], -32, 31, 1);
        f3 = token == TOK_ASM_c_addi ? 0 : token == TOK_ASM_c_addiw ? 1 : 2;
        gen_le16((f3 << 13) | (((v >> 5) & 1) << 12) | (rd << 7) | ((v & 31) << 2) | 1);
        return;
    case TOK_ASM_c_addi16sp:
        parse_operands(s1, ops, 2);
        if (c_reg(token, &ops[0], 0) != 2)
            c_error(token, "expected sp as the first operand");
        v = c_imm(token, &ops[1], -512, 496, 16);
        if (!v)
            c_error(token, "the immediate cannot be zero");
        gen_le16((3 << 13) | (((v >> 9) & 1) << 12) | (2 << 7) | (((v >> 4) & 1) << 6)
                 | (((v >> 6) & 1) << 5) | (((v >> 7) & 3) << 3) | (((v >> 5) & 1) << 2) | 1);
        return;
    case TOK_ASM_c_lui:
        /* the operand is the upper 20 bits, like lui: 1-31 or 0xfffe0-0xfffff */
        parse_operands(s1, ops, 2);
        rd = c_reg(token, &ops[0], 0);
        if (rd == 0 || rd == 2)
            c_error(token, "the destination cannot be zero or sp");
        v = c_imm(token, &ops[1], 1, 0xfffff, 1);
        if (v > 31 && v < 0xfffe0)
            c_error(token, "immediate not in [1, 31] or [0xfffe0, 0xfffff]");
        gen_le16((3 << 13) | (((v >> 5) & 1) << 12) | (rd << 7) | ((v & 31) << 2) | 1);
        return;
    case TOK_ASM_c_srli:
    case TOK_ASM_c_srai:
    case TOK_ASM_c_andi:
        parse_operands(s1, ops, 2);
        rd = c_reg3(token, &ops[0], 0);
        if (token == TOK_ASM_c_andi)
            v = c_imm(token, &ops[1], -32, 31, 1);
        else
            v = c_imm(token, &ops[1], 1, 63, 1);
        f3 = token == TOK_ASM_c_srli ? 0 : token == TOK_ASM_c_srai ? 1 : 2;
        gen_le16((4 << 13) | (((v >> 5) & 1) << 12) | (f3 << 10) | (rd << 7) | ((v & 31) << 2) | 1);
        return;
    case TOK_ASM_c_sub:
    case TOK_ASM_c_xor:
    case TOK_ASM_c_or:
    case TOK_ASM_c_and:
    case TOK_ASM_c_subw:
    case TOK_ASM_c_addw:
        parse_operands(s1, ops, 2);
        rd = c_reg3(token, &ops[0], 0);
        rs = c_reg3(token, &ops[1], 0);
        f3 = token == TOK_ASM_c_sub || token == TOK_ASM_c_subw ? 0 : token == TOK_ASM_c_xor
             || token == TOK_ASM_c_addw ? 1 : token == TOK_ASM_c_or ? 2 : 3;
        gen_le16((4 << 13) | ((token == TOK_ASM_c_subw || token == TOK_ASM_c_addw) << 12)
                 | (3 << 10) | (rd << 7) | (f3 << 5) | (rs << 2) | 1);
        return;
    case TOK_ASM_c_j:
        parse_branch_offset_operand(s1, &ops[0]);
        v = c_target(token, &ops[0], R_RISCV_RVC_JUMP, 2048);
        gen_le16((5 << 13) | (((v >> 11) & 1) << 12) | (((v >> 4) & 1) << 11) | (((v >> 8) & 3) << 9)
                 | (((v >> 10) & 1) << 8) | (((v >> 6) & 1) << 7) | (((v >> 7) & 1) << 6)
                 | (((v >> 1) & 7) << 3) | (((v >> 5) & 1) << 2) | 1);
        return;
    case TOK_ASM_c_beqz:
    case TOK_ASM_c_bnez:
        parse_operand(s1, &ops[0]);
        skip(',');
        parse_branch_offset_operand(s1, &ops[1]);
        rs = c_reg3(token, &ops[0], 0);
        v = c_target(token, &ops[1], R_RISCV_RVC_BRANCH, 256);
        gen_le16(((token == TOK_ASM_c_beqz ? 6 : 7) << 13) | (((v >> 8) & 1) << 12) | (((v >> 3) & 3) << 10)
                 | (rs << 7) | (((v >> 6) & 3) << 5) | (((v >> 1) & 3) << 3) | (((v >> 5) & 1) << 2) | 1);
        return;

    /* quadrant 2 */
    case TOK_ASM_c_slli:
        parse_operands(s1, ops, 2);
        rd = c_reg(token, &ops[0], 0);
        if (rd == 0)
            c_error(token, "the destination cannot be zero");
        v = c_imm(token, &ops[1], 1, 63, 1);
        gen_le16((((v >> 5) & 1) << 12) | (rd << 7) | ((v & 31) << 2) | 2);
        return;
    case TOK_ASM_c_lwsp:
        c_parse_mem(s1, &ops[0], &ops[1], &ops[2]);
        rd = c_reg(token, &ops[0], 0);
        if (rd == 0 || c_reg(token, &ops[1], 0) != 2)
            c_error(token, "expected rd, off(sp) with rd not zero");
        v = c_imm(token, &ops[2], 0, 252, 4);
        gen_le16((2 << 13) | (((v >> 5) & 1) << 12) | (rd << 7) | (((v >> 2) & 7) << 4) | (((v >> 6) & 3) << 2) | 2);
        return;
    case TOK_ASM_c_ldsp:
    case TOK_ASM_c_fldsp:
        c_parse_mem(s1, &ops[0], &ops[1], &ops[2]);
        rd = c_reg(token, &ops[0], token == TOK_ASM_c_fldsp);
        if ((rd == 0 && token == TOK_ASM_c_ldsp) || c_reg(token, &ops[1], 0) != 2)
            c_error(token, "expected rd, off(sp) with rd not zero");
        v = c_imm(token, &ops[2], 0, 504, 8);
        gen_le16(((token == TOK_ASM_c_fldsp ? 1 : 3) << 13) | (((v >> 5) & 1) << 12) | (rd << 7)
                 | (((v >> 3) & 3) << 5) | (((v >> 6) & 7) << 2) | 2);
        return;
    case TOK_ASM_c_swsp:
        c_parse_mem(s1, &ops[0], &ops[1], &ops[2]);
        rs = c_reg(token, &ops[0], 0);
        if (c_reg(token, &ops[1], 0) != 2)
            c_error(token, "expected rs2, off(sp)");
        v = c_imm(token, &ops[2], 0, 252, 4);
        gen_le16((6 << 13) | (((v >> 2) & 15) << 9) | (((v >> 6) & 3) << 7) | (rs << 2) | 2);
        return;
    case TOK_ASM_c_sdsp:
    case TOK_ASM_c_fsdsp:
        c_parse_mem(s1, &ops[0], &ops[1], &ops[2]);
        rs = c_reg(token, &ops[0], token == TOK_ASM_c_fsdsp);
        if (c_reg(token, &ops[1], 0) != 2)
            c_error(token, "expected rs2, off(sp)");
        v = c_imm(token, &ops[2], 0, 504, 8);
        gen_le16(((token == TOK_ASM_c_fsdsp ? 5 : 7) << 13) | (((v >> 3) & 7) << 10) | (((v >> 6) & 7) << 7)
                 | (rs << 2) | 2);
        return;
    case TOK_ASM_c_jr:
    case TOK_ASM_c_jalr:
        parse_operand(s1, &ops[0]);
        rs = c_reg(token, &ops[0], 0);
        if (rs == 0)
            c_error(token, "the register cannot be zero");
        gen_le16((4 << 13) | ((token == TOK_ASM_c_jalr) << 12) | (rs << 7) | 2);
        return;
    case TOK_ASM_c_mv:
    case TOK_ASM_c_add:
        parse_operands(s1, ops, 2);
        rd = c_reg(token, &ops[0], 0);
        rs = c_reg(token, &ops[1], 0);
        if (rd == 0 || rs == 0)
            c_error(token, "the registers cannot be zero");
        gen_le16((4 << 13) | ((token == TOK_ASM_c_add) << 12) | (rd << 7) | (rs << 2) | 2);
        return;

    default:
        c_error(token, "not an RV64C instruction");
    }
}

/*************************************************************/
#endif /* ndef TARGET_DEFS_ONLY */
