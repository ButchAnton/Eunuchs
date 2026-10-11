/* esh: built-in commands. */
#include "esh.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>

int test_unary(const char *op, const char *arg);
int test_binary(const char *l, const char *op, const char *r, int pattern);

/* ---- options ---- */

char opts[NOPTS];
const char *const opt_names[NOPTS] = {
    [OPT_AUTOCD] = "autocd",
    [OPT_AUTOPUSHD] = "autopushd",
    [OPT_PUSHDIGNOREDUPS] = "pushdignoredups",
    [OPT_PUSHDSILENT] = "pushdsilent",
    [OPT_CORRECT] = "correct",
    [OPT_NOMATCH] = "nomatch",
    [OPT_GLOB] = "glob",
    [OPT_GLOBDOTS] = "globdots",
    [OPT_NOCASEGLOB] = "nocaseglob",
    [OPT_HISTIGNOREDUPS] = "histignoredups",
    [OPT_HISTIGNORESPACE] = "histignorespace",
    [OPT_NOTIFY] = "notify",
    [OPT_XTRACE] = "xtrace",
    [OPT_ERREXIT] = "errexit",
    [OPT_NOUNSET] = "nounset",
    [OPT_INTERACTIVECOMMENTS] = "interactivecomments",
    [OPT_IGNOREEOF] = "ignoreeof",
    [OPT_SHWORDSPLIT] = "shwordsplit",
    [OPT_PROMPTSP] = "promptsp",
    [OPT_AUTOSUGGEST] = "autosuggest",
    [OPT_HIGHLIGHT] = "highlight",
    [OPT_PROMPTMARKS] = "promptmarks",
    [OPT_BEEP] = "beep",
    [OPT_EXTENDEDHIST] = "extendedhistory",
    [OPT_CHECKJOBS] = "checkjobs",
    [OPT_HUP] = "hup",
    [OPT_AUTOLIST] = "autolist",
    [OPT_AUTOMENU] = "automenu",
    [OPT_BANGHIST] = "banghist",
    [OPT_VERBOSE] = "verbose",
    [OPT_INTERACTIVE] = "interactive",
    [OPT_MONITOR] = "monitor",
    [OPT_LOGIN] = "login",
};

static void canon_opt(const char *in, char *out, int len)
{
    int k = 0;
    for (; *in && k < len - 1; in++)
        if (*in != '_' && *in != '-')
            out[k++] = to_lower(*in);
    out[k] = 0;
}

int opt_find(const char *name)
{
    char c[48];
    canon_opt(name, c, sizeof(c));
    for (int i = 0; i < NOPTS; i++)
        if (!strcmp(c, opt_names[i]))
            return i;
    return -1;
}

/* "name", "noname", "NO_NAME": index and the value to set; -1 if unknown. */
static int opt_parse(const char *name, int on, int *value)
{
    int i = opt_find(name);
    if (i >= 0) {
        *value = on;
        return i;
    }
    char c[48];
    canon_opt(name, c, sizeof(c));
    if (!strncmp(c, "no", 2) && (i = opt_find(c + 2)) >= 0) {
        *value = !on;
        return i;
    }
    return -1;
}

/* ---- aliases ---- */

static struct alias *aliases;

const char *alias_get(const char *name)
{
    for (struct alias *a = aliases; a; a = a->next)
        if (!strcmp(a->name, name))
            return a->value;
    return 0;
}

static void alias_set(const char *name, const char *value)
{
    for (struct alias *a = aliases; a; a = a->next)
        if (!strcmp(a->name, name)) {
            free(a->value);
            a->value = xstrdup(value);
            return;
        }
    struct alias *a = xmalloc(sizeof(*a));
    a->name = xstrdup(name);
    a->value = xstrdup(value);
    a->next = aliases;
    aliases = a;
}

static int alias_remove(const char *name)
{
    for (struct alias **pp = &aliases; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, name)) {
            struct alias *a = *pp;
            *pp = a->next;
            free(a->name);
            free(a->value);
            free(a);
            return 0;
        }
    return -1;
}

int alias_names(const char *prefix, struct strv *out)
{
    int n = 0;
    for (struct alias *a = aliases; a; a = a->next)
        if (!strncmp(a->name, prefix, strlen(prefix))) {
            strv_push(out, astrdup(a->name));
            n++;
        }
    return n;
}

/* ---- helpers ---- */

static int usage(const char *cmd, const char *msg)
{
    eprintf("%s: %s\n", cmd, msg);
    return 2;
}

static void print_escaped(const char *s, int *stop)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            putchar(*s);
            continue;
        }
        s++;
        switch (*s) {
        case 'a': putchar(7); break;
        case 'b': putchar(8); break;
        case 'c': *stop = 1; return;
        case 'e': case 'E': putchar(27); break;
        case 'f': putchar(12); break;
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'v': putchar(11); break;
        case '\\': putchar('\\'); break;
        case '0': {
            int v = 0, k = 0;
            while (k < 3 && s[1] >= '0' && s[1] <= '7')
                v = v * 8 + *++s - '0', k++;
            putchar(v);
            break;
        }
        case 'x': {
            int v = 0, k = 0;
            while (k < 2 && ((s[1] >= '0' && s[1] <= '9') || (to_lower(s[1]) >= 'a' && to_lower(s[1]) <= 'f'))) {
                char c = to_lower(*++s);
                v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
                k++;
            }
            putchar(v);
            break;
        }
        default:
            putchar('\\');
            putchar(*s);
        }
    }
}

/* ---- directories ---- */

static char *dirstack[32];
static int ndirs;

static void set_pwd_vars(const char *old)
{
    char cwd[512];
    if (old)
        var_set("OLDPWD", old, V_EXPORT);
    if (getcwd(cwd, sizeof(cwd)))
        var_set("PWD", cwd, V_EXPORT);
}

char *pwd_tilde(const char *path)
{
    const char *home = var_get("HOME");
    int hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/' || !path[hl])) {
        struct sb b = { 0 };
        sb_putc(&b, '~');
        sb_puts(&b, path + hl);
        char *r = sb_adup(&b);
        sb_free(&b);
        return r;
    }
    return astrdup(path);
}

static void dirstack_push(const char *d)
{
    if (opts[OPT_PUSHDIGNOREDUPS])
        for (int i = 0; i < ndirs; i++)
            if (!strcmp(dirstack[i], d)) {
                free(dirstack[i]);
                memmove(dirstack + i, dirstack + i + 1, (ndirs - i - 1) * sizeof(char *));
                ndirs--;
                break;
            }
    if (ndirs == 32) {
        free(dirstack[31]);
        ndirs--;
    }
    memmove(dirstack + 1, dirstack, ndirs * sizeof(char *));
    dirstack[0] = xstrdup(d);
    ndirs++;
}

static void print_dirs(int verbose)
{
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");
    if (verbose) {
        printf("0\t%s\n", pwd_tilde(cwd));
        for (int i = 0; i < ndirs; i++)
            printf("%d\t%s\n", i + 1, pwd_tilde(dirstack[i]));
        return;
    }
    printf("%s", pwd_tilde(cwd));
    for (int i = 0; i < ndirs; i++)
        printf(" %s", pwd_tilde(dirstack[i]));
    printf("\n");
}

static void run_hook(const char *name, const char *arg)
{
    if (!func_lookup(name))
        return;
    char *argv[] = { (char *)name, (char *)arg, 0 };
    int saved = sh.last_status;
    run_function(name, argv, arg ? 2 : 1);
    sh.last_status = saved;
}

