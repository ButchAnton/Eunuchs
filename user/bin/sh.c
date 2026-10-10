/*
 * sh: the Eunuchs shell.
 *
 *   pipelines        a | b | c
 *   lists            a ; b    a && b    a || b    a &
 *   redirections     < f   > f   >> f   2> f   2>&1   (any fd: n> n< n>> n>&m)
 *   quoting          'literal'  "with $vars"  \x
 *   expansion        $VAR ${VAR} $? $$ $!   ~ at word start   * ? globs
 *   variables        NAME=value   NAME=value cmd   export unset set
 *   jobs             cmd &   jobs   fg [%n]   bg [%n]   wait   ^Z stops the foreground job
 *   builtins         cd pwd exit export unset set jobs fg bg wait umask source . exec help
 *   scripts          sh -c 'commands'   sh file
 *
 * Interactive shells use job control: each job is a process group that owns
 * the terminal while it runs in the foreground.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAXWORDS  64
#define MAXCMDS   8
#define MAXREDIR  8
#define MAXJOBS   16
#define MAXVARS   64
#define LINE      1024

/* ---- variables ---- */

struct var {
    char *name, *value;
    int exported;
};

static struct var vars[MAXVARS];
static int nvars;
static int last_status, interactive, shell_pgid;
static pid_t last_bg;
static struct termios shell_tmodes;
static volatile int got_sigint;

static struct var *var_find(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name))
            return &vars[i];
    return 0;
}

static char *xstrdup(const char *s)
{
    char *d = malloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

static void var_set(const char *name, const char *value, int exported)
{
    struct var *v = var_find(name);
    if (!v) {
        if (nvars == MAXVARS) {
            eprintf("sh: too many variables\n");
            return;
        }
        v = &vars[nvars++];
        v->name = xstrdup(name);
        v->value = 0;
        v->exported = 0;
    }
    free(v->value);
    v->value = xstrdup(value);
    if (exported)
        v->exported = 1;
}

static void var_unset(const char *name)
{
    struct var *v = var_find(name);
    if (!v)
        return;
    free(v->name);
    free(v->value);
    *v = vars[--nvars];
}

static const char *var_get(const char *name)
{
    static char buf[24];
    if (!strcmp(name, "?")) {
        snprintf(buf, sizeof(buf), "%d", last_status);
        return buf;
    }
    if (!strcmp(name, "$")) {
        snprintf(buf, sizeof(buf), "%d", getpid());
        return buf;
    }
    if (!strcmp(name, "!")) {
        snprintf(buf, sizeof(buf), "%d", last_bg);
        return buf;
    }
    struct var *v = var_find(name);
    return v ? v->value : "";
}

/* ---- lexer: words (expanded) and operators ---- */

enum tt { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_REDIR, T_END };
enum rk { R_IN, R_OUT, R_APPEND, R_DUP };

struct tok {
    enum tt type;
    char *word;
    int glob;           /* unquoted * or ? present */
    int assign;         /* NAME=... with an unquoted '=' */
    enum rk rkind;
    int rfd, rdup;
};

static struct tok toks[256];
static int ntoks;

static int is_name_char(int c, int first)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
}

