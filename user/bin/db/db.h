/*
 * db: a source-level debugger for Eunuchs programs, in the spirit of gdb.
 *
 *   elf.c     the program file: sections and the symbol table
 *   dwarf.c   DWARF 4/5: line tables, scopes, variables, types, locations
 *   target.c  the debugged process through ptrace: run, attach, registers,
 *             memory, breakpoints, software single-step, the stack
 *   expr.c    C expressions: parse, evaluate against the stopped process, print
 *   disas.c   a RISC-V (RV64GC) disassembler
 *   main.c    the command loop and the commands
 */
#ifndef DB_H
#define DB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int8_t i8;

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* prints; no exit */

/* ---- elf.c ---- */

struct sym {
    char *name;
    u64 addr, size;
    int is_func;
};

struct section {
    const char *name;
    const u8 *data;
    u64 size;
    u64 addr;
};

struct program {
    char *path;
    u8 *image;                  /* the whole file */
    size_t image_size;
    u64 entry;
    struct sym *syms;           /* sorted by address */
    int nsyms;
    struct section debug_info, debug_abbrev, debug_line, debug_str, debug_line_str, debug_loclists,
        debug_loc, debug_ranges, debug_rnglists, debug_addr, debug_str_offsets;
    u64 text_start, text_end;
};

extern struct program prog;
int elf_load(const char *path);                         /* 0 or -1 (message printed) */
struct sym *sym_at(u64 addr);                           /* the function or object containing addr */
struct sym *sym_named(const char *name);

/* ---- dwarf.c ---- */

struct line_row {
    u64 addr;
    int file;                   /* index into files[] */
    int line;
    u8 is_stmt, end_seq;
};

struct src_file {
    char *path;                 /* as recorded (directory joined) */
    char *base;                 /* the name after the last / */
    char **lines;               /* loaded on demand */
    int nlines;
    int loaded;
};

extern struct line_row *rows;
extern int nrows;
extern struct src_file *files;
extern int nfiles;

int dwarf_load(void);                                   /* after elf_load; 0 if no DWARF */
struct line_row *line_for(u64 addr);                    /* the row covering addr, or NULL */
u64 addr_for_line(int file, int line, int *actual_line); /* first statement at or after line */
int find_file(const char *name);                        /* by path, base name or suffix */
const char *src_line(int file, int line);               /* text of a source line, or NULL */

/* Types, built from DWARF type DIEs. */
enum tkind { T_VOID, T_INT, T_FLOAT, T_BOOL, T_PTR, T_ARRAY, T_STRUCT, T_UNION, T_ENUM, T_FUNC };

struct member {
    char *name;
    struct type *type;
    u64 offset;
    int bit_size, bit_offset;   /* bit fields (0: not one) */
};

struct enumerator {
    char *name;
    i64 value;
};

struct type {
    enum tkind kind;
    char *name;                 /* int, struct foo, a typedef name, or NULL */
    u64 size;
    int is_signed;
    int is_char;
    struct type *target;        /* pointer, array element, function return, typedef */
    u64 count;                  /* array elements (0: unknown) */
    struct member *members;
    int nmembers;
    struct enumerator *enums;
    int nenums;
};

struct type *type_at(u64 die_offset);                   /* the type a DIE describes */
struct type *type_int(void);                            /* int */
struct type *type_long(void);                           /* long */
struct type *type_ulong(void);
struct type *type_char(void);
struct type *type_double(void);
struct type *type_pointer_to(struct type *t);
const char *type_name(struct type *t, char *buf, size_t size);

/* A variable: where it lives at a pc. */
struct var {
    char *name;
    struct type *type;
    const u8 *loc;              /* location expression (NULL: see loclist / const) */
    size_t loc_len;
    u64 loclist;                /* offset in .debug_loclists/.debug_loc, or ~0 */
    int loclist_form5;          /* DWARF 5 list format */
    int has_const;
    i64 const_value;
    int is_param;
    u64 cu_low_pc;              /* base address for location lists */
    int cu_index;
};

struct func {
    char *name;
    u64 low, high;
    const u8 *frame_base;       /* expression */
    size_t frame_base_len;
    int cu_index;
    u64 die;                    /* .debug_info offset of the subprogram */
    u64 ret_die;                /* its return type (0: void) */
};

struct func *func_at(u64 pc);                           /* the DWARF function containing pc */
struct func *func_named(const char *name);
/* The variables visible at pc, innermost first (params and locals of the
 * function, then the CU's globals, then all external globals). Returns a
 * count; *out is a static array valid until the next call. */