int do_cd(const char *dir, int print)
{
    char old[512];
    if (!getcwd(old, sizeof(old)))
        old[0] = 0;
    if (chdir(dir) < 0) {
        const char *why = errno == ENOENT ? "no such file or directory"
                        : errno == ENOTDIR ? "not a directory"
                        : errno == EACCES ? "permission denied" : strerror(errno);
        eprintf("cd: %s: %s\n", why, dir);
        return 1;
    }
    if (opts[OPT_AUTOPUSHD] && old[0] && strcmp(old, dir))
        dirstack_push(old);
    set_pwd_vars(old[0] ? old : 0);
    if (print) {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)))
            printf("%s\n", pwd_tilde(cwd));
    }
    run_hook("chpwd", 0);
    return 0;
}

static int b_cd(int argc, char **argv)
{
    if (argc > 3)
        return usage(argv[0], "too many arguments");
    if (argc == 3) {
        /* cd old new: replace old with new in $PWD */
        char cwd[512];
        if (!getcwd(cwd, sizeof(cwd)))
            return 1;
        char *at = strstr(cwd, argv[1]);
        if (!at) {
            eprintf("cd: string not in pwd: %s\n", argv[1]);
            return 1;
        }
        struct sb b = { 0 };
        sb_putn(&b, cwd, at - cwd);
        sb_puts(&b, argv[2]);
        sb_puts(&b, at + strlen(argv[1]));
        int r = do_cd(sb_str(&b), 1);
        sb_free(&b);
        return r;
    }
    const char *dir = argc > 1 ? argv[1] : var_get("HOME");
    if (!dir || !*dir)
        dir = "/";
    if (!strcmp(dir, "-")) {
        const char *o = var_get("OLDPWD");
        if (!o) {
            eprintf("cd: no previous directory\n");
            return 1;
        }
        return do_cd(astrdup(o), 1);
    }
    if ((dir[0] == '+' || dir[0] == '-') && is_number(dir + 1)) {
        int k = atoi(dir + 1);
        int idx = dir[0] == '+' ? k : ndirs - k;
        if (idx < 1 || idx > ndirs) {
            eprintf("cd: no such entry in dir stack\n");
            return 1;
        }
        char *d = astrdup(dirstack[idx - 1]);
        free(dirstack[idx - 1]);
        memmove(dirstack + idx - 1, dirstack + idx, (ndirs - idx) * sizeof(char *));
        ndirs--;
        return do_cd(d, 1);
    }
    return do_cd(dir, 0);
}

static int b_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        eprintf("pwd: %s\n", strerror(errno));
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int b_pushd(int argc, char **argv)
{
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd)))
        return 1;
    const char *target;
    if (argc < 2) {
        if (!ndirs) {
            eprintf("pushd: no other directory\n");
            return 1;
        }
        target = astrdup(dirstack[0]);
        free(dirstack[0]);
        memmove(dirstack, dirstack + 1, (ndirs - 1) * sizeof(char *));
        ndirs--;
    } else if ((argv[1][0] == '+' || argv[1][0] == '-') && is_number(argv[1] + 1)) {
        int k = atoi(argv[1] + 1);
        int idx = argv[1][0] == '+' ? k : ndirs - k;
        if (idx < 1 || idx > ndirs) {
            eprintf("pushd: no such entry in dir stack\n");
            return 1;
        }
        /* rotate so entry idx becomes the current directory */
        char *all[33];
        int n = 0;
        all[n++] = xstrdup(cwd);
        for (int i = 0; i < ndirs; i++)
            all[n++] = dirstack[i];
        target = astrdup(all[idx]);
        ndirs = 0;
        for (int i = 1; i < n; i++)
            dirstack[ndirs++] = all[(idx + i) % n];
        free(all[idx]);
        if (chdir(target) < 0) {
            eprintf("pushd: %s: %s\n", target, strerror(errno));
            return 1;
        }
        set_pwd_vars(cwd);
        if (!opts[OPT_PUSHDSILENT])
            print_dirs(0);
        return 0;
    } else {
        target = !strcmp(argv[1], "-") ? var_get("OLDPWD") : argv[1];
        if (!target) {
            eprintf("pushd: no previous directory\n");
            return 1;
        }
    }
    if (chdir(target) < 0) {
        eprintf("pushd: %s: %s\n", strerror(errno), target);
        return 1;
    }
    dirstack_push(cwd);
    set_pwd_vars(cwd);
    if (!opts[OPT_PUSHDSILENT])
        print_dirs(0);
    run_hook("chpwd", 0);
    return 0;
}

static int b_popd(int argc, char **argv)
{
    if (!ndirs) {
        eprintf("popd: directory stack empty\n");
        return 1;
    }
    if (argc > 1 && (argv[1][0] == '+' || argv[1][0] == '-') && is_number(argv[1] + 1)) {
        int k = atoi(argv[1] + 1);
        int idx = argv[1][0] == '+' ? k : ndirs - k;
        if (idx < 1 || idx > ndirs) {
            eprintf("popd: no such entry in dir stack\n");
            return 1;
        }
        free(dirstack[idx - 1]);
        memmove(dirstack + idx - 1, dirstack + idx, (ndirs - idx) * sizeof(char *));
        ndirs--;
        if (!opts[OPT_PUSHDSILENT])
            print_dirs(0);
        return 0;
    }
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = 0;
    char *d = dirstack[0];
    memmove(dirstack, dirstack + 1, (ndirs - 1) * sizeof(char *));
    ndirs--;
    int r = chdir(d);
    if (r < 0)
        eprintf("popd: %s: %s\n", strerror(errno), d);
    else
        set_pwd_vars(cwd);
    free(d);
    if (r == 0 && !opts[OPT_PUSHDSILENT])
        print_dirs(0);
    if (r == 0)
        run_hook("chpwd", 0);
    return r < 0;
}

static int b_dirs(int argc, char **argv)
{
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            for (int k = 0; k < ndirs; k++)
                free(dirstack[k]);
            ndirs = 0;
            return 0;
        }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-p"))
            verbose = 1;
    }
    print_dirs(verbose);
    return 0;
}

/* ---- output ---- */

static int b_echo(int argc, char **argv)
{
    int nl = 1, esc = 1, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        int ok = 1;
        for (const char *c = f; *c; c++)
            if (!strchr("neE", *c))
                ok = 0;
        if (!ok)
            break;
        for (const char *c = f; *c; c++) {
            if (*c == 'n') nl = 0;
            else if (*c == 'e') esc = 1;
            else if (*c == 'E') esc = 0;
        }
    }
    int stop = 0;
    for (int k = i; k < argc && !stop; k++) {
        if (k > i)
            putchar(' ');
        if (esc)
            print_escaped(argv[k], &stop);
        else
            printf("%s", argv[k]);
    }
    if (nl && !stop)
        putchar('\n');
    fflush_stdout();
    return 0;
}

