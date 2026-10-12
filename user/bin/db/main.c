/*
 * db: the command loop. The commands and their output follow gdb.
 *
 *   db prog [args]      debug prog (run starts it)
 *   db --args prog ...  the same, every word after prog is an argument
 *   db -p pid [prog]    attach to a running process
 */
#include "db.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- utilities ---- */

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "db: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "db: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "db: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s)
{
    char *d = xmalloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    return s;
}

/* ---- state ---- */

static char **prog_args;        /* argv for run: [0] is the program */
static int nprog_args;
static int have_dwarf;
static int list_file = -1, list_line;   /* where `list` continues */
static int list_center;                 /* after a stop: the next `list` centers on this line */
static int history;                     /* $1, $2, ... */
static char last_cmd[512];
static int x_count = 1, x_fmt = 'x', x_size = 4;
static u64 x_next;
static char *displays[16];
static int display_fmt[16];
static int ndisplays;

static int need_process(void)
{
    if (!pid) {
        err("The program is not being run.");
        return 0;
    }
    return 1;
}

/* ---- describing where we are ---- */

static void print_args(u64 pc, int frame_no)
{
    int saved = cur_frame;
    cur_frame = frame_no;
    struct var *v;
    int n = vars_at(frame_no ? pc - 1 : pc, &v, 1);
    printf("(");
    int first = 1;
    /* the parameters' names, copied: eval() rebuilds the scope array */
    char params[32][64];
    int np = 0;
    for (int i = 0; i < n && np < 32; i++)
        if (v[i].is_param)
            snprintf(params[np++], sizeof(params[0]), "%s", v[i].name);
    for (int i = 0; i < np; i++) {
        printf("%s%s=", first ? "" : ", ", params[i]);
        first = 0;
        struct value val;
        if (eval(params[i], &val) == 0) {
            if (val.type->kind == T_STRUCT || val.type->kind == T_UNION || val.type->kind == T_ARRAY)
                printf("...");
            else if (val.type->kind == T_PTR)
                printf("0x%llx", (unsigned long long)*(u64 *)val.bytes);
            else
                print_value(&val, 0);
        } else {
            printf("<optimized out>");
        }
    }
    printf(")");
    cur_frame = saved;
}

static void print_source_line(int file, int line)
{
    const char *s = src_line(file, line);
    if (s)
        printf("%d\t%s\n", line, s);
    else if (file >= 0)
        printf("%d\tin %s\n", line, files[file].path);
    list_file = file;
    list_line = line + 1;
    list_center = line;
}

/* "#1  0x... in main (argc=1, argv=0x...) at file.c:12" */
static void print_frame_line(int n, struct frame *f, int show_addr)
{
    char d[128];
    u64 look = n ? f->pc - 1 : f->pc;
    struct line_row *r = line_for(look);
    struct func *fn = func_at(look);
    printf("#%-2d ", n);
    if (show_addr || !r || (r && r->addr != f->pc && n))
        printf("0x%016llx in ", (unsigned long long)f->pc);
    printf("%s ", fn ? fn->name : frame_desc(f->pc, d, sizeof(d)));
    if (fn)
        print_args(f->pc, n);
    else
        printf("()");
    if (r)
        printf(" at %s:%d", files[r->file].path, r->line);
    printf("\n");
}

static const char *sig_name(int sig)
{
    static const char *const names[] = { "", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP",
        "SIGABRT", "SIGBUS", "SIGFPE", "SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE",
        "SIGALRM", "SIGTERM", "SIGSTKFLT", "SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN",
        "SIGTTOU" };
    static char other[16];
    if (sig > 0 && sig < (int)(sizeof(names) / sizeof(names[0])))
        return names[sig];
    snprintf(other, sizeof(other), "SIG%d", sig);
    return other;
}

/* gdb-style report after a resume */
static void report(struct stop st, int show_frame)
{
    char d[128];
    switch (st.kind) {
    case STOP_EXITED:
        if (st.status)
            printf("[Inferior 1 (process) exited with code %02o]\n", st.status);
        else
            printf("[Inferior 1 (process) exited normally]\n");
        return;
    case STOP_KILLED:
        printf("\nProgram terminated with signal %s, %s.\nThe program no longer exists.\n",
               sig_name(st.sig), strsignal(st.sig));
        return;
    case STOP_ERROR:
        return;
    case STOP_BREAKPOINT:
        if (st.bp)
            printf("\nBreakpoint %d, ", st.bp->id);
        else if (st.temp_id)
            printf("\nTemporary breakpoint %d, ", st.temp_id);
        show_frame = 1;
        break;
    case STOP_SIGNAL: {
        const char *nm = sig_name(st.sig);
        printf("\nProgram received signal %s, %s.\n", nm, strsignal(st.sig));
        if (st.sig == SIGSEGV || st.sig == SIGBUS)
            printf("Fault address: 0x%llx (%s)\n", (unsigned long long)st.addr,
                   st.code == 2 ? "mapped, but not with that access" : "not mapped");
        show_frame = 1;
        break;
    }
    case STOP_STEP:
        break;
    }
    struct line_row *r = line_for(regs.pc);
    struct func *fn = func_at(regs.pc);
    if (show_frame || !r) {
        if (!r || r->addr != regs.pc)
            printf("0x%016llx in ", (unsigned long long)regs.pc);
        printf("%s ", fn ? fn->name : frame_desc(regs.pc, d, sizeof(d)));
        if (fn)
            print_args(regs.pc, 0);
        else
            printf("()");
        if (r)
            printf(" at %s:%d", files[r->file].path, r->line);
        printf("\n");
    }
    if (r) {
        print_source_line(r->file, r->line);
    } else {
        u32 insn = 0;
        read_mem(regs.pc, &insn, 4);
        char dis[160];
        disassemble(regs.pc, insn, dis, sizeof(dis));
        printf("=> 0x%llx:\t%s\n", (unsigned long long)regs.pc, dis);
    }
    for (int i = 0; i < ndisplays; i++) {
        /* only the displays that make sense here, as in gdb */
        struct value v;
        eval_quiet = 1;
        int ok = eval(displays[i], &v) == 0;
        eval_quiet = 0;
        if (!ok)
            continue;
        printf("%d: %s = ", i + 1, displays[i]);
        print_value(&v, display_fmt[i]);
        printf("\n");
    }
}