static int lex(const char *s)
{
    ntoks = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')
            break;
        if (ntoks >= 250) {
            eprintf("sh: line too long\n");
            return -1;
        }
        struct tok *t = &toks[ntoks++];
        memset(t, 0, sizeof(*t));

        /* operators */
        if (s[0] == '|' && s[1] == '|') { t->type = T_OR; s += 2; continue; }
        if (s[0] == '&' && s[1] == '&') { t->type = T_AND; s += 2; continue; }
        if (*s == '|') { t->type = T_PIPE; s++; continue; }
        if (*s == '&') { t->type = T_BG; s++; continue; }
        if (*s == ';') { t->type = T_SEMI; s++; continue; }
        const char *p = s;
        int fd = -1;
        if (*p >= '0' && *p <= '9' && (p[1] == '<' || p[1] == '>'))
            fd = *p++ - '0';
        if (*p == '<' || *p == '>') {
            t->type = T_REDIR;
            if (p[0] == '>' && p[1] == '>') {
                t->rkind = R_APPEND;
                p += 2;
            } else if (p[0] == '>' && p[1] == '&') {
                t->rkind = R_DUP;
                p += 2;
                t->rdup = *p >= '0' && *p <= '9' ? *p++ - '0' : 1;
            } else {
                t->rkind = *p == '<' ? R_IN : R_OUT;
                p++;
            }
            t->rfd = fd >= 0 ? fd : (t->rkind == R_IN ? 0 : 1);
            s = p;
            continue;
        }

        /* a word, expanding quotes and $ as we go */
        char buf[LINE];
        int n = 0, quoted = 0;
        char q = 0;
        t->type = T_WORD;
        if (*s == '~' && (s[1] == '/' || s[1] == ' ' || !s[1])) {
            const char *home = var_get("HOME");
            n = snprintf(buf, sizeof(buf), "%s", *home ? home : "/");
            s++;
        }
        while (*s && n < LINE - 1) {
            char c = *s;
            if (!q && (c == ' ' || c == '\t' || c == '|' || c == '&' || c == ';' || c == '<' ||
                       c == '>'))
                break;
            if (!q && (c == '\'' || c == '"')) {
                q = c;
                quoted = 1;
                s++;
                continue;
            }
            if (q && c == q) {
                q = 0;
                s++;
                continue;
            }
            if (c == '\\' && q != '\'' && s[1]) {
                buf[n++] = s[1];
                s += 2;
                continue;
            }
            if (c == '$' && q != '\'' && (is_name_char(s[1], 1) || s[1] == '{' || s[1] == '?' ||
                                          s[1] == '$' || s[1] == '!')) {
                char name[64];
                int k = 0;
                s++;
                if (*s == '{') {
                    s++;
                    while (*s && *s != '}' && k < 63)
                        name[k++] = *s++;
                    if (*s == '}')
                        s++;
                } else if (*s == '?' || *s == '$' || *s == '!') {
                    name[k++] = *s++;
                } else {
                    while (is_name_char(*s, k == 0) && k < 63)
                        name[k++] = *s++;
                }
                name[k] = 0;
                n += snprintf(buf + n, sizeof(buf) - n, "%s", var_get(name));
                if (n > LINE - 1)
                    n = LINE - 1;
                continue;
            }
            if (!q && (c == '*' || c == '?'))
                t->glob = 1;
            if (!q && c == '=' && !t->assign && n > 0) {
                int ok = is_name_char(buf[0], 1);
                for (int i = 1; i < n && ok; i++)
                    ok = is_name_char(buf[i], 0);
                t->assign = ok && !quoted;
            }
            buf[n++] = c;
            s++;
        }
        if (q) {
            eprintf("sh: unterminated %c quote\n", q);
            return -1;
        }
        buf[n] = 0;
        t->word = xstrdup(buf);
    }
    toks[ntoks].type = T_END;
    return 0;
}

/* ---- globbing ---- */

static int match(const char *p, const char *s)
{
    if (!*p)
        return !*s;
    if (*p == '*')
        return match(p + 1, s) || (*s && match(p, s + 1));
    if (*s && (*p == '?' || *p == *s))
        return match(p + 1, s + 1);
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Expand a pattern (wildcards in the last component only) into argv. */
static int glob(const char *pat, char **out, int max)
{
    char dir[256], base[64];
    const char *slash = strrchr(pat, '/');
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - pat), pat);
        snprintf(base, sizeof(base), "%s", slash + 1);
    } else {
        strcpy(dir, ".");
        snprintf(base, sizeof(base), "%s", pat);
    }
    DIR *d = opendir(slash && slash == pat ? "/" : dir);
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.' && base[0] != '.')
            continue;
        if (!match(base, de->d_name))
            continue;
        char buf[320];
        if (slash)
            snprintf(buf, sizeof(buf), "%s/%s", slash == pat ? "" : dir, de->d_name);
        else
            snprintf(buf, sizeof(buf), "%s", de->d_name);
        out[n++] = xstrdup(buf);
    }
    if (d)
        closedir(d);
    /* insertion sort (qsort-free libc) */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cmp_str(&out[j - 1], &out[j]) > 0; j--) {
            char *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    if (!n)
        out[n++] = xstrdup(pat);        /* no match: keep the word */
    return n;
}