static int b_print(int argc, char **argv)
{
    int nl = 1, raw = 0, lines = 0, nul = 0, prompt = 0, hist = 0, fd = 1, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        const char *f = argv[i] + 1;
        int ok = 1;
        for (const char *c = f; *c; c++)
            if (!strchr("nrlNPsRu0123456789", *c))
                ok = 0;
        if (!ok)
            break;
        for (const char *c = f; *c; c++) {
            switch (*c) {
            case 'n': nl = 0; break;
            case 'r': case 'R': raw = 1; break;
            case 'l': lines = 1; break;
            case 'N': nul = 1; break;
            case 'P': prompt = 1; break;
            case 's': hist = 1; break;
            case 'u':
                if (c[1]) {
                    fd = atoi(c + 1);
                    c += strlen(c) - 1;
                } else if (i + 1 < argc) {
                    fd = atoi(argv[++i]);
                }
                break;
            }
        }
    }
    struct sb out = { 0 };
    for (int k = i; k < argc; k++) {
        if (k > i)
            sb_putc(&out, nul ? 0 : lines ? '\n' : ' ');
        const char *s = argv[k];
        if (prompt) {
            int w;
            s = prompt_expand(s, &w, isatty(fd));
        }
        sb_puts(&out, s);
    }
    if (hist) {
        hist_add(sb_str(&out));
        sb_free(&out);
        return 0;
    }
    if (raw || prompt) {
        if (nl)
            sb_putc(&out, nul ? 0 : '\n');
        out_flush();
        write(fd, out.s ? out.s : "", out.len);
    } else {
        int stop = 0;
        if (fd != 1) {
            out_flush();
            int saved = dup(1);
            dup2(fd, 1);
            print_escaped(sb_str(&out), &stop);
            if (nl && !stop)
                putchar(nul ? 0 : '\n');
            out_flush();
            dup2(saved, 1);
            close(saved);
        } else {
            print_escaped(sb_str(&out), &stop);
            if (nl && !stop)
                putchar(nul ? 0 : '\n');
        }
    }
    sb_free(&out);
    fflush_stdout();
    return 0;
}

static int b_printf(int argc, char **argv)
{
    if (argc < 2)
        return usage("printf", "usage: printf format [args ...]");
    int ai = 2, status = 0;
    const char *fmt = argv[1];
    do {
        int used = 0;
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                int stop = 0;
                char e[8] = { '\\', 0 };
                int k = 1;
                p++;
                if (*p == '0' || *p == 'x') {
                    e[k++] = *p;
                    while (k < 5 && p[1] && ((p[1] >= '0' && p[1] <= '9') ||
                                             (to_lower(p[1]) >= 'a' && to_lower(p[1]) <= 'f')))
                        e[k++] = *++p;
                } else if (*p) {
                    e[k++] = *p;
                } else {
                    p--;
                }
                e[k] = 0;
                print_escaped(e, &stop);
                if (stop)
                    return status;
                continue;
            }
            if (*p != '%') {
                putchar(*p);
                continue;
            }
            if (p[1] == '%') {
                putchar('%');
                p++;
                continue;
            }
            char spec[32];
            int k = 0;
            spec[k++] = '%';
            p++;
            while (*p && strchr("-+ #0", *p) && k < 20)
                spec[k++] = *p++;
            while (*p && (is_digit(*p) || *p == '*') && k < 24) {
                if (*p == '*') {
                    const char *a = ai < argc ? argv[ai++] : "0";
                    used = 1;
                    k += snprintf(spec + k, sizeof(spec) - k, "%d", atoi(a));
                    p++;
                } else {
                    spec[k++] = *p++;
                }
            }
            if (*p == '.') {
                spec[k++] = *p++;
                while (*p && (is_digit(*p) || *p == '*') && k < 28) {
                    if (*p == '*') {
                        const char *a = ai < argc ? argv[ai++] : "0";
                        used = 1;
                        k += snprintf(spec + k, sizeof(spec) - k, "%d", atoi(a));
                        p++;
                    } else {
                        spec[k++] = *p++;
                    }
                }
            }
            char conv = *p;
            if (!conv)
                break;
            const char *arg = ai < argc ? argv[ai++] : 0;
            if (arg)
                used = 1;
            char buf[512];
            switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c': {
                long long v = 0;
                if (conv == 'c') {
                    v = arg ? (unsigned char)arg[0] : 0;
                    spec[k++] = 'c';
                } else {
                    if (arg && (arg[0] == '\'' || arg[0] == '"'))
                        v = (unsigned char)arg[1];
                    else if (arg) {
                        char *end;
                        v = strtol(arg, &end, 0);
                        if (*end) {
                            eprintf("printf: %s: invalid number\n", arg);
                            status = 1;
                        }
                    }
                    spec[k++] = 'l';
                    spec[k++] = 'l';
                    spec[k++] = conv == 'i' ? 'd' : conv;
                }
                spec[k] = 0;
                if (conv == 'c' && !v)
                    break;
                snprintf(buf, sizeof(buf), spec, v);
                printf("%s", buf);
                break;
            }
            case 's': case 'b': case 'q': {
                spec[k++] = 's';
                spec[k] = 0;
                const char *s = arg ? arg : "";
                if (conv == 'q')
                    s = quote_word(s);
                if (conv == 'b') {
                    int stop = 0;
                    print_escaped(s, &stop);
                    if (stop)
                        return status;
                    break;
                }
                snprintf(buf, sizeof(buf), spec, s);
                printf("%s", buf);
                break;
            }
            default:
                eprintf("printf: %%%c: invalid directive\n", conv);
                return 1;
            }
        }
        if (!used)
            break;
    } while (ai < argc);
    fflush_stdout();
    return status;
}

/* ---- read ---- */