/* ---- locations ---- */

static int default_file(void)
{
    if (pid) {
        struct line_row *r = line_for(regs.pc);
        if (r)
            return r->file;
    }
    if (list_file >= 0)
        return list_file;
    struct func *m = func_named("main");
    if (m) {
        struct line_row *r = line_for(m->low);
        if (r)
            return r->file;
    }
    return -1;
}

/* A location: func, file:line, line, *expr. Returns 0 and the address. */
static int parse_location(const char *loc, u64 *addr, char *desc, size_t dsize)
{
    while (isspace((unsigned char)*loc))
        loc++;
    if (*loc == '*') {
        struct value v;
        i64 x;
        if (eval(loc + 1, &v) || value_as_int(&v, &x))
            return -1;
        *addr = (u64)x;
        snprintf(desc, dsize, "*0x%llx", (unsigned long long)x);
        return 0;
    }
    const char *colon = strrchr(loc, ':');
    int line = 0, file = -1;
    if (colon && isdigit((unsigned char)colon[1])) {
        char fname[256];
        snprintf(fname, sizeof(fname), "%.*s", (int)(colon - loc), loc);
        file = find_file(fname);
        if (file < 0) {
            err("No source file named %s.", fname);
            return -1;
        }
        line = atoi(colon + 1);
    } else if (isdigit((unsigned char)*loc)) {
        file = default_file();
        line = atoi(loc);
        if (file < 0) {
            err("No line number information available.");
            return -1;
        }
    }
    if (file >= 0) {
        int actual;
        u64 a = addr_for_line(file, line, &actual);
        if (!a) {
            err("No line %d in file \"%s\".", line, files[file].path);
            return -1;
        }
        *addr = a;
        snprintf(desc, dsize, "%s:%d", files[file].path, actual);
        return 0;
    }
    struct func *f = func_named(loc);
    if (f) {
        *addr = after_prologue(f->low);
        struct line_row *r = line_for(*addr);
        if (r)
            snprintf(desc, dsize, "%s at %s:%d", f->name, files[r->file].path, r->line);
        else
            snprintf(desc, dsize, "%s", f->name);
        return 0;
    }
    struct sym *s = sym_named(loc);
    if (s && s->is_func) {
        *addr = s->addr;
        snprintf(desc, dsize, "%s", s->name);
        return 0;
    }
    err("Function \"%s\" not defined.", loc);
    return -1;
}

/* ---- commands ---- */

static void cmd_break(char *arg, int temp)
{
    char *cond = 0;
    char *ifp = strstr(arg, " if ");
    if (ifp) {
        *ifp = 0;
        cond = trim(ifp + 4);
    } else if (!strncmp(arg, "if ", 3)) {
        cond = trim(arg + 3);
        arg[0] = 0;
    }
    arg = trim(arg);
    u64 addr;
    char desc[300];
    if (!*arg) {
        if (!need_process())
            return;
        addr = regs.pc;
        snprintf(desc, sizeof(desc), "*0x%llx", (unsigned long long)addr);
    } else if (parse_location(arg, &addr, desc, sizeof(desc))) {
        return;
    }
    struct bp *b = bp_add(addr, desc, 0);
    if (temp)
        b->temp = 1;
    if (cond)
        b->cond = xstrdup(cond);
    struct line_row *r = line_for(addr);
    printf("%s %d at 0x%llx", temp ? "Temporary breakpoint" : "Breakpoint", b->id, (unsigned long long)addr);
    if (r)
        printf(": file %s, line %d.\n", files[r->file].path, r->line);
    else
        printf("\n");
}

static void cmd_info_breakpoints(void)
{
    if (!nbps) {
        printf("No breakpoints.\n");
        return;
    }
    printf("Num     Type           Disp Enb Address            What\n");
    for (int i = 0; i < nbps; i++) {
        struct bp *b = &bps[i];
        if (!b->id)
            continue;
        char d[128];
        struct line_row *r = line_for(b->addr);
        struct func *f = func_at(b->addr);
        printf("%-7d breakpoint     %-4s %-3s 0x%016llx ", b->id, b->temp ? "del" : "keep",
               b->enabled ? "y" : "n", (unsigned long long)b->addr);
        if (r && f)
            printf("in %s at %s:%d\n", f->name, files[r->file].path, r->line);
        else
            printf("<%s>\n", frame_desc(b->addr, d, sizeof(d)));
        if (b->cond)
            printf("\tstop only if %s\n", b->cond);
        if (b->hits)
            printf("\tbreakpoint already hit %d time%s\n", b->hits, b->hits == 1 ? "" : "s");
        if (b->ignore)
            printf("\tWill ignore next %d crossings of breakpoint.\n", b->ignore);
    }
}

static void cmd_list(char *arg)
{
    arg = trim(arg);
    int file = list_file >= 0 ? list_file : default_file(), first, last;
    if (*arg == '-') {
        first = list_line - 20;
        last = first + 9;
    } else if (*arg) {
        char *comma = strchr(arg, ',');
        if (comma && isdigit((unsigned char)arg[0]) && isdigit((unsigned char)comma[1])) {
            first = atoi(arg);
            last = atoi(comma + 1);
        } else {
            u64 a;
            char d[300];
            if (parse_location(arg, &a, d, sizeof(d)))
                return;
            struct line_row *r = line_for(a);
            if (!r) {
                err("No line number information for %s.", arg);
                return;
            }
            /* a function: show its opening lines (the location is past the prologue) */
            struct func *f = func_named(arg);
            struct line_row *fr = f ? line_for(f->low) : 0;
            file = r->file;
            int center = fr ? fr->line : r->line;
            first = center - 5;
            last = center + 4;
        }
    } else {
        if (file < 0) {
            err("No symbol table is loaded.");
            return;
        }
        first = list_center ? list_center - 5 : list_line ? list_line : 1;
        last = first + 9;
    }
    if (first < 1) {
        last += 1 - first;
        first = 1;
    }
    if (file < 0) {
        err("No source file.");
        return;
    }
    int printed = 0;
    for (int l = first; l <= last; l++) {
        const char *s = src_line(file, l);
        if (!s)
            break;
        printf("%d\t%s\n", l, s);
        printed++;
    }
    if (!printed)
        printf("Line number %d out of range; \"%s\" has %d lines.\n", first, files[file].path,
               files[file].nlines);
    list_file = file;
    list_line = last + 1;
    list_center = 0;
}