/* ---- parser ---- */

struct redir {
    enum rk kind;
    int fd, dup;
    char *path;
};

struct cmd {
    char *argv[MAXWORDS + 1];
    int argc;
    char *assigns[8];
    int nassign;
    struct redir redirs[MAXREDIR];
    int nredir;
};

struct pipeline {
    struct cmd cmds[MAXCMDS];
    int ncmds;
    int bg;
    enum tt next;       /* T_AND, T_OR, T_SEMI, T_END */
    char text[160];
};

static int parse_pipeline(int *ti, struct pipeline *pl)
{
    memset(pl, 0, sizeof(*pl));
    struct cmd *c = &pl->cmds[0];
    pl->ncmds = 1;
    for (;;) {
        struct tok *t = &toks[*ti];
        switch (t->type) {
        case T_WORD:
            if (t->assign && c->argc == 0 && c->nassign < 8) {
                c->assigns[c->nassign++] = t->word;
            } else if (t->glob) {
                c->argc += glob(t->word, c->argv + c->argc, MAXWORDS - c->argc);
            } else if (c->argc < MAXWORDS) {
                c->argv[c->argc++] = t->word;
            }
            break;
        case T_REDIR: {
            struct tok *target = &toks[*ti + 1];
            if (c->nredir == MAXREDIR || (t->rkind != R_DUP && target->type != T_WORD)) {
                eprintf("sh: syntax error: redirection needs a file name\n");
                return -1;
            }
            struct redir *r = &c->redirs[c->nredir++];
            r->kind = t->rkind;
            r->fd = t->rfd;
            r->dup = t->rdup;
            if (t->rkind != R_DUP) {
                r->path = target->word;
                (*ti)++;
            }
            break;
        }
        case T_PIPE:
            if (!c->argc && !c->nassign) {
                eprintf("sh: syntax error near '|'\n");
                return -1;
            }
            if (pl->ncmds == MAXCMDS) {
                eprintf("sh: pipeline too long\n");
                return -1;
            }
            c = &pl->cmds[pl->ncmds++];
            break;
        default:
            if (c->argc == 0 && c->nassign == 0 && (pl->ncmds > 1 || c->nredir)) {
                eprintf("sh: syntax error: missing command\n");
                return -1;
            }
            pl->bg = t->type == T_BG;
            pl->next = t->type == T_BG ? T_SEMI : t->type;
            if (t->type != T_END)
                (*ti)++;
            return 0;
        }
        (*ti)++;
    }
}

/* ---- jobs ---- */

struct job {
    int id;
    pid_t pgid;
    pid_t pids[MAXCMDS];
    int done[MAXCMDS];
    int npids, running;
    int stopped;
    int status;
    char text[160];
};

static struct job jobs[MAXJOBS];

static struct job *job_new(void)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (!jobs[i].id) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].id = i + 1;
            return &jobs[i];
        }
    return 0;
}

