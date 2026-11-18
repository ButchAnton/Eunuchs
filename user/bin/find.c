/*
 * find: walk directory trees, evaluating an expression for each file.
 *
 *   find [PATH...] [EXPRESSION]
 *
 * Tests: -name -iname -path -ipath -type [fdcbp] -size [+-]N[cwbkMG]
 *   -mtime/-ctime/-atime [+-]N (days) -mmin/-cmin/-amin [+-]N -newer FILE
 *   -perm [-/]MODE -user -group -uid -gid -nouser -nogroup -links -inum
 *   -empty -readable -writable -executable -true -false
 * Actions: -print -print0 -printf FMT -ls -exec CMD {} ; -exec CMD {} +
 *   -ok CMD ; -delete -prune -quit
 * Options: -maxdepth N -mindepth N -depth -xdev (-mount)
 * Operators: ( ) ! -not -a -and -o -or ,  (implicit -a between terms)
 * Without an action, the whole expression is followed by -print.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    N_AND, N_OR, N_NOT, N_COMMA, N_TRUE, N_FALSE,
    N_NAME, N_INAME, N_PATH, N_IPATH, N_TYPE, N_SIZE, N_MTIME, N_MMIN, N_CTIME, N_CMIN,
    N_NEWER, N_PERM, N_USER, N_GROUP, N_NOUSER, N_NOGROUP, N_LINKS, N_INUM, N_EMPTY,
    N_READABLE, N_WRITABLE, N_EXECUTABLE,
    N_PRINT, N_PRINT0, N_PRINTF, N_LS, N_EXEC, N_EXECPLUS, N_OK, N_DELETE, N_PRUNE, N_QUIT,
};

struct node {
    int type;
    struct node *l, *r;
    const char *s;                      /* pattern, format */
    long n;                             /* number */
    int cmp;                            /* '+' '-' or 0 for numbers; perm: '-' '/' or 0 */
    long unit;
    char **argv;                        /* -exec: the command, {} included */
    int argc;
    /* -exec ... +: the files gathered */
    char **acc;
    int nacc, capacc;
    size_t accbytes;
};

static char **args;
static int nargs, ai;
static int maxdepth = -1, mindepth = 0, depth_first, xdev, had_action;
static int status, quit;
static time_t now;

/* the file being looked at */
static const char *cur_path;
static struct stat cur_st;
static int cur_depth, pruned;

static void die(const char *fmt, const char *arg)
{
    eprintf("find: ");
    eprintf(fmt, arg);
    eprintf("\n");
    exit(1);
}

static struct node *mk(int type)
{
    struct node *n = calloc(1, sizeof(*n));
    n->type = type;
    return n;
}

static const char *need_arg(const char *prim)
{
    if (ai >= nargs)
        die("missing argument to `%s'", prim);
    return args[ai++];
}

static long parse_num(const char *s, int *cmp, const char *prim)
{
    *cmp = 0;
    if (*s == '+' || *s == '-')
        *cmp = *s++;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s)
        die("invalid argument to %s", prim);
    return v;
}

static struct node *parse_or(void);