static void cmd_backtrace(char *arg)
{
    if (!need_process())
        return;
    arg = trim(arg);
    int full = 0;
    if (!strncmp(arg, "full", 4) || !strncmp(arg, "-full", 5)) {
        full = 1;
        arg = trim(arg + (arg[0] == '-' ? 5 : 4));
    }
    int max = *arg ? atoi(arg) : 64;
    struct frame fr[64];
    int n = unwind(fr, 64);
    int saved = cur_frame;
    for (int i = 0; i < n && i < max; i++) {
        print_frame_line(i, &fr[i], 0);
        if (full) {
            /* the frame's locals, indented, as gdb does */
            cur_frame = i;
            struct var *v;
            int nv = vars_at(i ? fr[i].pc - 1 : fr[i].pc, &v, 1);
            char names[64][64];
            int k = 0;
            for (int j = 0; j < nv && k < 64; j++)
                if (!v[j].is_param)
                    snprintf(names[k++], sizeof(names[0]), "%s", v[j].name);
            if (!k)
                printf("No locals.\n");
            for (int j = 0; j < k; j++) {
                struct value val;
                printf("        %s = ", names[j]);
                if (eval(names[j], &val) == 0)
                    print_value(&val, 0);
                printf("\n");
            }
        }
    }
    cur_frame = saved;
    if (n > max)
        printf("(More stack frames follow...)\n");
}

static void select_frame(int k)
{
    struct frame f;
    if (frame_get(k, &f) < 0) {
        err(k > 0 ? "Initial frame selected; you cannot go up." : "Bottom (innermost) frame selected; you cannot go down.");
        return;
    }
    cur_frame = k;
    print_frame_line(k, &f, 0);
    struct line_row *r = line_for(k ? f.pc - 1 : f.pc);
    if (r)
        print_source_line(r->file, r->line);
}

static int parse_fmt(char **argp, int *count, int *fmt, int *size)
{
    char *a = *argp;
    if (*a != '/')
        return 0;
    a++;
    if (isdigit((unsigned char)*a))
        *count = (int)strtol(a, &a, 10);
    while (*a && !isspace((unsigned char)*a)) {
        if (strchr("xduotacfsi", *a))
            *fmt = *a;
        else if (*a == 'b')
            *size = 1;
        else if (*a == 'h')
            *size = 2;
        else if (*a == 'w')
            *size = 4;
        else if (*a == 'g')
            *size = 8;
        a++;
    }
    *argp = a;
    return 1;
}

static void cmd_print(char *arg)
{
    int fmt = 0, count = 1, size = 0;
    parse_fmt(&arg, &count, &fmt, &size);
    arg = trim(arg);
    if (!*arg) {
        err("print: nothing to print");
        return;
    }
    struct value v;
    if (eval(arg, &v))
        return;
    printf("$%d = ", ++history);
    print_value(&v, fmt);
    printf("\n");
}

static void cmd_x(char *arg)
{
    if (!need_process())
        return;
    parse_fmt(&arg, &x_count, &x_fmt, &x_size);
    arg = trim(arg);
    u64 addr = x_next;
    if (*arg) {
        struct value v;
        i64 a;
        if (eval(arg, &v))
            return;
        if (v.type->kind == T_ARRAY || v.type->kind == T_FUNC)
            a = (i64)v.addr;
        else if (value_as_int(&v, &a))
            return;
        addr = (u64)a;
    }
    char d[128];
    if (x_fmt == 'i') {
        for (int i = 0; i < x_count; i++) {
            u32 insn = 0;
            if (read_mem(addr, &insn, 4) < 0 && read_mem(addr, &insn, 2) < 0) {
                err("Cannot access memory at address 0x%llx", (unsigned long long)addr);
                return;
            }
            char dis[160];
            int len = disassemble(addr, insn, dis, sizeof(dis));
            printf("%s0x%llx <%s>:\t%s\n", addr == regs.pc ? "=> " : "   ", (unsigned long long)addr,
                   frame_desc(addr, d, sizeof(d)), dis);
            addr += len;
        }
        x_next = addr;
        return;
    }
    if (x_fmt == 's') {
        for (int i = 0; i < x_count; i++) {
            char c;
            printf("0x%llx:\t\"", (unsigned long long)addr);
            int n = 0;
            while (n < 400 && read_mem(addr, &c, 1) == 0 && c) {
                putchar(c == '\n' ? ' ' : c);
                addr++;
                n++;
            }
            printf("\"\n");
            addr++;
        }
        x_next = addr;
        return;
    }
    int size = x_fmt == 'a' ? 8 : x_fmt == 'c' ? 1 : x_size;
    int per_line = size == 8 ? 2 : size == 4 ? 4 : 8;
    for (int i = 0; i < x_count; i++) {
        if (i % per_line == 0) {
            if (i)
                printf("\n");
            if (sym_at(addr))
                printf("0x%llx <%s>:", (unsigned long long)addr, frame_desc(addr, d, sizeof(d)));
            else
                printf("0x%llx:", (unsigned long long)addr);
        }
        u64 val = 0;
        if (read_mem(addr, &val, size) < 0) {
            printf("\nCannot access memory at address 0x%llx\n", (unsigned long long)addr);
            return;
        }
        printf("\t");
        i64 sval = size == 1 ? (int8_t)val : size == 2 ? (int16_t)val : size == 4 ? (int32_t)val : (i64)val;
        switch (x_fmt) {
        case 'd': printf("%lld", (long long)sval); break;
        case 'u': printf("%llu", (unsigned long long)val); break;
        case 'o': printf("0%llo", (unsigned long long)val); break;
        case 'c': printf("%d '%c'", (int)sval, isprint((int)(val & 0xff)) ? (int)(val & 0xff) : '.'); break;
        case 'a': printf("0x%llx <%s>", (unsigned long long)val, frame_desc(val, d, sizeof(d))); break;
        case 'f':
            if (size == 4) {
                float f;
                memcpy(&f, &val, 4);
                printf("%g", f);
            } else {
                double f;
                memcpy(&f, &val, 8);
                printf("%g", f);
            }
            break;
        case 't':
            for (int b = size * 8 - 1; b >= 0; b--)
                putchar((val >> b) & 1 ? '1' : '0');
            break;
        default: printf("0x%0*llx", size * 2, (unsigned long long)val); break;
        }
        addr += size;
    }
    printf("\n");
    x_next = addr;
}