static int status_code(int st)
{
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void report_signal(int st)
{
    if (WIFSIGNALED(st) && WTERMSIG(st) != SIGINT && WTERMSIG(st) != SIGPIPE)
        eprintf("%s%s\n", strsignal(WTERMSIG(st)), WCOREDUMP(st) ? " (core dumped)" : "");
}

/* Collect one process of a job; returns 1 if it stopped. */
static int collect(struct job *j, int i, int st)
{
    if (WIFSTOPPED(st)) {
        j->stopped = 1;
        return 1;
    }
    j->done[i] = 1;
    j->running--;
    if (i == j->npids - 1)
        j->status = st;
    return 0;
}

/* Wait for a foreground job until it finishes or stops. */
static void wait_fg(struct job *j)
{
    for (int i = 0; i < j->npids && !j->stopped; i++) {
        while (!j->done[i]) {
            int st;
            pid_t pid = waitpid(j->pids[i], &st, WUNTRACED);
            if (pid < 0) {
                if (errno == EINTR)
                    continue;
                j->done[i] = 1;
                j->running--;
                break;
            }
            if (collect(j, i, st))
                break;
        }
    }
    if (interactive) {
        tcsetpgrp(0, shell_pgid);
        tcsetattr(0, TCSADRAIN, &shell_tmodes);
    }
    if (j->stopped) {
        printf("\n[%d]+  Stopped  %s\n", j->id, j->text);
        last_status = 128 + SIGTSTP;
        return;
    }
    report_signal(j->status);
    last_status = status_code(j->status);
    j->id = 0;
}

static void reap_background(void)
{
    for (int i = 0; i < MAXJOBS; i++) {
        struct job *j = &jobs[i];
        if (!j->id)
            continue;
        for (int k = 0; k < j->npids; k++) {
            int st;
            if (!j->done[k] && waitpid(j->pids[k], &st, WNOHANG | WUNTRACED) > 0)
                collect(j, k, st);
        }
        if (j->running == 0) {
            if (WIFSIGNALED(j->status))
                printf("[%d]   %s  %s\n", j->id, strsignal(WTERMSIG(j->status)), j->text);
            else if (WEXITSTATUS(j->status))
                printf("[%d]   Exit %d  %s\n", j->id, WEXITSTATUS(j->status), j->text);
            else
                printf("[%d]   Done  %s\n", j->id, j->text);
            j->id = 0;
        }
    }
}

static struct job *job_arg(int argc, char **argv)
{
    if (argc > 1) {
        int n = atoi(argv[1][0] == '%' ? argv[1] + 1 : argv[1]);
        return n >= 1 && n <= MAXJOBS && jobs[n - 1].id ? &jobs[n - 1] : 0;
    }
    for (int i = MAXJOBS - 1; i >= 0; i--)
        if (jobs[i].id)
            return &jobs[i];
    return 0;
}

/* ---- execution ---- */

static int apply_redirs(struct cmd *c)
{
    for (int i = 0; i < c->nredir; i++) {
        struct redir *r = &c->redirs[i];
        if (r->kind == R_DUP) {
            if (dup2(r->dup, r->fd) < 0) {
                eprintf("sh: %d>&%d: %s\n", r->fd, r->dup, strerror(errno));
                return -1;
            }
            continue;
        }
        int flags = r->kind == R_IN ? O_RDONLY
                  : O_WRONLY | O_CREAT | (r->kind == R_APPEND ? O_APPEND : O_TRUNC);
        int fd = open(r->path, flags, 0666);
        if (fd < 0) {
            eprintf("sh: %s: %s\n", r->path, strerror(errno));
            return -1;
        }
        if (fd != r->fd) {
            dup2(fd, r->fd);
            close(fd);
        }
    }
    return 0;
}

static char **build_env(struct cmd *c)
{
    static char *env[MAXVARS + 9];
    static char store[MAXVARS + 8][256];
    int n = 0;
    for (int i = 0; i < nvars; i++) {
        if (!vars[i].exported)
            continue;
        int overridden = 0;
        for (int k = 0; k < c->nassign; k++)
            if (!strncmp(c->assigns[k], vars[i].name, strlen(vars[i].name)) &&
                c->assigns[k][strlen(vars[i].name)] == '=')
                overridden = 1;
        if (overridden)
            continue;
        snprintf(store[n], sizeof(store[n]), "%s=%s", vars[i].name, vars[i].value);
        env[n] = store[n];
        n++;
    }
    for (int k = 0; k < c->nassign; k++)
        env[n++] = c->assigns[k];
    env[n] = 0;
    return env;
}

static void child_exec(struct cmd *c)
{
    /* Restore default dispositions the shell changed. */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    if (apply_redirs(c) < 0)
        _exit(1);
    if (!c->argc)
        _exit(0);
    environ = build_env(c);
    execvp(c->argv[0], c->argv);
    int e = errno;
    eprintf("sh: %s: %s\n", c->argv[0], e == ENOENT ? "command not found" : strerror(e));
    _exit(e == ENOENT ? 127 : 126);
}

static int run_builtin(struct cmd *c);

static void run_pipeline(struct pipeline *pl)
{
    struct cmd *first = &pl->cmds[0];
    if (pl->ncmds == 1 && !pl->bg) {
        if (first->argc == 0) {                 /* NAME=value ... alone: shell variables */
            for (int k = 0; k < first->nassign; k++) {
                char *eq = strchr(first->assigns[k], '=');
                *eq = 0;
                var_set(first->assigns[k], eq + 1, var_find(first->assigns[k]) &&
                                                   var_find(first->assigns[k])->exported);
                *eq = '=';
            }
            last_status = 0;
            return;
        }
        int r = run_builtin(first);
        if (r >= 0) {
            last_status = r;
            return;
        }
    }

    struct job *j = job_new();
    if (!j) {
        eprintf("sh: too many jobs\n");
        last_status = 1;
        return;
    }
    snprintf(j->text, sizeof(j->text), "%s", pl->text);
    int in = -1;
    for (int i = 0; i < pl->ncmds; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < pl->ncmds && pipe(fds) < 0) {
            eprintf("sh: pipe: %s\n", strerror(errno));
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            eprintf("sh: fork: %s\n", strerror(errno));
            break;
        }
        if (pid == 0) {
            if (interactive) {
                setpgid(0, j->pgid ? j->pgid : getpid());
                if (!pl->bg)
                    tcsetpgrp(0, j->pgid ? j->pgid : getpid());
            }
            if (in >= 0) {
                dup2(in, 0);
                close(in);
            }
            if (fds[1] >= 0) {
                dup2(fds[1], 1);
                close(fds[1]);
                close(fds[0]);
            }
            child_exec(&pl->cmds[i]);
        }
        if (!j->pgid)
            j->pgid = interactive ? pid : getpgrp();
        if (interactive)
            setpgid(pid, j->pgid);              /* both sides: no race with the child */
        j->pids[j->npids++] = pid;
        j->running++;
        if (in >= 0)
            close(in);
        if (fds[1] >= 0)
            close(fds[1]);
        in = fds[0];
    }
    if (in >= 0)
        close(in);

    if (pl->bg) {
        last_bg = j->pids[j->npids - 1];
        if (interactive)
            printf("[%d] %d\n", j->id, last_bg);
        last_status = 0;
        return;
    }
    if (interactive)
        tcsetpgrp(0, j->pgid);
    wait_fg(j);
}