static struct node *parse_primary(void)
{
    const char *a = args[ai++];
    struct node *n;
    if (!strcmp(a, "(")) {
        n = parse_or();
        if (ai >= nargs || strcmp(args[ai], ")"))
            die("missing `)'%s", "");
        ai++;
        return n;
    }
    if (!strcmp(a, "!") || !strcmp(a, "-not")) {
        if (ai >= nargs)
            die("expected an expression after `%s'", a);
        n = mk(N_NOT);
        n->l = parse_primary();
        return n;
    }
#define IS(x) (!strcmp(a, x))
    if (IS("-name") || IS("-iname") || IS("-path") || IS("-ipath") || IS("-wholename")) {
        n = mk(IS("-name") ? N_NAME : IS("-iname") ? N_INAME : IS("-ipath") ? N_IPATH : N_PATH);
        n->s = need_arg(a);
        return n;
    }
    if (IS("-type")) {
        n = mk(N_TYPE);
        n->s = need_arg(a);
        if (!strchr("fdcbpls", n->s[0]) || n->s[1])
            die("unknown argument to -type: %s", n->s);
        return n;
    }
    if (IS("-size")) {
        n = mk(N_SIZE);
        const char *v = need_arg(a);
        n->n = parse_num(v, &n->cmp, a);
        char u = v[strlen(v) - 1];
        n->unit = u == 'c' ? 1 : u == 'w' ? 2 : u == 'k' ? 1024 : u == 'M' ? 1048576
                : u == 'G' ? 1073741824L : 512;
        return n;
    }
    if (IS("-mtime") || IS("-atime") || IS("-ctime") || IS("-mmin") || IS("-amin") || IS("-cmin")) {
        int mins = strstr(a, "min") != 0, ch = a[1] == 'c';
        n = mk(mins ? (ch ? N_CMIN : N_MMIN) : (ch ? N_CTIME : N_MTIME));
        n->n = parse_num(need_arg(a), &n->cmp, a);
        return n;
    }
    if (IS("-newer")) {
        n = mk(N_NEWER);
        const char *f = need_arg(a);
        struct stat st;
        if (stat(f, &st) < 0)
            die("%s: no such file", f);
        n->n = st.st_mtime;
        n->unit = st.st_mtim.tv_nsec;
        return n;
    }
    if (IS("-perm")) {
        n = mk(N_PERM);
        const char *v = need_arg(a);
        if (*v == '-' || *v == '/')
            n->cmp = *v++;
        char *end;
        n->n = strtol(v, &end, 8);
        if (end == v || *end)
            die("invalid mode `%s' (only octal modes)", v);
        return n;
    }
    if (IS("-user") || IS("-uid") || IS("-group") || IS("-gid")) {
        int grp = a[1] == 'g';
        n = mk(grp ? N_GROUP : N_USER);
        const char *v = need_arg(a);
        char *end;
        long id = strtol(v, &end, 10);
        if (end == v || *end) {
            if (IS("-uid") || IS("-gid"))
                die("invalid id `%s'", v);
            if (grp) {
                struct group *g = getgrnam(v);
                if (!g)
                    die("`%s' is not the name of an existing group", v);
                id = g->gr_gid;
            } else {
                struct passwd *pw = getpwnam(v);
                if (!pw)
                    die("`%s' is not the name of a known user", v);
                id = pw->pw_uid;
            }
        }
        n->n = id;
        return n;
    }
    if (IS("-links") || IS("-inum")) {
        n = mk(IS("-links") ? N_LINKS : N_INUM);
        n->n = parse_num(need_arg(a), &n->cmp, a);
        return n;
    }
    if (IS("-nouser")) return mk(N_NOUSER);
    if (IS("-nogroup")) return mk(N_NOGROUP);
    if (IS("-empty")) return mk(N_EMPTY);
    if (IS("-readable")) return mk(N_READABLE);
    if (IS("-writable")) return mk(N_WRITABLE);
    if (IS("-executable")) return mk(N_EXECUTABLE);
    if (IS("-true")) return mk(N_TRUE);
    if (IS("-false")) return mk(N_FALSE);
    if (IS("-print") || IS("-print0") || IS("-ls") || IS("-delete") || IS("-quit")) {
        had_action = 1;
        if (IS("-delete"))
            depth_first = 1;
        return mk(IS("-print") ? N_PRINT : IS("-print0") ? N_PRINT0 : IS("-ls") ? N_LS
                  : IS("-delete") ? N_DELETE : N_QUIT);
    }
    if (IS("-printf")) {
        had_action = 1;
        n = mk(N_PRINTF);
        n->s = need_arg(a);
        return n;
    }
    if (IS("-prune")) return mk(N_PRUNE);
    if (IS("-exec") || IS("-ok")) {
        had_action = 1;
        int start = ai;
        while (ai < nargs && strcmp(args[ai], ";") &&
               !(IS("-exec") && !strcmp(args[ai], "+") && ai > start && !strcmp(args[ai - 1], "{}")))
            ai++;
        if (ai >= nargs || ai == start)
            die("missing argument to `%s'", a);
        n = mk(IS("-ok") ? N_OK : !strcmp(args[ai], "+") ? N_EXECPLUS : N_EXEC);
        n->argc = ai - start;
        n->argv = calloc(n->argc + 1, sizeof(char *));
        memcpy(n->argv, args + start, n->argc * sizeof(char *));
        ai++;
        return n;
    }
    /* options: always true */
    if (IS("-maxdepth") || IS("-mindepth")) {
        const char *v = need_arg(a);
        char *end;
        long d = strtol(v, &end, 10);
        if (end == v || *end || d < 0)
            die("invalid argument to %s", a);
        if (IS("-maxdepth"))
            maxdepth = d;
        else
            mindepth = d;
        return mk(N_TRUE);
    }
    if (IS("-depth") || IS("-d")) {
        depth_first = 1;
        return mk(N_TRUE);
    }
    if (IS("-xdev") || IS("-mount")) {
        xdev = 1;
        return mk(N_TRUE);
    }
    if (IS("-follow") || IS("-noleaf"))
        return mk(N_TRUE);
#undef IS
    die("unknown predicate `%s'", a);
    return 0;
}