static void print_type(struct type *t, int expand)
{
    char tn[200];
    if (expand && (t->kind == T_STRUCT || t->kind == T_UNION)) {
        printf("%s {\n", t->name ? t->name : "struct");
        for (int i = 0; i < t->nmembers; i++) {
            struct member *m = &t->members[i];
            struct type *mt = m->type;
            if (mt->kind == T_ARRAY) {
                printf("    %s %s[%llu]", type_name(mt->target, tn, sizeof(tn)), m->name,
                       (unsigned long long)mt->count);
            } else {
                type_name(mt, tn, sizeof(tn));
                size_t n = strlen(tn);
                printf("    %s%s%s", tn, n && tn[n - 1] == '*' ? "" : " ", m->name);
            }
            if (m->bit_size)
                printf(" : %d", m->bit_size);
            printf(";\n");
        }
        printf("}");
        return;
    }
    if (expand && t->kind == T_ENUM) {
        printf("%s {", t->name);
        for (int i = 0; i < t->nenums; i++)
            printf("%s%s", i ? ", " : "", t->enums[i].name);
        printf("}");
        return;
    }
    printf("%s", type_name(t, tn, sizeof(tn)));
}

static void cmd_ptype(char *arg, int expand)
{
    arg = trim(arg);
    struct value v;
    if (eval(arg, &v))
        return;
    printf("type = ");
    print_type(v.type, expand);
    printf("\n");
}

static void cmd_info_locals(int args)
{
    if (!need_process())
        return;
    struct frame f;
    if (frame_get(cur_frame, &f) < 0)
        return;
    struct var *v;
    int n = vars_at(cur_frame ? f.pc - 1 : f.pc, &v, 1);
    /* copy the names: eval reuses the scope array */
    char names[64][64];
    int is_param[64], k = 0;
    for (int i = 0; i < n && k < 64; i++) {
        if (v[i].is_param != args)
            continue;
        snprintf(names[k], sizeof(names[k]), "%s", v[i].name);
        is_param[k++] = v[i].is_param;
    }
    if (!k) {
        printf(args ? "No arguments.\n" : "No locals.\n");
        return;
    }
    for (int i = 0; i < k; i++) {
        struct value val;
        printf("%s = ", names[i]);
        if (eval(names[i], &val) == 0)
            print_value(&val, 0);
        printf("\n");
    }
    (void)is_param;
}

static void cmd_info_registers(char *arg)
{
    if (!need_process())
        return;
    arg = trim(arg);
    char d[128];
    if (*arg) {
        struct value v;
        char e[64];
        snprintf(e, sizeof(e), "$%s", arg[0] == '$' ? arg + 1 : arg);
        if (eval(e, &v))
            return;
        u64 x;
        memcpy(&x, v.bytes, 8);
        printf("%-14s 0x%-18llx %lld\n", arg, (unsigned long long)x, (long long)x);
        return;
    }
    for (int i = 1; i < 32; i++) {
        u64 x = regs.x[i];
        printf("%-14s 0x%-18llx ", reg_name(i), (unsigned long long)x);
        if (i == 1 || (x >= prog.text_start && x < prog.text_end))
            printf("0x%llx <%s>\n", (unsigned long long)x, frame_desc(x, d, sizeof(d)));
        else if (i == 2 || i == 8)
            printf("0x%llx\n", (unsigned long long)x);
        else
            printf("%lld\n", (long long)x);
    }
    printf("%-14s 0x%-18llx 0x%llx <%s>\n", "pc", (unsigned long long)regs.pc, (unsigned long long)regs.pc,
           frame_desc(regs.pc, d, sizeof(d)));
}

static void cmd_info_float(void)
{
    if (!need_process())
        return;
    u64 f[32], fcsr;
    if (fetch_fpregs(f, &fcsr) < 0) {
        err("cannot read the FP registers");
        return;
    }
    static const char *const fabi[32] = {
        "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1",
        "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
        "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
    };
    for (int i = 0; i < 32; i++) {
        double dv;
        memcpy(&dv, &f[i], 8);
        printf("%-6s %-24.17g (raw 0x%016llx)\n", fabi[i], dv, (unsigned long long)f[i]);
    }
    printf("fcsr   0x%llx\n", (unsigned long long)fcsr);
}