static void run_line(const char *line)
{
    if (lex(line) < 0) {
        last_status = 2;
        return;
    }
    int ti = 0;
    enum tt prev = T_SEMI;
    while (toks[ti].type != T_END) {
        int start = ti;
        struct pipeline pl;
        if (parse_pipeline(&ti, &pl) < 0) {
            last_status = 2;
            return;
        }
        /* text of the pipeline for job messages */
        size_t len = 0;
        for (int k = start; k < ti && len < sizeof(pl.text) - 2; k++)
            if (toks[k].type == T_WORD)
                len += snprintf(pl.text + len, sizeof(pl.text) - len, "%s%s", len ? " " : "",
                                toks[k].word);
            else if (toks[k].type == T_PIPE)
                len += snprintf(pl.text + len, sizeof(pl.text) - len, " |");
        int skip = (prev == T_AND && last_status != 0) || (prev == T_OR && last_status == 0);
        if (!skip && (pl.cmds[0].argc || pl.cmds[0].nassign))
            run_pipeline(&pl);
        prev = pl.next;
    }
}

/* ---- builtins ---- */

static void run_file(const char *path);

static int run_builtin(struct cmd *c)
{
    char **argv = c->argv;
    int argc = c->argc;
    const char *n = argv[0];

    if (!strcmp(n, "cd")) {
        const char *dir = argc > 1 ? argv[1] : var_get("HOME");
        if (chdir(*dir ? dir : "/") < 0) {
            eprintf("cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)))
            var_set("PWD", cwd, 1);
        return 0;
    }
    if (!strcmp(n, "pwd")) {
        char cwd[256];
        printf("%s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
        return 0;
    }
    if (!strcmp(n, "exit"))
        exit(argc > 1 ? atoi(argv[1]) : last_status);
    if (!strcmp(n, "export") || !strcmp(n, "unset")) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (n[0] == 'u') {
                var_unset(argv[i]);
            } else if (eq) {
                *eq = 0;
                var_set(argv[i], eq + 1, 1);
            } else if (var_find(argv[i])) {
                var_find(argv[i])->exported = 1;
            } else {
                var_set(argv[i], "", 1);
            }
        }
        return 0;
    }
    if (!strcmp(n, "set")) {
        for (int i = 0; i < nvars; i++)
            printf("%s%s=%s\n", vars[i].exported ? "export " : "", vars[i].name, vars[i].value);
        return 0;
    }
    if (!strcmp(n, "jobs")) {
        reap_background();
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].id)
                printf("[%d]  %-8s %s\n", jobs[i].id, jobs[i].stopped ? "Stopped" : "Running",
                       jobs[i].text);
        return 0;
    }
    if (!strcmp(n, "fg") || !strcmp(n, "bg")) {
        struct job *j = job_arg(argc, argv);
        if (!j) {
            eprintf("%s: no such job\n", n);
            return 1;
        }
        printf("%s\n", j->text);
        j->stopped = 0;
        if (n[0] == 'f') {
            if (interactive)
                tcsetpgrp(0, j->pgid);
            kill(-j->pgid, SIGCONT);
            wait_fg(j);
            return last_status;
        }
        kill(-j->pgid, SIGCONT);
        return 0;
    }
    if (!strcmp(n, "kill")) {
        int sig = SIGTERM, i = 1, rc = 0;
        if (argc > 1 && argv[1][0] == '-' && argv[1][1] != 'l') {
            const char *s = argv[1] + 1;
            if (*s >= '0' && *s <= '9')
                sig = atoi(s);
            else if (!strcmp(s, "KILL") || !strcmp(s, "SIGKILL")) sig = SIGKILL;
            else if (!strcmp(s, "INT") || !strcmp(s, "SIGINT")) sig = SIGINT;
            else if (!strcmp(s, "STOP") || !strcmp(s, "SIGSTOP")) sig = SIGSTOP;
            else if (!strcmp(s, "CONT") || !strcmp(s, "SIGCONT")) sig = SIGCONT;
            else if (!strcmp(s, "HUP") || !strcmp(s, "SIGHUP")) sig = SIGHUP;
            else if (!strcmp(s, "TERM") || !strcmp(s, "SIGTERM")) sig = SIGTERM;
            else
                return -1;              /* let /bin/kill parse other names */
            i = 2;
        }
        int any_job = 0;
        for (int k = i; k < argc; k++)
            any_job |= argv[k][0] == '%';
        if (!any_job)
            return -1;                  /* plain pids: /bin/kill */
        for (; i < argc; i++) {
            pid_t target;
            if (argv[i][0] == '%') {
                char *one[] = { "kill", argv[i], 0 };
                struct job *j = job_arg(2, one);
                if (!j) {
                    eprintf("kill: %s: no such job\n", argv[i]);
                    rc = 1;
                    continue;
                }
                target = -j->pgid;
                if (sig == SIGTERM || sig == SIGKILL || sig == SIGINT || sig == SIGHUP ||
                    sig == SIGCONT) {
                    kill(target, SIGCONT);      /* a stopped job must wake to die */
                    j->stopped = 0;
                }
            } else {
                target = atoi(argv[i]);
            }
            if (kill(target, sig) < 0) {
                eprintf("kill: %s: %s\n", argv[i], strerror(errno));
                rc = 1;
            }
        }
        return rc;
    }
    if (!strcmp(n, "wait")) {
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].id && !jobs[i].stopped)
                wait_fg(&jobs[i]);
        return 0;
    }
    if (!strcmp(n, "umask")) {
        if (argc > 1) {
            umask((int)strtol(argv[1], 0, 8));
        } else {
            int m = umask(0);
            umask(m);
            printf("%04o\n", m);
        }
        return 0;
    }
    if (!strcmp(n, "source") || !strcmp(n, ".")) {
        if (argc > 1)
            run_file(argv[1]);
        return last_status;
    }
    if (!strcmp(n, "exec")) {
        if (argc < 2)
            return 0;
        if (apply_redirs(c) < 0)
            return 1;
        environ = build_env(c);
        execvp(argv[1], argv + 1);
        eprintf("exec: %s: %s\n", argv[1], strerror(errno));
        return 127;
    }
    if (!strcmp(n, "help")) {
        printf("builtins: cd pwd exit export unset set jobs fg bg wait umask source exec help\n");
        printf("syntax: a | b, a ; b, a && b, a || b, a &, < > >> 2> 2>&1, 'q' \"q\" $VAR ~ * ?\n");
        printf("programs in /bin:");
        DIR *d = opendir("/bin");
        struct dirent *de;
        while (d && (de = readdir(d)))
            if (de->d_name[0] != '.')
                printf(" %s", de->d_name);
        if (d)
            closedir(d);
        printf("\nkernel debugging: Ctrl-] enters the monitor; see also strace, dbg, /proc\n");
        return 0;
    }
    return -1;          /* not a builtin */
}