static int b_read(int argc, char **argv)
{
    int raw = 0, silent = 0, nchars = -1, timeout = -1, i = 1;
    const char *prompt = 0, *array = 0;
    char delim = '\n';
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *c = argv[i] + 1; *c; c++) {
            const char *val = 0;
            if (strchr("ptnkAd", *c)) {
                val = c[1] ? c + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!val)
                    return usage("read", "option requires an argument");
            }
            switch (*c) {
            case 'r': raw = 1; break;
            case 's': silent = 1; break;
            case 'p': prompt = val; break;
            case 't': timeout = atoi(val); break;
            case 'n': case 'k': nchars = atoi(val); break;
            case 'A': array = val; break;
            case 'd': delim = *val; break;
            case 'e': case 'q': case 'E': break;
            default:
                return usage("read", "usage: read [-rs] [-p prompt] [-t secs] [-n n] [-A array] [name ...]");
            }
            if (val)
                break;
        }
    }
    char **names = argv + i;
    int nnames = argc - i;
    char *q = nnames ? strchr(names[0], '?') : 0;
    if (q) {                            /* zsh: read 'name?prompt' */
        prompt = q + 1;
        names[0] = astrndup(names[0], q - names[0]);
    }
    if (prompt && isatty(0)) {
        out_flush();
        write(2, prompt, strlen(prompt));
    }
    struct termios saved;
    int restore = 0;
    if ((silent || nchars > 0) && isatty(0) && tcgetattr(0, &saved) == 0) {
        struct termios t = saved;
        if (silent)
            t.c_lflag &= ~ECHO;
        if (nchars > 0)
            t.c_lflag &= ~ICANON;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        restore = 1;
    }
    struct sb line = { 0 };
    int got_any = 0, eof = 0, timed_out = 0, count = 0;
    for (;;) {
        if (timeout >= 0) {
            struct pollfd pfd = { .fd = 0, .events = POLLIN };
            if (poll(&pfd, 1, timeout * 1000) <= 0) {
                timed_out = 1;
                break;
            }
        }
        char c;
        long n = read(0, &c, 1);
        if (n < 0 && errno == EINTR) {
            if (got_sigint)
                break;
            continue;
        }
        if (n <= 0) {
            eof = 1;
            break;
        }
        got_any = 1;
        if (nchars > 0) {
            sb_putc(&line, c);
            if (++count >= nchars)
                break;
            continue;
        }
        if (c == delim)
            break;
        if (c == '\\' && !raw) {
            n = read(0, &c, 1);
            if (n <= 0)
                break;
            if (c == '\n')
                continue;
            sb_putc(&line, '\\');
        }
        if (c == '\r' && delim == '\n')
            continue;
        sb_putc(&line, c);
    }
    if (restore) {
        tcsetattr(0, TCSANOW, &saved);
        if (silent)
            write(2, "\n", 1);
    }
    char *s = sb_adup(&line);
    sb_free(&line);
    if (!raw) {
        char *o = s;
        for (char *p = s; *p; p++) {
            if (*p == '\\' && p[1])
                p++;
            *o++ = *p;
        }
        *o = 0;
    }
    const char *ifs = var_get("IFS");
    if (!ifs)
        ifs = " \t\n";
    if (array || nnames > 1) {
        /* split into fields */
        struct strv f = { 0 };
        char *p = s;
        int max = array ? 1 << 30 : nnames;
        while (*p) {
            while (*p && strchr(ifs, *p) && is_space(*p))
                p++;
            if (!*p)
                break;
            if (f.n == max - 1) {
                /* the last name gets the rest (trailing IFS whitespace removed) */
                char *e = p + strlen(p);
                while (e > p && strchr(ifs, e[-1]) && is_space(e[-1]))
                    e--;
                strv_push(&f, astrndup(p, e - p));
                break;
            }
            char *st = p;
            while (*p && !strchr(ifs, *p))
                p++;
            strv_push(&f, astrndup(st, p - st));
            if (*p && strchr(ifs, *p) && !is_space(*p))
                p++;
        }
        if (array) {
            var_set_array(array, f.v, f.n, 0);
        } else {
            for (int k = 0; k < nnames; k++)
                if (var_set(names[k], k < f.n ? f.v[k] : "", 0) < 0)
                    return 1;
        }
    } else {
        /* one name: strip leading/trailing IFS whitespace */
        char *p = s;
        if (nnames) {
            while (*p && strchr(ifs, *p) && is_space(*p))
                p++;
            char *e = p + strlen(p);
            while (e > p && strchr(ifs, e[-1]) && is_space(e[-1]))
                e--;
            *e = 0;
        }
        if (var_set(nnames ? names[0] : "REPLY", p, 0) < 0)
            return 1;
    }
    if (timed_out)
        return 1;
    return eof && !got_any ? 1 : eof ? 1 : 0;
}

/* ---- variables ---- */

int is_declare_builtin(const char *name)
{
    return !strcmp(name, "local") || !strcmp(name, "typeset") || !strcmp(name, "declare") ||
           !strcmp(name, "export") || !strcmp(name, "readonly") || !strcmp(name, "integer");
}

static int filter_flags;
static int by_flags(struct var *v)
{
    return (v->flags & filter_flags) == filter_flags;
}

int builtin_declare(char **raw, int n)
{
    const char *cmd = raw[0];
    int set = 0, clear = 0, global = 0, print = 0, funcs = 0, unexport = 0;
    if (!strcmp(cmd, "export"))
        set |= V_EXPORT, global = 1;
    else if (!strcmp(cmd, "readonly"))
        set |= V_READONLY, global = 1;
    else if (!strcmp(cmd, "integer"))
        set |= V_INTEGER;
    int local = !global && sh.func_depth > 0;
    int status = 0, i = 1;
    /* options (expanded normally) */
    for (; i < n; i++) {
        char *w = expand_str(raw[i], EX_TILDE);
        if (!w)
            return 1;
        if (w[0] != '-' && w[0] != '+')
            break;
        if (!strcmp(w, "--")) {
            i++;
            break;
        }
        int on = w[0] == '-';
        for (char *c = w + 1; *c; c++) {
            int f = 0;
            switch (*c) {
            case 'x': f = V_EXPORT; break;
            case 'r': f = V_READONLY; break;
            case 'i': f = V_INTEGER; break;
            case 'a': case 'A': f = V_ARRAY; break;
            case 'l': f = V_LOWER; break;
            case 'u': f = V_UPPER; break;
            case 'g': global = 1; local = 0; break;
            case 'p': print = 1; break;
            case 'f': funcs = 1; break;
            case 'n': unexport = 1; break;
            default:
                eprintf("%s: bad option: -%c\n", cmd, *c);
                return 1;
            }
            if (on)
                set |= f;
            else
                clear |= f;
        }
    }
    if (funcs) {
        if (i >= n) {
            funcs_list();
            return 0;
        }
        for (; i < n; i++) {
            struct func *f = func_lookup(raw[i]);
            if (f)
                printf("%s () %s\n", f->name, f->body);
            else
                status = 1;
        }
        return status;
    }
    if (i >= n) {
        filter_flags = set & ~V_ARRAY;
        vars_list(filter_flags ? by_flags : 0, !strcmp(cmd, "export") || print);
        return 0;
    }
    if (!strcmp(cmd, "export") && unexport) {
        set &= ~V_EXPORT;
        clear |= V_EXPORT;
    }
    for (; i < n; i++) {
        const char *w = raw[i];
        struct assign *a = 0;
        char *name;
        if (is_assignment_word(w)) {
            a = parse_assignment(w);
            if (!a)
                return 1;
            name = a->name;
        } else {
            name = expand_str(w, EX_TILDE);
            if (!name)
                return 1;
            if (!is_name(name)) {
                eprintf("%s: not an identifier: %s\n", cmd, name);
                status = 1;
                continue;
            }
        }
        if (local)
            var_local(name);
        struct var *v = var_lookup(name);
        if (!a) {
            if (!v || (v->flags & V_UNSET)) {
                if (set & V_ARRAY)
                    var_set_array(name, 0, 0, 0);
                else if (set & V_EXPORT)
                    var_export(name);
                else if (local || (set & ~V_EXPORT))
                    var_set(name, (set & V_INTEGER) ? "0" : "", 0);
                else if (print)
                    continue;
                v = var_lookup(name);
            } else if (print && !set && !clear) {
                const char *val = var_get(name);
                printf("%s=%s\n", name, quote_word(val ? val : ""));
                continue;
            }
            if (v)
                v->flags = (v->flags | (set & ~V_ARRAY)) & ~(clear & ~V_ARRAY);
            continue;
        }
        if (v && (v->flags & V_READONLY)) {
            eprintf("%s: read-only variable: %s\n", cmd, name);
            status = 1;
            continue;
        }
        /* attributes first, so integer/case conversions apply to the value */
        if (v)
            v->flags = (v->flags | (set & ~(V_READONLY | V_ARRAY))) & ~clear;
        else if (set & ~(V_READONLY | V_ARRAY)) {
            var_set(name, "", set & ~(V_READONLY | V_ARRAY));
        }
        if (assign_do(a, set & V_EXPORT) < 0) {
            status = 1;
            continue;
        }
        v = var_lookup(name);
        if (v)
            v->flags = (v->flags | (set & ~V_ARRAY)) & ~(clear & ~V_ARRAY);
    }
    return status;
}

static int b_unset(int argc, char **argv)
{
    int funcs = 0, status = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) {
            funcs = 1;
            continue;
        }
        if (!strcmp(argv[i], "-v"))
            continue;
        if (funcs)
            func_remove(argv[i]);
        else if (var_unset(argv[i]) < 0)
            status = 1;
    }
    return status;
}