static void cmd_disassemble(char *arg)
{
    arg = trim(arg);
    u64 lo, hi, mark = pid ? regs.pc : 0;
    if (*arg) {
        char *comma = strchr(arg, ',');
        if (comma) {
            *comma = 0;
            struct value a, b;
            i64 x, y;
            if (eval(arg, &a) || value_as_int(&a, &x))
                return;
            const char *rest = trim(comma + 1);
            int plus = *rest == '+';
            if (eval(plus ? rest + 1 : rest, &b) || value_as_int(&b, &y))
                return;
            lo = (u64)x;
            hi = plus ? lo + (u64)y : (u64)y;
        } else {
            struct func *f = func_named(arg);
            struct sym *s = f ? 0 : sym_named(arg);
            if (f) {
                lo = f->low;
                hi = f->high;
            } else if (s) {
                lo = s->addr;
                hi = s->addr + (s->size ? s->size : 64);
            } else {
                struct value v;
                i64 x;
                if (eval(arg, &v) || value_as_int(&v, &x))
                    return;
                struct func *g = func_at((u64)x);
                struct sym *t = g ? 0 : sym_at((u64)x);
                lo = g ? g->low : t ? t->addr : (u64)x;
                hi = g ? g->high : t && t->size ? t->addr + t->size : lo + 64;
            }
        }
    } else {
        if (!need_process())
            return;
        struct func *f = func_at(regs.pc);
        struct sym *s = f ? 0 : sym_at(regs.pc);
        lo = f ? f->low : s ? s->addr : regs.pc;
        hi = f ? f->high : s && s->size ? s->addr + s->size : regs.pc + 32;
    }
    if (!pid) {
        err("disassemble needs a running program (it reads the code from memory)");
        return;
    }
    char d[128];
    printf("Dump of assembler code for function %s:\n", frame_desc(lo, d, sizeof(d)));
    for (u64 a = lo; a < hi;) {
        u32 insn = 0;
        if (read_mem(a, &insn, 2) < 0)
            break;
        if ((insn & 3) == 3 && read_mem(a, &insn, 4) < 0)
            break;
        char dis[160];
        int len = disassemble(a, insn, dis, sizeof(dis));
        struct func *fa = func_at(a);
        struct sym *s = fa ? 0 : sym_at(a);
        u64 base = fa ? fa->low : s ? s->addr : lo;
        printf("%s0x%016llx <+%llu>:\t%s\n", a == mark ? "=> " : "   ", (unsigned long long)a,
               (unsigned long long)(a - base), dis);
        a += len;
    }
    printf("End of assembler dump.\n");
}

static void cmd_run(char *arg)
{
    if (pid) {
        printf("The program being debugged has been started already; restarting it.\n");
        target_kill();
    }
    arg = trim(arg);
    if (*arg) {
        /* new arguments (kept for the next run) */
        char *copy = xstrdup(arg);
        char **v = xcalloc(64, sizeof(char *));
        int n = 0;
        v[n++] = prog_args[0];
        for (char *t = strtok(copy, " \t"); t && n < 63; t = strtok(0, " \t"))
            v[n++] = t;
        prog_args = v;
        nprog_args = n;
    }
    printf("Starting program: %s", prog_args[0]);
    for (int i = 1; i < nprog_args; i++)
        printf(" %s", prog_args[i]);
    printf("\n");
    if (target_run(prog_args))
        return;
    report(resume_continue(0), 0);
}

static void cmd_info_frame(void)
{
    if (!need_process())
        return;
    struct frame fr[64];
    int n = unwind(fr, 64);
    if (cur_frame >= n)
        return;
    struct frame *f = &fr[cur_frame];
    char d[128];
    struct line_row *r = line_for(cur_frame ? f->pc - 1 : f->pc);
    printf("Stack level %d, frame at 0x%llx:\n pc = 0x%llx in %s", cur_frame, (unsigned long long)f->cfa,
           (unsigned long long)f->pc, frame_desc(f->pc, d, sizeof(d)));
    if (r)
        printf(" (%s:%d)", files[r->file].path, r->line);
    if (cur_frame + 1 < n)
        printf("; saved pc = 0x%llx", (unsigned long long)fr[cur_frame + 1].pc);
    printf("\n");
    if (cur_frame + 1 < n)
        printf(" called by frame at 0x%llx\n", (unsigned long long)fr[cur_frame + 1].cfa);
    if (cur_frame > 0)
        printf(" caller of frame at 0x%llx\n", (unsigned long long)fr[cur_frame - 1].cfa);
    printf(" frame pointer s0 = 0x%llx, sp = 0x%llx\n", (unsigned long long)f->fp, (unsigned long long)f->sp);
}

static void cmd_info_line(char *arg)
{
    arg = trim(arg);
    u64 addr;
    if (*arg) {
        char d[300];
        if (parse_location(arg, &addr, d, sizeof(d)))
            return;
    } else {
        if (!need_process())
            return;
        addr = regs.pc;
    }
    struct line_row *r = line_for(addr);
    char d[128];
    if (!r) {
        printf("No line number information available for address 0x%llx <%s>\n", (unsigned long long)addr,
               frame_desc(addr, d, sizeof(d)));
        return;
    }
    printf("Line %d of \"%s\" starts at address 0x%llx <%s>.\n", r->line, files[r->file].path,
           (unsigned long long)r->addr, frame_desc(r->addr, d, sizeof(d)));
}

static void cmd_info_symbol(char *arg)
{
    struct value v;
    i64 a;
    if (eval(trim(arg), &v))
        return;
    if (v.type->kind == T_FUNC || v.type->kind == T_ARRAY)
        a = (i64)v.addr;
    else if (value_as_int(&v, &a))
        return;
    struct sym *s = sym_at((u64)a);
    if (!s) {
        printf("No symbol matches %s.\n", arg);
        return;
    }
    if ((u64)a == s->addr)
        printf("%s in section %s\n", s->name, s->is_func ? ".text" : ".data");
    else
        printf("%s + %llu in section %s\n", s->name, (unsigned long long)((u64)a - s->addr),
               s->is_func ? ".text" : ".data");
}

static void cmd_info_functions(char *arg, int funcs)
{
    arg = trim(arg);
    printf("All %s%s%s:\n", funcs ? "functions" : "variables", *arg ? " matching " : "", arg);
    int n = 0;
    for (int i = 0; i < prog.nsyms; i++) {
        struct sym *s = &prog.syms[i];
        if (s->is_func != funcs || (*arg && !strstr(s->name, arg)))
            continue;
        printf("0x%016llx  %s\n", (unsigned long long)s->addr, s->name);
        n++;
    }
    if (!n)
        printf("(none)\n");
}

static void help(void)
{
    printf(
        "Running:   run/r [args]  start  continue/c [N]  kill/k  attach PID  detach  quit/q\n"
        "Stepping:  step/s [N]  next/n [N]  stepi/si [N]  nexti/ni [N]  finish  until/u [LOC]  advance LOC\n"
        "Breaking:  break/b [LOC] [if COND]  tbreak LOC  delete/d [N...]  disable/enable [N...]\n"
        "           condition N COND  ignore N COUNT      LOC: func, file:line, line, *address\n"
        "Stack:     backtrace/bt [N]  frame/f [N]  up [N]  down [N]  info frame\n"
        "Data:      print/p[/FMT] EXPR  x/[N][FMT][SIZE] ADDR  display EXPR  undisplay N\n"
        "           set var VAR = EXPR  ptype EXPR  whatis EXPR   FMT: x d u o t c a f s i\n"
        "Info:      info breakpoints|locals|args|registers [R]|all-registers|frame|line|symbol|\n"
        "                functions [S]|variables [S]|source|display\n"
        "Source:    list/l [LOC | N,M | -]  disassemble [FUNC | START,END | START,+LEN]\n"
        "Other:     shell CMD  echo TEXT  help\n");
}