static int starts_term(void)
{
    return ai < nargs && strcmp(args[ai], ")") && strcmp(args[ai], "-o") && strcmp(args[ai], "-or") &&
           strcmp(args[ai], ",");
}

static struct node *parse_and(void)
{
    struct node *l = parse_primary();
    while (starts_term()) {
        if (!strcmp(args[ai], "-a") || !strcmp(args[ai], "-and")) {
            ai++;
            if (ai >= nargs)
                die("expected an expression after -a%s", "");
        }
        struct node *n = mk(N_AND);
        n->l = l;
        n->r = parse_primary();
        l = n;
    }
    return l;
}

static struct node *parse_or(void)
{
    struct node *l = parse_and();
    while (ai < nargs && (!strcmp(args[ai], "-o") || !strcmp(args[ai], "-or") || !strcmp(args[ai], ","))) {
        int comma = args[ai][0] == ',';
        ai++;
        if (ai >= nargs)
            die("expected an expression after `%s'", comma ? "," : "-o");
        struct node *n = mk(comma ? N_COMMA : N_OR);
        n->l = l;
        n->r = parse_and();
        l = n;
    }
    return l;
}

/* ---- evaluation ---- */

static int num_cmp(long v, const struct node *n)
{
    return n->cmp == '+' ? v > n->n : n->cmp == '-' ? v < n->n : v == n->n;
}

static const char *base_name(const char *path)
{
    /* the last component, ignoring trailing slashes ("/" stays "/") */
    static char b[256];
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    size_t s = len;
    while (s > 0 && path[s - 1] != '/')
        s--;
    if (len - s >= sizeof(b))
        len = s + sizeof(b) - 1;
    memcpy(b, path + s, len - s);
    b[len - s] = 0;
    return b;
}