static int b_set(int argc, char **argv)
{
    if (argc == 1) {
        vars_list(0, 0);
        return 0;
    }
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            pos_set(argv + i, argc - i);
            return 0;
        }
        if ((a[0] != '-' && a[0] != '+') || !a[1])
            break;
        int on = a[0] == '-';
        if (!strcmp(a + 1, "o")) {
            if (i + 1 >= argc) {
                for (int k = 0; k < NOPTS; k++)
                    printf("%-22s%s\n", opt_names[k], opts[k] ? "on" : "off");
                return 0;
            }
            int v, k = opt_parse(argv[++i], on, &v);
            if (k < 0) {
                eprintf("set: no such option: %s\n", argv[i]);
                return 1;
            }
            opts[k] = v;
            continue;
        }
        if (!strcmp(a + 1, "A")) {
            if (i + 1 >= argc)
                return usage("set", "-A: name expected");
            const char *name = argv[++i];
            return var_set_array(name, argv + i + 1, argc - i - 1, 0) < 0;
        }
        for (const char *c = a + 1; *c; c++) {
            int k;
            switch (*c) {
            case 'e': k = OPT_ERREXIT; break;
            case 'u': k = OPT_NOUNSET; break;
            case 'x': k = OPT_XTRACE; break;
            case 'v': k = OPT_VERBOSE; break;
            case 'f': opts[OPT_GLOB] = !on; continue;
            case 'm': k = OPT_MONITOR; break;
            case 'C': continue;
            default:
                eprintf("set: bad option: %c%c\n", a[0], *c);
                return 1;
            }
            opts[k] = on;
        }
    }
    if (i < argc)
        pos_set(argv + i, argc - i);
    return 0;
}

static int b_setopt(int argc, char **argv)
{
    int on = argv[0][0] == 's';
    if (argc == 1) {
        for (int k = 0; k < NOPTS; k++)
            if (opts[k] == on)
                printf("%s\n", opt_names[k]);
        return 0;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        int v, k = opt_parse(argv[i], on, &v);
        if (k < 0) {
            eprintf("%s: no such option: %s\n", argv[0], argv[i]);
            st = 1;
            continue;
        }
        opts[k] = v;
    }
    return st;
}

static int b_shift(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > pos.n) {
        eprintf("shift: shift count must be <= $#\n");
        return 1;
    }
    char **rest = amalloc((pos.n - n + 1) * sizeof(char *));
    for (int i = n; i < pos.n; i++)
        rest[i - n] = astrdup(pos.v[i]);
    pos_set(rest, pos.n - n);
    return 0;
}

/* ---- control flow ---- */

static int source_depth;

static int b_source(int argc, char **argv)
{
    if (argc < 2)
        return usage(argv[0], "filename argument required");
    const char *file = argv[1];
    struct stat st;
    if (!strchr(file, '/') && stat(file, &st) < 0) {
        char *p = path_lookup(file);
        if (p)
            file = p;
    }
    struct posargs saved = { 0 };
    int with_args = argc > 2;
    if (with_args) {
        saved = pos_save();
        pos_set(argv + 2, argc - 2);
    }
    source_depth++;
    int r = run_file(file, 1);
    source_depth--;
    if (with_args)
        pos_restore(saved);
    return r;
}

static int b_exit(int argc, char **argv)
{
    static int warned;
    int code = argc > 1 ? atoi(argv[1]) : sh.last_status;
    if (sh.interactive && !sh.in_child && opts[OPT_CHECKJOBS] && jobs_any_stopped() && !warned &&
        sh.func_depth == 0) {
        eprintf("esh: you have suspended jobs.\n");
        warned = 1;
        return 1;
    }
    sh_exit(code & 0xff);
}

static int b_return(int argc, char **argv)
{
    if (sh.func_depth == 0 && source_depth == 0) {
        eprintf("return: not in a function or sourced file\n");
        return 1;
    }
    sh.exit_status = argc > 1 ? atoi(argv[1]) & 0xff : sh.last_status;
    sh.returning = 1;
    return sh.exit_status;
}

static int b_break(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (sh.loop_depth == 0) {
        eprintf("%s: not in while, until, select, or repeat loop\n", argv[0]);
        return 1;
    }
    if (n < 1)
        n = 1;
    if (n > sh.loop_depth)
        n = sh.loop_depth;
    if (argv[0][0] == 'b')
        sh.breaking = n;
    else
        sh.continuing = n;
    return 0;
}

static int b_eval(int argc, char **argv)
{
    struct sb b = { 0 };
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            sb_putc(&b, ' ');
        sb_puts(&b, argv[i]);
    }
    char *code = xstrdup(sb_str(&b));
    sb_free(&b);
    sh.last_status = 0;
    int st = run_string(code, 0);
    free(code);
    return st;
}

static int b_true(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int b_false(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

static int b_let(int argc, char **argv)
{
    long long v = 0;
    for (int i = 1; i < argc; i++)
        if (arith_eval(argv[i], &v) < 0)
            return 2;
    return v == 0;
}

/* ---- aliases and lookup ---- */

static int b_alias(int argc, char **argv)
{
    int st = 0;
    if (argc == 1) {
        struct strv names = { 0 };
        alias_names("", &names);
        sort_strs(names.v, names.n);
        for (int i = 0; i < names.n; i++)
            printf("%s=%s\n", names.v[i], quote_word(alias_get(names.v[i])));
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "-L") || !strcmp(argv[i], "--"))
            continue;
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = alias_get(argv[i]);
            if (v)
                printf("%s=%s\n", argv[i], quote_word(v));
            else
                st = 1;
            continue;
        }
        char *name = astrndup(argv[i], eq - argv[i]);
        alias_set(name, eq + 1);
    }
    return st;
}

static int b_unalias(int argc, char **argv)
{
    int st = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) {
            while (aliases)
                alias_remove(aliases->name);
            continue;
        }
        if (alias_remove(argv[i]) < 0) {
            eprintf("unalias: no such hash table element: %s\n", argv[i]);
            st = 1;
        }
    }
    return st;
}

/* style: 'v' (type), 'c' (which), 'w' (whence), 'p' (path only) */
static int describe(const char *name, int style, int all)
{
    int found = 0;
    const char *a = style != 'p' ? alias_get(name) : 0;
    if (a) {
        found = 1;
        if (style == 'v')
            printf("%s is an alias for %s\n", name, a);
        else if (style == 'c')
            printf("%s: aliased to %s\n", name, a);
        else
            printf("%s\n", a);
        if (!all)
            return 0;
    }
    if (style != 'p' && is_keyword(name)) {
        found = 1;
        if (style == 'v')
            printf("%s is a reserved word\n", name);
        else if (style == 'c')
            printf("%s: shell reserved word\n", name);
        else
            printf("%s\n", name);
        if (!all)
            return 0;
    }
    struct func *f = style != 'p' ? func_lookup(name) : 0;
    if (f) {
        found = 1;
        if (style == 'v')
            printf("%s is a shell function\n", name);
        else if (style == 'c')
            printf("%s () %s\n", name, f->body);
        else
            printf("%s\n", name);
        if (!all)
            return 0;
    }
    if (style != 'p' && builtin_lookup(name)) {
        found = 1;
        if (style == 'v')
            printf("%s is a shell builtin\n", name);
        else if (style == 'c')
            printf("%s: shell built-in command\n", name);
        else
            printf("%s\n", name);
        if (!all)
            return 0;
    }
    char *p = path_lookup(name);
    if (p) {
        found = 1;
        if (style == 'v')
            printf("%s is %s\n", name, p);
        else
            printf("%s\n", p);
        return 0;
    }
    if (!found) {
        if (style == 'v' || style == 'c')
            printf("%s not found\n", name);
        return 1;
    }
    return 0;
}