int vars_at(u64 pc, struct var **out, int locals_only);
struct var *var_lookup(u64 pc, const char *name);

/* ---- target.c ---- */

struct regs {
    u64 pc;
    u64 x[32];                  /* x[0] is 0 */
};

extern pid_t pid;               /* the debugged process, or 0 */
extern int attached;            /* attached with -p (detach, not kill, on quit) */
extern struct regs regs;        /* of the selected frame's thread, current */
extern int stop_sig;            /* why it stopped last */

int target_run(char **argv);                            /* start it; stops at the entry */
int target_attach(int pid);
void target_kill(void);
void target_detach(void);
int target_alive(void);
int read_mem(u64 addr, void *buf, size_t n);            /* 0 or -1 */
int write_mem(u64 addr, const void *buf, size_t n);
int read_u64(u64 addr, u64 *v);
int fetch_regs(void);
int store_regs(void);
int fetch_fpregs(u64 f[32], u64 *fcsr);
int store_fpregs(const u64 f[32], u64 fcsr);
const char *reg_name(int i);                            /* abi name of x[i] */
int reg_number(const char *name);                       /* -1 if not a register; 32 is pc */

/* breakpoints */
struct bp {
    int id;
    u64 addr;
    u16 saved;
    int inserted, enabled, temp;
    int hits;
    char *where;                /* as given (for info breakpoints) */
    char *cond;                 /* stop only if this expression is non-zero */
    int ignore;                 /* skip this many hits */
};

extern struct bp *bps;
extern int nbps;
struct bp *bp_add(u64 addr, const char *where, int temp);
int bp_delete(int id);                                  /* 0 or -1 */
struct bp *bp_at(u64 addr);

/* What a resume ended with. */
enum stop_kind { STOP_BREAKPOINT, STOP_STEP, STOP_SIGNAL, STOP_EXITED, STOP_KILLED, STOP_ERROR };
struct stop {
    enum stop_kind kind;
    int sig;                    /* STOP_SIGNAL, STOP_KILLED */
    int status;                 /* STOP_EXITED */
    u64 addr;                   /* fault address */
    int code;                   /* si_code */
    struct bp *bp;
    int temp_id;                /* a deleted temporary breakpoint stopped us (0: an internal one) */
};

struct stop resume_continue(int pass_sig);              /* until a breakpoint or a signal */
struct stop step_insn(int over_calls);                  /* one instruction (stepi / nexti) */
struct stop step_line(int over_calls);                  /* one source line (step / next) */
struct stop step_out(u64 *retval_a0, u64 *retval_fa0, int *ok);   /* finish */
struct stop run_until(u64 addr);                        /* until / advance */
int insn_length(u64 pc);

/* frames */
struct frame {
    u64 pc, fp, sp, cfa;        /* cfa: the frame base for variables */
};
int unwind(struct frame *out, int max);                 /* from the current registers */
extern int cur_frame;                                   /* selected frame (frame / up / down) */
int frame_get(int n, struct frame *f);

/* ---- expr.c ---- */

enum vloc { V_NONE, V_MEM, V_REG, V_FREG };

struct value {
    struct type *type;
    enum vloc loc;
    u64 addr;                   /* V_MEM */
    int reg;                    /* V_REG, V_FREG */
    u8 bytes[16];               /* the value (scalars) */
    int lvalue;
    int bitfield;               /* bits (with bit_offset) */
    int bit_offset;
};

int eval(const char *expr, struct value *out);          /* 0 or -1 (message printed) */
extern int eval_quiet;                                  /* eval without error messages */
int value_as_int(struct value *v, i64 *out);
void print_value(struct value *v, int fmt);             /* fmt: 0 natural, 'x' 'd' 'u' 'o' 't' 'c' 'a' 'f' 's' */
int assign(struct value *lhs, struct value *rhs);
const char *frame_desc(u64 pc, char *buf, size_t size); /* func + offset (file:line) */

/* dwarf.c, for expr.c */
int var_location(struct var *v, u64 pc, const u8 **expr, size_t *len);
int dwarf_eval(const u8 *e, size_t n, struct frame *f, int frame_no, u64 frame_base, int has_fb,
               struct value *out);
int frame_base_of(struct func *fn, struct frame *f, int frame_no, u64 *fb);
u64 after_prologue(u64 func_addr);

/* ---- disas.c ---- */

int disassemble(u64 pc, u32 insn, char *out, size_t size);   /* returns the length (2 or 4) */

#endif