static int run_cmd(char **argv)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        eprintf("find: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        eprintf("find: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* {} anywhere in an argument becomes the path */
static char *subst(const char *arg, const char *path)
{
    const char *b = strstr(arg, "{}");
    if (!b)
        return strdup(arg);
    size_t n = strlen(arg) + strlen(path) * 4 + 1;
    char *o = malloc(n), *q = o;
    for (const char *s = arg; *s;) {
        if (s[0] == '{' && s[1] == '}') {
            q = stpcpy(q, path);
            s += 2;
        } else {
            *q++ = *s++;
        }
    }
    *q = 0;
    return o;
}

static int exec_one(struct node *n, int ask)
{
    char **argv = calloc(n->argc + 1, sizeof(char *));
    for (int i = 0; i < n->argc; i++)
        argv[i] = subst(n->argv[i], cur_path);
    int ok = 1;
    if (ask) {
        eprintf("< ");
        for (int i = 0; i < n->argc; i++)
            eprintf("%s%s", argv[i], i + 1 < n->argc ? " " : "");
        eprintf(" > ? ");
        char line[64];
        int fd = open("/dev/tty", O_RDONLY);
        long r = read(fd >= 0 ? fd : 0, line, sizeof(line) - 1);
        if (fd >= 0)
            close(fd);
        ok = r > 0 && (line[0] == 'y' || line[0] == 'Y');
    }
    int r = ok ? run_cmd(argv) : 1;
    for (int i = 0; i < n->argc; i++)
        free(argv[i]);
    free(argv);
    return r == 0;
}

static void flush_plus(struct node *n)
{
    if (!n->nacc)
        return;
    int pre = n->argc - 1;              /* the arguments before {} */
    char **argv = calloc(pre + n->nacc + 1, sizeof(char *));
    memcpy(argv, n->argv, pre * sizeof(char *));
    memcpy(argv + pre, n->acc, n->nacc * sizeof(char *));
    if (run_cmd(argv) != 0)
        status = 1;
    for (int i = 0; i < n->nacc; i++)
        free(n->acc[i]);
    free(argv);
    n->nacc = 0;
    n->accbytes = 0;
}

static void flush_all(struct node *n)
{
    if (!n)
        return;
    if (n->type == N_EXECPLUS)
        flush_plus(n);
    flush_all(n->l);
    flush_all(n->r);
}

static void mode_string(unsigned mode, char *b)
{
    b[0] = S_ISDIR(mode) ? 'd' : S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b' : S_ISFIFO(mode) ? 'p' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        b[i + 1] = mode & (0400 >> i) ? rwx[i] : '-';
    if (mode & 04000) b[3] = b[3] == 'x' ? 's' : 'S';
    if (mode & 02000) b[6] = b[6] == 'x' ? 's' : 'S';
    if (mode & 01000) b[9] = b[9] == 'x' ? 't' : 'T';
    b[10] = 0;
}

static void do_ls(void)
{
    char mode[12], when[32], u[16], g[16];
    mode_string(cur_st.st_mode, mode);
    struct passwd *pw = getpwuid(cur_st.st_uid);
    snprintf(u, sizeof(u), "%s", pw ? pw->pw_name : "");
    if (!pw)
        snprintf(u, sizeof(u), "%u", (unsigned)cur_st.st_uid);
    struct group *gr = getgrgid(cur_st.st_gid);
    snprintf(g, sizeof(g), "%s", gr ? gr->gr_name : "");
    if (!gr)
        snprintf(g, sizeof(g), "%u", (unsigned)cur_st.st_gid);
    time_t t = cur_st.st_mtime;
    strftime(when, sizeof(when), "%b %e %H:%M", localtime(&t));
    printf("%7lu %6lu %s %3u %-8s %-8s %8lu %s %s\n", (unsigned long)cur_st.st_ino,
           (unsigned long)(cur_st.st_blocks + 1) / 2, mode, (unsigned)cur_st.st_nlink, u, g,
           (unsigned long)cur_st.st_size, when, cur_path);
}

static void do_printf(const char *f)
{
    for (; *f; f++) {
        if (*f == '\\' && f[1]) {
            f++;
            putchar(*f == 'n' ? '\n' : *f == 't' ? '\t' : *f == '0' ? 0 : *f);
            continue;
        }
        if (*f != '%' || !f[1]) {
            putchar(*f);
            continue;
        }
        f++;
        switch (*f) {
        case '%': putchar('%'); break;
        case 'p': fputs(cur_path, stdout); break;
        case 'f': fputs(base_name(cur_path), stdout); break;
        case 'h': {
            const char *slash = strrchr(cur_path, '/');
            if (!slash)
                putchar('.');
            else
                fwrite(cur_path, 1, slash == cur_path ? 1 : (size_t)(slash - cur_path), stdout);
            break;
        }
        case 's': printf("%lu", (unsigned long)cur_st.st_size); break;
        case 'd': printf("%d", cur_depth); break;
        case 'm': printf("%o", cur_st.st_mode & 07777); break;
        case 'M': { char m[12]; mode_string(cur_st.st_mode, m); fputs(m, stdout); break; }
        case 'u': { struct passwd *p = getpwuid(cur_st.st_uid); if (p) fputs(p->pw_name, stdout); else printf("%u", (unsigned)cur_st.st_uid); break; }
        case 'U': printf("%u", (unsigned)cur_st.st_uid); break;
        case 'g': { struct group *g = getgrgid(cur_st.st_gid); if (g) fputs(g->gr_name, stdout); else printf("%u", (unsigned)cur_st.st_gid); break; }
        case 'G': printf("%u", (unsigned)cur_st.st_gid); break;
        case 'i': printf("%lu", (unsigned long)cur_st.st_ino); break;
        case 'n': printf("%u", (unsigned)cur_st.st_nlink); break;
        case 'y': putchar(S_ISDIR(cur_st.st_mode) ? 'd' : S_ISCHR(cur_st.st_mode) ? 'c' : S_ISBLK(cur_st.st_mode) ? 'b' : S_ISFIFO(cur_st.st_mode) ? 'p' : 'f'); break;
        case 't': { time_t t = cur_st.st_mtime; char b[64]; strftime(b, sizeof(b), "%a %b %e %H:%M:%S %Y", localtime(&t)); fputs(b, stdout); break; }
        case 'T':
            if (f[1]) {
                f++;
                time_t t = cur_st.st_mtime;
                char fmt[3] = { '%', *f, 0 }, b[64];
                if (*f == '@')
                    printf("%ld", (long)t);
                else {
                    strftime(b, sizeof(b), fmt, localtime(&t));
                    fputs(b, stdout);
                }
            }
            break;
        default: putchar('%'); putchar(*f); break;
        }
    }
}

static int is_empty(void)
{
    if (S_ISREG(cur_st.st_mode))
        return cur_st.st_size == 0;
    if (!S_ISDIR(cur_st.st_mode))
        return 0;
    DIR *d = opendir(cur_path);
    if (!d)
        return 0;
    struct dirent *e;
    int empty = 1;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            empty = 0;
            break;
        }
    closedir(d);
    return empty;
}