static int lookup_cmd(int argc, char **argv, int style)
{
    int st = 0, all = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'a') all = 1;
            else if (*c == 'v') style = 'v';
            else if (*c == 'c') style = 'c';
            else if (*c == 'p') style = 'p';
            else if (*c == 'w') style = 'w';
        }
    for (; i < argc; i++)
        st |= describe(argv[i], style, all);
    return st;
}

static int b_type(int argc, char **argv) { return lookup_cmd(argc, argv, 'v'); }
static int b_which(int argc, char **argv) { return lookup_cmd(argc, argv, 'c'); }
static int b_whence(int argc, char **argv) { return lookup_cmd(argc, argv, 'w'); }

static int b_command(int argc, char **argv)
{
    /* only reached with an option: command -v / -V */
    if (argc > 2 && !strcmp(argv[1], "-v"))
        return lookup_cmd(argc - 1, argv + 1, 'w');
    if (argc > 2 && !strcmp(argv[1], "-V"))
        return lookup_cmd(argc - 1, argv + 1, 'v');
    if (argc > 2 && !strcmp(argv[1], "-p")) {
        /* run with the default path */
        return usage("command", "-p is not supported");
    }
    return 0;
}

static int b_builtin(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int b_rehash(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    path_rehash();
    return 0;
}

static int b_functions(int argc, char **argv)
{
    if (argc == 1) {
        funcs_list();
        return 0;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        struct func *f = func_lookup(argv[i]);
        if (f)
            printf("%s () %s\n", f->name, f->body);
        else
            st = 1;
    }
    return st;
}

static int b_unfunction(int argc, char **argv)
{
    int st = 0;
    for (int i = 1; i < argc; i++)
        if (func_remove(argv[i]) < 0) {
            eprintf("unfunction: no such hash table element: %s\n", argv[i]);
            st = 1;
        }
    return st;
}

/* ---- jobs ---- */

static int b_jobs(int argc, char **argv)
{
    int longfmt = 0, pids = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l"))
            longfmt = 1;
        else if (!strcmp(argv[i], "-p"))
            pids = 1;
    }
    jobs_reap(0);
    out_flush();
    for (int i = 0; i < MAXJOBS; i++) {
        struct job *j = jobtab[i];
        if (!j)
            continue;
        job_print(j, longfmt, pids);
        if (!j->stopped && !j->fg) {
            /* finished jobs are reported (and forgotten) here */
            int live = 0;
            for (int k = 0; k < j->nprocs; k++)
                live |= j->done[k] != 1;
            if (!live)
                job_free(j);
        }
    }
    return 0;
}

static int b_fg(int argc, char **argv)
{
    int fg = argv[0][0] == 'f';
    if (!opts[OPT_MONITOR] && fg) {
        eprintf("fg: no job control in this shell.\n");
        return 1;
    }
    struct job *j = job_find(argc > 1 ? argv[1] : 0);
    if (!j) {
        eprintf("%s: %s\n", argv[0], argc > 1 ? "no such job" : "no current job");
        return 1;
    }
    int i = job_index(j);
    char mark = i == cur_job ? '+' : i == prev_job ? '-' : ' ';
    const char *state = j->stopped ? "continued" : "running";
    eprintf("[%d]  %c %s  %s\n", j->id, mark, state, j->text);
    return job_continue(j, fg);
}

static int b_wait(int argc, char **argv)
{
    int st = 0;
    if (argc == 1) {
        for (int i = 0; i < MAXJOBS; i++) {
            struct job *j = jobtab[i];
            if (j && !j->stopped) {
                int saved = sh.interactive;
                sh.interactive = 0;
                st = job_wait_fg(j);
                sh.interactive = saved;
            }
        }
        return st;
    }
    for (int i = 1; i < argc; i++) {
        struct job *j = job_find(argv[i]);
        if (!j) {
            eprintf("wait: %s: no such job\n", argv[i]);
            st = 127;
            continue;
        }
        int saved = sh.interactive;
        sh.interactive = 0;
        st = job_wait_fg(j);
        sh.interactive = saved;
    }
    return st;
}

static int b_kill(int argc, char **argv)
{
    int sig = SIGTERM, i = 1, st = 0;
    if (argc < 2)
        return usage("kill", "usage: kill [-s sig | -sig] pid|%job ...  or  kill -l [sig]");
    if (!strcmp(argv[1], "-l") || !strcmp(argv[1], "-L")) {
        if (argc > 2) {
            for (i = 2; i < argc; i++) {
                int n = atoi(argv[i]);
                if (n > 128)
                    n -= 128;
                printf("%s\n", is_number(argv[i]) ? signame(n) : argv[i]);
            }
            return 0;
        }
        for (int s = 1; s < NSIG; s++)
            if (strcmp(signame(s), "?"))
                printf("%s%s", s > 1 ? " " : "", signame(s));
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "-s") || !strcmp(argv[1], "-n")) {
        if (argc < 3)
            return usage("kill", "-s requires a signal");
        sig = sig_by_name(argv[2]);
        i = 3;
    } else if (argv[1][0] == '-' && argv[1][1]) {
        sig = sig_by_name(argv[1] + 1);
        i = 2;
    }
    if (sig < 0) {
        eprintf("kill: unknown signal: %s\n", argv[i - 1]);
        eprintf("kill: type kill -l for a list of signals\n");
        return 1;
    }
    for (; i < argc; i++) {
        pid_t target;
        if (argv[i][0] == '%') {
            struct job *j = job_find(argv[i]);
            if (!j) {
                eprintf("kill: %s: no such job\n", argv[i]);
                st = 1;
                continue;
            }
            target = -j->pgid;
            if (j->stopped && sig != SIGSTOP && sig != SIGTSTP && sig != SIGCONT && sig) {
                kill(target, sig);
                sig = SIGCONT;          /* a stopped job must wake up to die */
            }
        } else if (is_number(argv[i])) {
            target = atoi(argv[i]);
        } else {
            eprintf("kill: illegal pid: %s\n", argv[i]);
            st = 1;
            continue;
        }
        if (kill(target, sig) < 0 && !(sig == SIGCONT && target < 0)) {
            eprintf("kill: kill %s failed: %s\n", argv[i],
                    errno == ESRCH ? "no such process" : strerror(errno));
            st = 1;
        }
    }
    return st;
}

static int b_disown(int argc, char **argv)
{
    struct job *j = job_find(argc > 1 ? argv[1] : 0);
    if (!j) {
        eprintf("disown: no current job\n");
        return 1;
    }
    if (j->stopped)
        eprintf("esh: warning: job is suspended, use `kill -CONT -%d' to resume\n", j->pgid);
    j->disowned = 1;
    job_free(j);
    return 0;
}

/* ---- test ---- */

static int t_argc, t_pos;
static char **t_argv;

static int t_or(void);