/* ---- input ---- */

static int read_line(int fd, char *buf, int size)
{
    int n = 0;
    while (n < size - 1) {
        char c;
        long r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR && got_sigint) {
                got_sigint = 0;
                n = 0;
                return -2;              /* ^C at the prompt */
            }
            return -1;
        }
        if (r == 0)
            return n ? n : -1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;                   /* lines from the network (rshd, nc) */
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static void run_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        eprintf("sh: %s: %s\n", path, strerror(errno));
        last_status = 127;
        return;
    }
    char line[LINE];
    int r;
    while ((r = read_line(fd, line, sizeof(line))) >= 0) {
        if (line[0])
            run_line(line);
        reap_background();
    }
    close(fd);
}

static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
}

static void prompt(void)
{
    char cwd[256];
    uid_t uid = geteuid();
    printf("%s@eunuchs:%s%c ", uid == 0 ? "root" : uid == 1000 ? "guest" : "user",
           getcwd(cwd, sizeof(cwd)) ? cwd : "?", uid == 0 ? '#' : '$');
    fflush_stdout();
}

int main(int argc, char **argv)
{
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        *eq = 0;
        var_set(*e, eq + 1, 1);
        *eq = '=';
    }
    if (!var_find("PATH"))
        var_set("PATH", "/bin", 1);

    if (argc > 2 && !strcmp(argv[1], "-c")) {
        run_line(argv[2]);
        return last_status;
    }
    /* -i: show prompts even without a terminal (rshd), but no job control */
    int prompts = 0;
    if (argc > 1 && !strcmp(argv[1], "-i")) {
        prompts = 1;
        argc--, argv++;
    }
    if (argc > 1) {
        run_file(argv[1]);
        return last_status;
    }

    interactive = isatty(0);
    prompts |= interactive;
    if (interactive) {
        /* Job control: our own process group, owning the terminal. */
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        struct sigaction act = { .sa_handler = on_sigint };     /* no SA_RESTART: read returns EINTR */
        sigaction(SIGINT, &act, 0);
        setpgid(0, 0);
        shell_pgid = getpgrp();
        tcsetpgrp(0, shell_pgid);
        tcgetattr(0, &shell_tmodes);
    }

    char line[LINE];
    for (;;) {
        reap_background();
        if (prompts)
            prompt();
        int r = read_line(0, line, sizeof(line));
        if (r == -2)
            continue;
        if (r < 0)
            break;
        if (line[0])
            run_line(line);
    }
    if (interactive)
        printf("exit\n");
    return last_status;
}