static int eval(struct node *n)
{
    switch (n->type) {
    case N_AND: return eval(n->l) && !quit && eval(n->r);
    case N_OR: return eval(n->l) || eval(n->r);
    case N_COMMA: eval(n->l); return eval(n->r);
    case N_NOT: return !eval(n->l);
    case N_TRUE: return 1;
    case N_FALSE: return 0;
    case N_NAME: return !fnmatch(n->s, base_name(cur_path), 0);
    case N_INAME: return !fnmatch(n->s, base_name(cur_path), FNM_CASEFOLD);
    case N_PATH: return !fnmatch(n->s, cur_path, 0);
    case N_IPATH: return !fnmatch(n->s, cur_path, FNM_CASEFOLD);
    case N_TYPE: {
        unsigned m = cur_st.st_mode;
        switch (n->s[0]) {
        case 'f': return S_ISREG(m);
        case 'd': return S_ISDIR(m);
        case 'c': return S_ISCHR(m);
        case 'b': return S_ISBLK(m);
        case 'p': return S_ISFIFO(m);
        default: return 0;              /* no symbolic links or sockets in efs */
        }
    }
    case N_SIZE: {
        long units = (cur_st.st_size + n->unit - 1) / n->unit;
        return num_cmp(units, n);
    }
    case N_MTIME: return num_cmp((now - cur_st.st_mtime) / 86400, n);
    case N_CTIME: return num_cmp((now - cur_st.st_ctime) / 86400, n);
    case N_MMIN: return num_cmp((now - cur_st.st_mtime + 59) / 60, n);
    case N_CMIN: return num_cmp((now - cur_st.st_ctime + 59) / 60, n);
    case N_NEWER:
        return cur_st.st_mtime > n->n || (cur_st.st_mtime == n->n && cur_st.st_mtim.tv_nsec > n->unit);
    case N_PERM: {
        unsigned m = cur_st.st_mode & 07777;
        if (n->cmp == '-') return (m & n->n) == (unsigned)n->n;
        if (n->cmp == '/') return n->n ? (m & n->n) != 0 : 1;
        return m == (unsigned)n->n;
    }
    case N_USER: return cur_st.st_uid == (unsigned)n->n;
    case N_GROUP: return cur_st.st_gid == (unsigned)n->n;
    case N_NOUSER: return !getpwuid(cur_st.st_uid);
    case N_NOGROUP: return !getgrgid(cur_st.st_gid);
    case N_LINKS: return num_cmp(cur_st.st_nlink, n);
    case N_INUM: return num_cmp(cur_st.st_ino, n);
    case N_EMPTY: return is_empty();
    case N_READABLE: return access(cur_path, R_OK) == 0;
    case N_WRITABLE: return access(cur_path, W_OK) == 0;
    case N_EXECUTABLE: return access(cur_path, X_OK) == 0;
    case N_PRINT: printf("%s\n", cur_path); return 1;
    case N_PRINT0: fputs(cur_path, stdout); putchar(0); return 1;
    case N_PRINTF: do_printf(n->s); return 1;
    case N_LS: do_ls(); return 1;
    case N_EXEC: return exec_one(n, 0);
    case N_OK: return exec_one(n, 1);
    case N_EXECPLUS:
        if (n->nacc == n->capacc) {
            n->capacc = n->capacc ? n->capacc * 2 : 64;
            n->acc = realloc(n->acc, n->capacc * sizeof(char *));
        }
        n->acc[n->nacc++] = strdup(cur_path);
        n->accbytes += strlen(cur_path) + 1;
        if (n->nacc >= 1000 || n->accbytes >= 32768)
            flush_plus(n);
        return 1;
    case N_DELETE:
        if (!strcmp(cur_path, "."))
            return 1;
        if ((S_ISDIR(cur_st.st_mode) ? rmdir(cur_path) : unlink(cur_path)) < 0) {
            eprintf("find: cannot delete `%s': %s\n", cur_path, strerror(errno));
            status = 1;
            return 0;
        }
        return 1;
    case N_PRUNE:
        if (!depth_first)
            pruned = 1;
        return 1;
    case N_QUIT:
        quit = 1;
        return 1;
    }
    return 0;
}