static int t_prim(void)
{
    if (t_pos >= t_argc) {
        eprintf("test: argument expected\n");
        return -1;
    }
    const char *a = t_argv[t_pos];
    if (!strcmp(a, "!")) {
        t_pos++;
        int r = t_prim();
        return r < 0 ? r : !r;
    }
    if (!strcmp(a, "(") ) {
        t_pos++;
        int r = t_or();
        if (t_pos >= t_argc || strcmp(t_argv[t_pos], ")")) {
            eprintf("test: ')' expected\n");
            return -1;
        }
        t_pos++;
        return r;
    }
    /* binary? */
    if (t_pos + 2 < t_argc) {
        const char *op = t_argv[t_pos + 1];
        static const char *const bin[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt",
                                           "-le", "-gt", "-ge", "-nt", "-ot", "-ef", 0 };
        for (int k = 0; bin[k]; k++)
            if (!strcmp(op, bin[k])) {
                int r = test_binary(a, op, t_argv[t_pos + 2], 0);
                t_pos += 3;
                return r;
            }
    }
    if (a[0] == '-' && a[1] && !a[2] && t_pos + 1 < t_argc && strchr("abcdefghknprstuwxzLOGSNov", a[1])) {
        int r = test_unary(a, t_argv[t_pos + 1]);
        t_pos += 2;
        return r;
    }
    t_pos++;
    return *a != 0;
}

static int t_and(void)
{
    int r = t_prim();
    while (r >= 0 && t_pos < t_argc && !strcmp(t_argv[t_pos], "-a")) {
        t_pos++;
        int s = t_prim();
        if (s < 0)
            return s;
        r = r && s;
    }
    return r;
}

static int t_or(void)
{
    int r = t_and();
    while (r >= 0 && t_pos < t_argc && !strcmp(t_argv[t_pos], "-o")) {
        t_pos++;
        int s = t_and();
        if (s < 0)
            return s;
        r = r || s;
    }
    return r;
}

static int b_test(int argc, char **argv)
{
    if (!strcmp(argv[0], "[")) {
        if (strcmp(argv[argc - 1], "]")) {
            eprintf("[: ']' expected\n");
            return 2;
        }
        argc--;
    }
    t_argv = argv + 1;
    t_argc = argc - 1;
    t_pos = 0;
    if (t_argc == 0)
        return 1;
    int r = t_or();
    if (r >= 0 && t_pos < t_argc) {
        eprintf("test: too many arguments\n");
        return 2;
    }
    return r < 0 ? 2 : !r;
}

/* ---- misc ---- */

static int b_history(int argc, char **argv)
{
    int count = 16, numbers = 1, all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            hist_clear();
            return 0;
        }
        if (!strcmp(argv[i], "-n"))
            numbers = 0;
        else if (is_number(argv[i]))
            { count = atoi(argv[i]); if (count < 0) count = -count; }
        else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "0"))
            all = 1;
    }
    int n = hist_count();
    int start = all || count >= n ? 0 : n - count;
    for (int i = start; i < n; i++) {
        if (numbers)
            printf("%5d  %s\n", hist_base() + i, hist_get(i));
        else
            printf("%s\n", hist_get(i));
    }
    return 0;
}

static int b_umask(int argc, char **argv)
{
    int m = umask(0);
    umask(m);
    if (argc < 2) {
        printf("%03o\n", m);
        return 0;
    }
    if (!strcmp(argv[1], "-S")) {
        int a = ~m & 0777;
        printf("u=%s%s%s,g=%s%s%s,o=%s%s%s\n", a & 0400 ? "r" : "", a & 0200 ? "w" : "",
               a & 0100 ? "x" : "", a & 040 ? "r" : "", a & 020 ? "w" : "", a & 010 ? "x" : "",
               a & 4 ? "r" : "", a & 2 ? "w" : "", a & 1 ? "x" : "");
        return 0;
    }
    char *end;
    long v = strtol(argv[1], &end, 8);
    if (*end || v < 0 || v > 0777) {
        eprintf("umask: bad umask: %s\n", argv[1]);
        return 1;
    }
    umask((int)v);
    return 0;
}

static int b_trap(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-p"))) {
        trap_list();
        return 0;
    }
    if (!strcmp(argv[1], "-l")) {
        char *l[] = { "kill", "-l", 0 };
        return b_kill(2, l);
    }
    int i = 1;
    const char *action = argv[1];
    if (sig_by_name(action) >= 0 && strcmp(action, "-") && argc == 2) {
        /* trap SIG: reset */
        action = "-";
    } else {
        i = 2;
    }
    int st = 0;
    for (; i < argc; i++) {
        int sig = sig_by_name(argv[i]);
        if (sig < 0) {
            eprintf("trap: undefined signal: %s\n", argv[i]);
            st = 1;
            continue;
        }
        if (trap_set(sig, action) < 0)
            st = 1;
    }
    return st;
}

static int b_getopts(int argc, char **argv)
{
    if (argc < 3)
        return usage("getopts", "usage: getopts optstring name [args]");
    const char *optstring = argv[1], *name = argv[2];
    char **args = argc > 3 ? argv + 3 : pos.v;
    int nargs = argc > 3 ? argc - 3 : pos.n;
    const char *oi = var_get("OPTIND");
    int optind = oi && *oi ? atoi(oi) : 1;
    static int charpos = 1;
    static int last_optind;
    if (optind != last_optind)
        charpos = 1;
    if (optind > nargs || !args[optind - 1] || args[optind - 1][0] != '-' || !args[optind - 1][1] ||
        !strcmp(args[optind - 1], "--")) {
        if (optind <= nargs && args[optind - 1] && !strcmp(args[optind - 1], "--"))
            optind++;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", optind);
        var_set("OPTIND", buf, 0);
        var_set(name, "?", 0);
        last_optind = optind;
        return 1;
    }
    const char *arg = args[optind - 1];
    char c = arg[charpos];
    const char *spec = strchr(optstring, c);
    int silent = optstring[0] == ':';
    char opt[2] = { c, 0 };
    var_unset("OPTARG");
    if (!spec || c == ':') {
        if (!silent)
            eprintf("%s: bad option: -%c\n", sh.script_name ? sh.script_name : "esh", c);
        else
            var_set("OPTARG", opt, 0);
        var_set(name, "?", 0);
    } else if (spec[1] == ':') {
        if (arg[charpos + 1]) {
            var_set("OPTARG", arg + charpos + 1, 0);
        } else if (optind < nargs) {
            var_set("OPTARG", args[optind], 0);
            optind++;
        } else {
            if (!silent) {
                eprintf("%s: argument expected after -%c option\n",
                        sh.script_name ? sh.script_name : "esh", c);
                var_set(name, "?", 0);
            } else {
                var_set(name, ":", 0);
                var_set("OPTARG", opt, 0);
            }
            optind++;
            charpos = 1;
            goto done;
        }
        var_set(name, opt, 0);
        optind++;
        charpos = 1;
        goto done;
    } else {
        var_set(name, opt, 0);
    }
    if (arg[++charpos] == 0) {
        optind++;
        charpos = 1;
    }
done:;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", optind);
    var_set("OPTIND", buf, 0);
    last_optind = optind;
    return 0;
}

static int b_noop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int b_help(int argc, char **argv);