/* Repeat-on-Enter applies to these, as in gdb. */
static int repeatable(const char *cmd)
{
    static const char *const r[] = { "s", "step", "n", "next", "si", "stepi", "ni", "nexti", "c",
                                     "continue", "x", "l", "list", "u", "until", "finish", "up", "down", 0 };
    for (int i = 0; r[i]; i++)
        if (!strcmp(cmd, r[i]))
            return 1;
    return 0;
}

static void after_resume(struct stop st, int show_frame)
{
    report(st, show_frame);
}

static int command(char *line)
{
    char *cmd = trim(line);
    if (!*cmd || *cmd == '#')
        return 0;
    char word[32];
    int n = 0;
    while (cmd[n] && !isspace((unsigned char)cmd[n]) && cmd[n] != '/' && n < 31) {
        word[n] = cmd[n];
        n++;
    }
    word[n] = 0;
    char *arg = cmd + n;
    if (*arg != '/')
        arg = trim(arg);
#define IS(a) (!strcmp(word, a))
    if (IS("q") || IS("quit") || IS("exit")) {
        if (pid) {
            if (attached)
                target_detach();
            else
                target_kill();
        }
        return 1;
    }
    if (IS("h") || IS("help")) {
        help();
    } else if (IS("r") || IS("run")) {
        cmd_run(arg);
    } else if (IS("start")) {
        struct func *m = func_named("main");
        u64 a = m ? after_prologue(m->low) : 0;
        if (!a) {
            struct sym *s = sym_named("main");
            a = s ? s->addr : 0;
        }
        if (!a) {
            err("No symbol table loaded: no main.");
            return 0;
        }
        struct bp *b = bp_add(a, "main", 1);
        b->temp = 1;
        printf("Temporary breakpoint at 0x%llx\n", (unsigned long long)a);
        cmd_run(arg);
    } else if (IS("c") || IS("cont") || IS("continue")) {
        if (!need_process())
            return 0;
        if (*arg) {
            struct bp *b = bp_at(regs.pc);
            if (b) {
                b->ignore = atoi(arg) - 1;
                printf("Will ignore next %d crossings of breakpoint %d.  Continuing.\n", b->ignore, b->id);
            }
        } else {
            printf("Continuing.\n");
        }
        int sig = stop_sig == SIGTRAP || stop_sig == SIGINT || stop_sig == SIGSTOP ? 0 : stop_sig;
        after_resume(resume_continue(sig), 0);
    } else if (IS("s") || IS("step") || IS("n") || IS("next")) {
        if (!need_process())
            return 0;
        int times = *arg ? atoi(arg) : 1;
        struct func *f0 = func_at(regs.pc);
        struct frame fr0, fr1;
        u64 cfa0 = frame_get(0, &fr0) == 0 ? fr0.cfa : 0;
        struct stop st;
        st.kind = STOP_STEP;
        for (int i = 0; i < times && pid; i++) {
            st = step_line(word[0] == 'n');
            if (st.kind != STOP_STEP)
                break;
        }
        int moved = pid && (func_at(regs.pc) != f0 || (frame_get(0, &fr1) == 0 && fr1.cfa != cfa0));
        after_resume(st, moved);
    } else if (IS("si") || IS("stepi") || IS("ni") || IS("nexti")) {
        if (!need_process())
            return 0;
        int times = *arg ? atoi(arg) : 1;
        struct stop st;
        st.kind = STOP_STEP;
        for (int i = 0; i < times && pid; i++) {
            st = step_insn(word[0] == 'n');
            if (st.kind != STOP_STEP)
                break;
        }
        if (st.kind == STOP_STEP && pid) {
            struct line_row *r = line_for(regs.pc);
            char d[128];
            u32 insn = 0;
            read_mem(regs.pc, &insn, 4);
            char dis[160];
            disassemble(regs.pc, insn, dis, sizeof(dis));
            if (r && r->addr == regs.pc)
                print_source_line(r->file, r->line);
            else if (r)
                printf("0x%016llx\t%d\t%s\n", (unsigned long long)regs.pc, r->line,
                       src_line(r->file, r->line) ? src_line(r->file, r->line) : "");
            printf("=> 0x%llx <%s>:\t%s\n", (unsigned long long)regs.pc, frame_desc(regs.pc, d, sizeof(d)), dis);
        } else {
            after_resume(st, 0);
        }
    } else if (IS("fin") || IS("finish")) {
        if (!need_process())
            return 0;
        struct frame fr[2];
        if (unwind(fr, 2) < 2) {
            err("\"finish\" not meaningful in the outermost frame.");
            return 0;
        }
        printf("Run till exit from ");
        print_frame_line(0, &fr[0], 0);
        struct func *callee = func_at(regs.pc);
        u64 a0, fa0;
        int ok;
        struct stop st = step_out(&a0, &fa0, &ok);
        after_resume(st, 1);
        if (ok && callee && callee->ret_die) {
            /* the value returned: in a0 (or fa0), as the function's return type */
            struct type *t = type_at(callee->ret_die);
            char tn[128];
            if (t->kind == T_STRUCT || t->kind == T_UNION || t->kind == T_ARRAY) {
                printf("Value returned has type: %s. Cannot determine contents\n", type_name(t, tn, sizeof(tn)));
            } else if (t->kind != T_VOID) {
                struct value v;
                memset(&v, 0, sizeof(v));
                v.type = t;
                v.loc = V_NONE;
                memcpy(v.bytes, t->kind == T_FLOAT ? &fa0 : &a0, 8);
                printf("Value returned is $%d = ", ++history);
                print_value(&v, 0);
                printf("\n");
            }
        }
    } else if (IS("u") || IS("until") || IS("advance")) {
        if (!need_process())
            return 0;
        if (!*arg) {
            after_resume(step_line(1), 0);
            return 0;
        }
        u64 a;
        char d[300];
        if (parse_location(arg, &a, d, sizeof(d)))
            return 0;
        after_resume(run_until(a), 1);
    } else if (IS("b") || IS("br") || IS("break")) {
        cmd_break(arg, 0);
    } else if (IS("tb") || IS("tbreak")) {
        cmd_break(arg, 1);
    } else if (IS("d") || IS("delete")) {
        if (!*arg) {
            while (nbps)
                bp_delete(bps[0].id);
            printf("Deleted all breakpoints.\n");
        } else {
            for (char *t = strtok(arg, " "); t; t = strtok(0, " "))
                if (bp_delete(atoi(t)) < 0)
                    err("No breakpoint number %s.", t);
        }
    } else if (IS("disable") || IS("enable")) {
        int en = word[0] == 'e';
        for (int i = 0; i < nbps; i++) {
            if (!*arg) {
                bps[i].enabled = en;
                continue;
            }
            char copy[256];
            snprintf(copy, sizeof(copy), "%s", arg);
            for (char *t = strtok(copy, " "); t; t = strtok(0, " "))
                if (atoi(t) == bps[i].id)
                    bps[i].enabled = en;
        }
    } else if (IS("condition")) {
        int id = (int)strtol(arg, &arg, 10);
        arg = trim(arg);
        for (int i = 0; i < nbps; i++)
            if (bps[i].id == id) {
                free(bps[i].cond);
                bps[i].cond = *arg ? xstrdup(arg) : 0;
                if (!*arg)
                    printf("Breakpoint %d now unconditional.\n", id);
                return 0;
            }
        err("No breakpoint number %d.", id);
    } else if (IS("ignore")) {
        int id = (int)strtol(arg, &arg, 10);
        int count = atoi(trim(arg));
        for (int i = 0; i < nbps; i++)
            if (bps[i].id == id) {
                bps[i].ignore = count;
                printf("Will ignore next %d crossings of breakpoint %d.\n", count, id);
                return 0;
            }
        err("No breakpoint number %d.", id);
    } else if (IS("bt") || IS("backtrace") || IS("where")) {
        cmd_backtrace(arg);
    } else if (IS("f") || IS("frame")) {
        if (need_process())
            select_frame(*arg ? atoi(arg) : cur_frame);
    } else if (IS("up") || IS("down")) {
        if (need_process()) {
            int k = *arg ? atoi(arg) : 1;
            select_frame(cur_frame + (word[0] == 'u' ? k : -k));
        }
    } else if (IS("p") || IS("print") || IS("inspect") || IS("call")) {
        cmd_print(arg);
    } else if (IS("output")) {
        struct value v;
        if (eval(arg, &v) == 0)
            print_value(&v, 0);
    } else if (IS("x")) {
        cmd_x(arg);
    } else if (IS("display")) {
        int fmt = 0, count = 1, size = 0;
        parse_fmt(&arg, &count, &fmt, &size);
        arg = trim(arg);
        if (!*arg) {
            for (int i = 0; i < ndisplays; i++) {
                struct value v;
                printf("%d: %s = ", i + 1, displays[i]);
                if (eval(displays[i], &v) == 0)
                    print_value(&v, display_fmt[i]);
                printf("\n");
            }
        } else if (ndisplays < 16) {
            display_fmt[ndisplays] = fmt;
            displays[ndisplays++] = xstrdup(arg);
            struct value v;
            eval_quiet = 1;             /* shown only where it makes sense, as at stops */
            int ok = pid && eval(arg, &v) == 0;
            eval_quiet = 0;
            if (ok) {
                printf("%d: %s = ", ndisplays, arg);
                print_value(&v, fmt);
                printf("\n");
            }
        }
    } else if (IS("undisplay")) {
        int k = atoi(arg);
        if (k >= 1 && k <= ndisplays) {
            free(displays[k - 1]);
            memmove(&displays[k - 1], &displays[k], (ndisplays - k) * sizeof(char *));
            memmove(&display_fmt[k - 1], &display_fmt[k], (ndisplays - k) * sizeof(int));
            ndisplays--;
        } else if (!*arg) {
            ndisplays = 0;
        }
    } else if (IS("set")) {
        if (!strncmp(arg, "var ", 4))
            arg = trim(arg + 4);
        else if (!strncmp(arg, "variable ", 9))
            arg = trim(arg + 9);
        struct value v;
        if (strchr(arg, '='))
            eval(arg, &v);
        else
            err("set: expected VAR = EXPR");
    } else if (IS("ptype") || IS("whatis")) {
        cmd_ptype(arg, word[0] == 'p');
    } else if (IS("l") || IS("list")) {
        cmd_list(arg);
    } else if (IS("disas") || IS("disassemble")) {
        cmd_disassemble(arg);
    } else if (IS("k") || IS("kill")) {
        if (need_process()) {
            target_kill();
            printf("[Inferior 1 (process) killed]\n");
        }
    } else if (IS("detach")) {
        if (need_process()) {
            target_detach();
            printf("Detached.\n");
        }
    } else if (IS("attach")) {
        if (pid) {
            err("already debugging a process: kill or detach it first");
            return 0;
        }
        if (target_attach(atoi(arg)) == 0) {
            printf("Attaching to process %s\n", arg);
            report((struct stop){ .kind = STOP_STEP }, 1);
        }
    } else if (IS("i") || IS("info")) {
        char sub[32];
        int k = 0;
        while (arg[k] && !isspace((unsigned char)arg[k]) && k < 31) {
            sub[k] = arg[k];
            k++;
        }
        sub[k] = 0;
        char *rest = trim(arg + k);
#define SUB(a) (!strncmp(sub, a, strlen(sub) ? strlen(sub) : 1) && strlen(sub) <= strlen(a))
        if (!sub[0])
            printf("info: breakpoints locals args registers all-registers frame line symbol functions variables source display\n");
        else if (SUB("breakpoints"))
            cmd_info_breakpoints();
        else if (SUB("locals"))
            cmd_info_locals(0);
        else if (SUB("args"))
            cmd_info_locals(1);
        else if (SUB("registers"))
            cmd_info_registers(rest);
        else if (SUB("frame"))              /* first: "info f" is the frame, as in gdb */
            cmd_info_frame();
        else if (SUB("all-registers") || !strcmp(sub, "float")) {
            cmd_info_registers("");
            cmd_info_float();
        }
        else if (SUB("line"))
            cmd_info_line(rest);
        else if (SUB("symbol"))
            cmd_info_symbol(rest);
        else if (SUB("functions"))
            cmd_info_functions(rest, 1);
        else if (SUB("variables"))
            cmd_info_functions(rest, 0);
        else if (SUB("display")) {
            for (int i = 0; i < ndisplays; i++)
                printf("%d:   %s\n", i + 1, displays[i]);
        } else if (SUB("source")) {
            int f = default_file();
            if (f >= 0)
                printf("Current source file is %s\n", files[f].path);
            printf("%d source files, %d line table rows\n", nfiles, nrows);
        } else if (SUB("program")) {
            if (pid)
                printf("\tUsing the running image of child process %d.\nProgram stopped at 0x%llx.\n", pid,
                       (unsigned long long)regs.pc);
            else
                printf("The program being debugged is not being run.\n");
        } else
            err("Undefined info command: \"%s\".  Try \"help\".", sub);
#undef SUB
    } else if (IS("shell") || word[0] == '!') {
        const char *c = word[0] == '!' ? cmd + 1 : arg;
        fflush(stdout);
        if (system(c) < 0)
            err("shell: %s", strerror(errno));
    } else if (IS("echo")) {
        for (const char *p = arg; *p; p++) {
            if (*p == '\\' && p[1] == 'n') {
                putchar('\n');
                p++;
            } else {
                putchar(*p);
            }
        }
    } else if (IS("file")) {
        if (*arg && elf_load(arg) == 0) {
            have_dwarf = dwarf_load();
            prog_args[0] = xstrdup(arg);
            printf("Reading symbols from %s...%s\n", arg, have_dwarf ? "" : "\n(No debugging symbols found)");
        }
    } else {
        err("Undefined command: \"%s\".  Try \"help\".", word);
    }
#undef IS
    return 0;
}