static struct node *expr;

static void visit(const char *path, int depth, unsigned root_dev)
{
    if (quit)
        return;
    struct stat st;
    if (stat(path, &st) < 0) {
        eprintf("find: `%s': %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    int is_dir = S_ISDIR(st.st_mode);
    int descend = is_dir && (maxdepth < 0 || depth < maxdepth) && !(xdev && depth > 0 && st.st_dev != root_dev);
    if (!depth_first && depth >= mindepth) {
        cur_path = path, cur_st = st, cur_depth = depth, pruned = 0;
        eval(expr);
        if (pruned)
            descend = 0;
    }
    if (descend && !quit) {
        DIR *d = opendir(path);
        if (!d) {
            eprintf("find: `%s': %s\n", path, strerror(errno));
            status = 1;
        } else {
            /* read the names first: -delete and -exec may change the directory */
            char **names = 0;
            int n = 0, cap = 0;
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                if (n == cap) {
                    cap = cap ? cap * 2 : 32;
                    names = realloc(names, cap * sizeof(char *));
                }
                names[n++] = strdup(e->d_name);
            }
            closedir(d);
            size_t plen = strlen(path);
            for (int i = 0; i < n; i++) {
                char *child = malloc(plen + strlen(names[i]) + 2);
                sprintf(child, "%s%s%s", path, plen && path[plen - 1] == '/' ? "" : "/", names[i]);
                visit(child, depth + 1, depth == 0 ? st.st_dev : root_dev);
                free(child);
                free(names[i]);
            }
            free(names);
        }
    }
    if (depth_first && depth >= mindepth && !quit) {
        cur_path = path, cur_st = st, cur_depth = depth, pruned = 0;
        eval(expr);
    }
}

int main(int argc, char **argv)
{
    now = time(0);
    int i = 1;
    while (i < argc && (!strcmp(argv[i], "-H") || !strcmp(argv[i], "-L") || !strcmp(argv[i], "-P")))
        i++;
    int first_path = i;
    while (i < argc && argv[i][0] != '-' && strcmp(argv[i], "(") && strcmp(argv[i], "!"))
        i++;
    int npaths = i - first_path;
    args = argv + i;
    nargs = argc - i;
    ai = 0;
    expr = nargs ? parse_or() : mk(N_TRUE);
    if (ai < nargs)
        die("unexpected `%s'", args[ai]);
    if (!had_action) {
        struct node *n = mk(N_AND);
        n->l = expr;
        n->r = mk(N_PRINT);
        expr = n;
    }
    if (!npaths)
        visit(".", 0, 0);
    for (int k = 0; k < npaths && !quit; k++)
        visit(argv[first_path + k], 0, 0);
    flush_all(expr);
    fflush(stdout);
    return status;
}