const struct builtin builtins[] = {
    { ".", b_source, "run commands from a file in this shell" },
    { ":", b_true, "do nothing, successfully" },
    { "[", b_test, "test a condition (see test)" },
    { "alias", b_alias, "alias [name[=value] ...]: define or list aliases" },
    { "autoload", b_noop, "(accepted for zsh compatibility; does nothing)" },
    { "bg", b_fg, "bg [%job]: continue a job in the background" },
    { "break", b_break, "break [n]: leave n enclosing loops" },
    { "builtin", b_builtin, "builtin name [args]: run a builtin, ignoring functions" },
    { "bye", b_exit, "exit the shell" },
    { "cd", b_cd, "cd [dir | - | +n | old new]: change directory" },
    { "chdir", b_cd, "same as cd" },
    { "command", b_command, "command [-v|-V] name [args]: run or describe a command, ignoring functions" },
    { "compinit", b_noop, "(accepted for zsh compatibility; completion is always on)" },
    { "continue", b_break, "continue [n]: next iteration of the nth enclosing loop" },
    { "declare", b_noop, "declare variables (see typeset)" },
    { "dirs", b_dirs, "dirs [-v] [-c]: show (or clear) the directory stack" },
    { "disown", b_disown, "disown [%job]: forget a job" },
    { "echo", b_echo, "echo [-neE] args: print arguments (escapes are interpreted)" },
    { "emulate", b_noop, "(accepted for zsh compatibility; does nothing)" },
    { "eval", b_eval, "eval args: run arguments as a command" },
    { "exec", b_noop, "exec cmd [args] | exec redirections: replace the shell, or redirect it" },
    { "exit", b_exit, "exit [n]: leave the shell" },
    { "export", b_noop, "export name[=value] ...: export variables to commands" },
    { "false", b_false, "do nothing, unsuccessfully" },
    { "fg", b_fg, "fg [%job]: continue a job in the foreground" },
    { "functions", b_functions, "functions [name ...]: list function definitions" },
    { "getopts", b_getopts, "getopts optstring name [args]: parse options" },
    { "hash", b_rehash, "forget remembered command locations" },
    { "help", b_help, "help [builtin]: this list, or help for one builtin" },
    { "history", b_history, "history [n | -c | -n]: list (or clear) the history" },
    { "integer", b_noop, "integer name[=value]: declare an integer variable" },
    { "jobs", b_jobs, "jobs [-l] [-p]: list jobs" },
    { "kill", b_kill, "kill [-sig] pid|%job ... | kill -l: send a signal" },
    { "let", b_let, "let expr ...: arithmetic" },
    { "local", b_noop, "local name[=value] ...: function-local variables" },
    { "logout", b_exit, "exit a login shell" },
    { "noglob", b_noop, "noglob cmd: run cmd without filename generation" },
    { "popd", b_popd, "popd [+n]: leave a directory from the stack" },
    { "print", b_print, "print [-nrlNP] args: print (-P: prompt escapes, -l: one per line)" },
    { "printf", b_printf, "printf format [args]: formatted output" },
    { "pushd", b_pushd, "pushd [dir | +n]: change directory, remembering this one" },
    { "pwd", b_pwd, "print the working directory" },
    { "read", b_read, "read [-rs] [-p prompt] [-t secs] [-n n] [-A arr] [name?prompt] [names]" },
    { "readonly", b_noop, "readonly name[=value] ...: make variables read-only" },
    { "rehash", b_rehash, "forget remembered command locations" },
    { "return", b_return, "return [n]: return from a function or sourced file" },
    { "set", b_set, "set [-euxvf] [-o opt] [-A arr] [-- args]: options and positional parameters" },
    { "setopt", b_setopt, "setopt [opt ...]: turn options on (no args: list)" },
    { "shift", b_shift, "shift [n]: drop positional parameters" },
    { "source", b_source, "source file [args]: run commands from a file in this shell" },
    { "test", b_test, "test expr: evaluate a condition" },
    { "trap", b_trap, "trap [action] sig ...: run action on a signal (EXIT: on exit)" },
    { "true", b_true, "do nothing, successfully" },
    { "type", b_type, "type name ...: describe how a name would be run" },
    { "typeset", b_noop, "typeset [-xriaglu] name[=value]: declare variables" },
    { "umask", b_umask, "umask [-S] [mode]: show or set the file creation mask" },
    { "unalias", b_unalias, "unalias name ... | -a: remove aliases" },
    { "unfunction", b_unfunction, "unfunction name ...: remove functions" },
    { "unset", b_unset, "unset [-f] name ...: remove variables (or functions)" },
    { "unsetopt", b_setopt, "unsetopt opt ...: turn options off (no args: list)" },
    { "wait", b_wait, "wait [%job | pid]: wait for background jobs" },
    { "whence", b_whence, "whence [-vcpa] name ...: how a name would be run" },
    { "which", b_which, "which name ...: show the command a name would run" },
    { "zstyle", b_noop, "(accepted for zsh compatibility; does nothing)" },
    { 0, 0, 0 },
};

const struct builtin *builtin_lookup(const char *name)
{
    for (int i = 0; builtins[i].name; i++)
        if (!strcmp(builtins[i].name, name))
            return &builtins[i];
    return 0;
}

int builtin_names(const char *prefix, struct strv *out)
{
    int n = 0;
    for (int i = 0; builtins[i].name; i++)
        if (!strncmp(builtins[i].name, prefix, strlen(prefix))) {
            strv_push(out, astrdup(builtins[i].name));
            n++;
        }
    return n;
}

static int b_help(int argc, char **argv)
{
    if (argc > 1) {
        int st = 0;
        for (int i = 1; i < argc; i++) {
            const struct builtin *b = builtin_lookup(argv[i]);
            if (b)
                printf("%-10s %s\n", b->name, b->help);
            else {
                eprintf("help: no builtin named %s\n", argv[i]);
                st = 1;
            }
        }
        return st;
    }
    printf("esh %s, the Eunuchs shell (zsh-like)\n\n", ESH_VERSION);
    printf("Builtins (help NAME for one):\n");
    int col = 0;
    for (int i = 0; builtins[i].name; i++) {
        int w = strlen(builtins[i].name) + 1;
        if (col + w > 76) {
            printf("\n");
            col = 0;
        }
        printf("%s%s", col ? " " : "  ", builtins[i].name);
        col += w + (col ? 0 : 1);
    }
    printf("\n\n");
    printf("Syntax: a | b   a |& b   a && b   a || b   a &   ( a )   { a; }   ! a   time a\n");
    printf("        if/elif/else/fi  while/until/do/done  for x in ..  for ((;;))  case/esac\n");
    printf("        repeat n  f() { ... }  [[ cond ]]  (( math ))  < > >> >| &> 2>&1 << <<< \n");
    printf("Expansion: $x ${x:-d} ${x#p} ${x/a/b} ${(U)x} ${#x} $arr ${arr[2]} $(cmd) $((1+2))\n");
    printf("        ~user {a,b} {1..9} * ? [a-z] **/*.c  'q' \"q\" $'\\n'  !! !$ !n ^a^b\n");
    printf("\nLine editing (emacs keys):\n");
    printf("  Tab complete (again: menu)     Up/Down  history matching the typed prefix\n");
    printf("  Ctrl-R  search history          Right/End  accept the gray suggestion\n");
    printf("  Ctrl-A/E start/end of line      Alt-B/F  word left/right (Ctrl-arrows too)\n");
    printf("  Ctrl-W/Alt-D  delete word       Ctrl-K/U  delete to end / whole line\n");
    printf("  Ctrl-Y  yank (Alt-Y cycles)     Ctrl-_  undo     Ctrl-T  transpose\n");
    printf("  Alt-.  last word of previous    Alt-Q  push line (restored after next command)\n");
    printf("  Ctrl-L  clear screen            Ctrl-C  discard line     Ctrl-D  exit (empty line)\n");
    printf("\nOptions: setopt (list on), unsetopt (list off); see /etc/eshrc and ~/.eshrc.\n");
    printf("Kernel debugging: Ctrl-] enters the monitor; see also strace, dbg, /proc.\n");
    return 0;
}