static char *find_program(const char *name)
{
    if (strchr(name, '/'))
        return xstrdup(name);
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    char buf[512];
    struct stat st;
    if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(buf, sizeof(buf), "./%s", name);
        return xstrdup(buf);
    }
    while (*path) {
        const char *colon = strchr(path, ':');
        int n = colon ? (int)(colon - path) : (int)strlen(path);
        snprintf(buf, sizeof(buf), "%.*s/%s", n, path, name);
        if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
            return xstrdup(buf);
        path += n + (colon ? 1 : 0);
    }
    return xstrdup(name);
}

int main(int argc, char **argv)
{
    int attach_pid = 0, batch = 0;
    char *cmds[16];
    int ncmds = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            attach_pid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--args")) {
            i++;
            break;
        } else if (!strcmp(argv[i], "-batch") || !strcmp(argv[i], "--batch"))
            batch = 1;
        else if ((!strcmp(argv[i], "-ex") || !strcmp(argv[i], "-x")) && i + 1 < argc && ncmds < 16)
            cmds[ncmds++] = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: db [-ex CMD]... [-batch] PROG [ARGS...]\n"
                            "       db --args PROG ARGS...\n       db -p PID [PROG]\n");
            return 2;
        } else
            break;
    }
    signal(SIGINT, SIG_IGN);    /* ^C stops the program (it gets SIGINT), not us */
    if (i < argc) {
        nprog_args = argc - i;
        prog_args = xcalloc(nprog_args + 1, sizeof(char *));
        prog_args[0] = find_program(argv[i]);
        for (int k = 1; k < nprog_args; k++)
            prog_args[k] = argv[i + k];
    } else if (attach_pid) {
        char path[64], buf[256];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", attach_pid);
        FILE *f = fopen(path, "r");
        size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
        if (f)
            fclose(f);
        buf[n] = 0;
        nprog_args = 1;
        prog_args = xcalloc(2, sizeof(char *));
        prog_args[0] = find_program(n ? buf : "?");
    } else {
        fprintf(stderr, "usage: db PROG [ARGS...]   (db --help for more)\n");
        return 2;
    }
    printf("Reading symbols from %s...\n", prog_args[0]);
    if (elf_load(prog_args[0]) < 0)
        return 1;
    have_dwarf = dwarf_load();
    if (!have_dwarf)
        printf("(No debugging symbols found in %s: compile with tcc -g for source-level debugging)\n",
               prog_args[0]);
    if (attach_pid) {
        if (target_attach(attach_pid) < 0)
            return 1;
        printf("Attaching to process %d\n", attach_pid);
        report((struct stop){ .kind = STOP_STEP }, 1);
    }
    for (int k = 0; k < ncmds; k++) {
        printf("(db) %s\n", cmds[k]);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", cmds[k]);
        if (command(buf))
            return 0;
    }
    if (batch) {
        if (pid)
            attached ? target_detach() : target_kill();
        return 0;
    }
    char line[512];
    for (;;) {
        printf("(db) ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("quit\n");
            command("quit");
            return 0;
        }
        char *t = trim(line);
        if (!*t) {
            if (!last_cmd[0])
                continue;
            snprintf(line, sizeof(line), "%s", last_cmd);
            /* `list` and `x` continue; others repeat */
        } else {
            char w[32];
            int n = 0;
            while (t[n] && !isspace((unsigned char)t[n]) && t[n] != '/' && n < 31) {
                w[n] = t[n];
                n++;
            }
            w[n] = 0;
            if (repeatable(w)) {
                if (!strcmp(w, "x") || !strcmp(w, "l") || !strcmp(w, "list"))
                    snprintf(last_cmd, sizeof(last_cmd), "%s", w);   /* continue from where it ended */
                else
                    snprintf(last_cmd, sizeof(last_cmd), "%s", t);
            } else {
                last_cmd[0] = 0;
            }
        }
        if (command(line))
            return 0;
    }
}
