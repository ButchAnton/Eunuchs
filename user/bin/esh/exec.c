/*
 * esh: execution of the syntax tree.
 *
 * Builtins and functions run inside the shell, with their redirections
 * applied to the shell's own descriptors and undone afterwards. External
 * commands and pipelines run in children; an interactive shell puts each
 * pipeline in its own process group (a job). Inside a child that will do
 * nothing else (EF_FORKED), the last command is exec'ed without a fork.
 */
#include "esh.h"

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

volatile int got_sigint, got_sigchld, got_sigwinch;
int pending_traps;

/* ---- functions ---- */

static struct func *funcs;

struct func *func_lookup(const char *name)
{
    for (struct func *f = funcs; f; f = f->next)
        if (!strcmp(f->name, name))
            return f;
    return 0;
}

void func_define(const char *name, const char *body)
{
    struct func *f = func_lookup(name);
    if (!f) {
        f = xmalloc(sizeof(*f));
        f->name = xstrdup(name);
        f->body = 0;
        f->next = funcs;
        funcs = f;
    }
    free(f->body);
    f->body = xstrdup(body);
}

int func_remove(const char *name)
{
    for (struct func **pp = &funcs; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, name)) {
            struct func *f = *pp;
            *pp = f->next;
            free(f->name);
            free(f->body);
            free(f);
            return 0;
        }
    return -1;
}

int func_names(const char *prefix, struct strv *out)
{
    int n = 0;
    for (struct func *f = funcs; f; f = f->next)
        if (!strncmp(f->name, prefix, strlen(prefix))) {
            strv_push(out, astrdup(f->name));
            n++;
        }
    return n;
}

void funcs_list(void)
{
    struct strv names = { 0 };
    func_names("", &names);
    sort_strs(names.v, names.n);
    for (int i = 0; i < names.n; i++)
        printf("%s () %s\n", names.v[i], func_lookup(names.v[i])->body);
}

/* ---- command lookup ---- */

#define HASHSZ 64
static struct {
    char *name, *path;
} hashtab[HASHSZ];

void path_rehash(void)
{
    for (int i = 0; i < HASHSZ; i++) {
        free(hashtab[i].name);
        free(hashtab[i].path);
        hashtab[i].name = hashtab[i].path = 0;
    }
}

static int executable(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

char *path_lookup(const char *cmd)
{
    if (strchr(cmd, '/'))
        return executable(cmd) ? astrdup(cmd) : 0;
    unsigned h = 0;
    for (const char *p = cmd; *p; p++)
        h = h * 31 + (unsigned char)*p;
    h %= HASHSZ;
    if (hashtab[h].name && !strcmp(hashtab[h].name, cmd) && executable(hashtab[h].path))
        return astrdup(hashtab[h].path);
    const char *path = var_get("PATH");
    if (!path)
        path = "/bin";
    const char *p = path;
    for (;;) {
        const char *e = strchr(p, ':');
        int n = e ? e - p : (int)strlen(p);
        struct sb b = { 0 };
        if (n)
            sb_putn(&b, p, n);
        else
            sb_putc(&b, '.');
        sb_putc(&b, '/');
        sb_puts(&b, cmd);
        if (executable(sb_str(&b))) {
            char *r = sb_adup(&b);
            free(hashtab[h].name);
            free(hashtab[h].path);
            hashtab[h].name = xstrdup(cmd);
            hashtab[h].path = xstrdup(r);
            sb_free(&b);
            return r;
        }
        sb_free(&b);
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

int path_commands(const char *prefix, struct strv *out)
{
    const char *path = var_get("PATH");
    if (!path)
        path = "/bin";
    int n = 0, pl = strlen(prefix);
    char *copy = astrdup(path);
    for (char *dir = copy; dir;) {
        char *e = strchr(dir, ':');
        if (e)
            *e = 0;
        DIR *d = opendir(*dir ? dir : ".");
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.' || strncmp(de->d_name, prefix, pl))
                continue;
            char full[320];
            snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", de->d_name);
            if (executable(full)) {
                strv_push(out, astrdup(de->d_name));
                n++;
            }
        }
        if (d)
            closedir(d);
        dir = e ? e + 1 : 0;
    }
    return n;
}

const char *suggest_command(const char *name)
{
    struct strv all = { 0 };
    path_commands("", &all);
    builtin_names("", &all);
    func_names("", &all);
    alias_names("", &all);
    int best = 99;
    const char *pick = 0;
    int max = strlen(name) <= 6 ? 1 : 2;
    for (int i = 0; i < all.n; i++) {
        int d = edit_distance(name, all.v[i]);
        if (d < best || (d == best && pick && strcmp(all.v[i], pick) < 0)) {
            best = d;
            pick = all.v[i];
        }
    }
    return best <= max ? pick : 0;
}

/* ---- signals and traps ---- */

static char *traps[NSIG];
static volatile int trap_pending[NSIG];

static void on_signal(int sig)
{
    if (sig == SIGINT)
        got_sigint = 1;
    else if (sig == SIGCHLD)
        got_sigchld = 1;
    else if (sig == SIGWINCH)
        got_sigwinch = 1;
    if (sig > 0 && sig < NSIG && traps[sig]) {
        trap_pending[sig] = 1;
        pending_traps = 1;
    }
}

static void catch(int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = on_signal;     /* no SA_RESTART: reads return EINTR */
    sigaction(sig, &act, 0);
}

void signals_init_interactive(void)
{
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    catch(SIGINT);
    catch(SIGCHLD);
}

void child_reset_signals(void)
{
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGTERM, SIGCHLD,
                                SIGHUP, SIGUSR1, SIGUSR2, SIGALRM, SIGPIPE };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        int s = sigs[i];
        if (traps[s] && !*traps[s])
            signal(s, SIG_IGN);     /* trap '' SIG: stays ignored */
        else
            signal(s, SIG_DFL);
    }
}

int trap_set(int sig, const char *action)
{
    if (sig < 0 || sig >= NSIG)
        return -1;
    if (sig == SIGKILL || sig == SIGSTOP) {
        sh_error("trap: can't trap SIG%s", signame(sig));
        return -1;
    }
    if (sig == 0 && !action) {
        /* clear everything (subshells) */
        for (int i = 0; i < NSIG; i++) {
            free(traps[i]);
            traps[i] = 0;
        }
        return 0;
    }
    free(traps[sig]);
    traps[sig] = action && strcmp(action, "-") ? xstrdup(action) : 0;
    if (sig == 0)
        return 0;
    if (!traps[sig]) {
        /* back to the shell's own handling */
        if (sh.interactive && (sig == SIGINT || sig == SIGCHLD))
            catch(sig);
        else if (sh.interactive && (sig == SIGQUIT || sig == SIGTSTP || sig == SIGTTIN ||
                                    sig == SIGTTOU || sig == SIGTERM))
            signal(sig, SIG_IGN);
        else
            signal(sig, SIG_DFL);
    } else if (!*traps[sig]) {
        signal(sig, SIG_IGN);
    } else {
        catch(sig);
    }
    return 0;
}

const char *trap_get(int sig)
{
    return sig >= 0 && sig < NSIG ? traps[sig] : 0;
}

void trap_list(void)
{
    for (int i = 0; i < NSIG; i++)
        if (traps[i])
            printf("trap -- %s %s\n", quote_word(traps[i]), i ? signame(i) : "EXIT");
}

void run_traps(void)
{
    if (!pending_traps)
        return;
    pending_traps = 0;
    for (int i = 1; i < NSIG; i++) {
        if (!trap_pending[i])
            continue;
        trap_pending[i] = 0;
        if (traps[i] && *traps[i]) {
            int saved = sh.last_status;
            char *code = xstrdup(traps[i]);
            run_string(code, 0);
            free(code);
            sh.last_status = saved;
        }
    }
}

/* ---- process creation ---- */

pid_t sh_fork(void)
{
    out_flush();
    return fork();
}

/* ---- redirections ---- */

struct saved_fd {
    int fd, copy;       /* copy -1: fd was closed before */
};
struct fdsave {
    struct saved_fd v[16];
    int n;
};

static void save_fd(struct fdsave *s, int fd)
{
    if (!s || s->n == 16)
        return;
    for (int i = 0; i < s->n; i++)
        if (s->v[i].fd == fd)
            return;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    s->v[s->n].fd = fd;
    s->v[s->n].copy = copy;
    s->n++;
}

static void restore_fds(struct fdsave *s)
{
    out_flush();
    for (int i = s->n - 1; i >= 0; i--) {
        if (s->v[i].copy >= 0) {
            dup2(s->v[i].copy, s->v[i].fd);
            close(s->v[i].copy);
        } else {
            close(s->v[i].fd);
        }
    }
    s->n = 0;
}

static int heredoc_fd(const char *text)
{
    int len = strlen(text);
    if (len < 4000) {
        int fds[2];
        if (pipe(fds) < 0)
            return -1;
        write(fds[1], text, len);
        close(fds[1]);
        return fds[0];
    }
    char path[64];
    static int seq;
    snprintf(path, sizeof(path), "/tmp/.esh-hd-%d-%d", getpid(), seq++);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    unlink(path);
    write(fd, text, len);
    lseek(fd, 0, 0);
    return fd;
}

static int move_fd(int from, int to)
{
    if (from == to)
        return 0;
    if (dup2(from, to) < 0)
        return -1;
    close(from);
    return 0;
}

static int apply_redirs(struct redir *r, struct fdsave *save)
{
    for (; r; r = r->next) {
        out_flush();
        if (r->op == R_HEREDOC || r->op == R_HERESTR) {
            char *text;
            if (r->op == R_HERESTR) {
                char *w = expand_str(r->word, EX_TILDE);
                if (!w)
                    return -1;
                struct sb b = { 0 };
                sb_puts(&b, w);
                sb_putc(&b, '\n');
                text = sb_adup(&b);
                sb_free(&b);
            } else {
                text = r->hd_quoted ? r->heredoc : expand_heredoc(r->heredoc ? r->heredoc : "");
                if (!text)
                    return -1;
            }
            int fd = heredoc_fd(text ? text : "");
            if (fd < 0) {
                sh_error("can't create here-document: %s", strerror(errno));
                return -1;
            }
            save_fd(save, r->fd);
            move_fd(fd, r->fd);
            continue;
        }
        char *w = expand_str(r->word, EX_TILDE);
        if (!w)
            return -1;
        if (r->op == R_DUPIN || r->op == R_DUPOUT) {
            save_fd(save, r->fd);
            if (!strcmp(w, "-")) {
                close(r->fd);
                continue;
            }
            if (!is_number(w)) {
                sh_error("%s: bad file descriptor", w);
                return -1;
            }
            int from = atoi(w);
            if (dup2(from, r->fd) < 0) {
                sh_error("%d: %s", from, strerror(errno));
                return -1;
            }
            continue;
        }
        int flags;
        switch (r->op) {
        case R_IN: flags = O_RDONLY; break;
        case R_RDWR: flags = O_RDWR | O_CREAT; break;
        case R_APPEND: case R_ALLAPP: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        }
        int fd = open(w, flags, 0666);
        if (fd < 0) {
            const char *why = errno == ENOENT ? "no such file or directory"
                            : errno == EACCES ? "permission denied"
                            : errno == EISDIR ? "is a directory" : strerror(errno);
            sh_error("%s: %s", why, w);
            return -1;
        }
        if (r->op == R_ALLOUT || r->op == R_ALLAPP) {
            save_fd(save, 1);
            save_fd(save, 2);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd != 1 && fd != 2)
                close(fd);
            continue;
        }
        save_fd(save, r->fd);
        move_fd(fd, r->fd);
    }
    return 0;
}

/* ---- assignments ---- */

int assign_do(struct assign *a, int flags)
{
    if (a->is_array) {
        struct strv v = { 0 };
        if (expand_words(a->array, a->narray, &v, EX_FULL) < 0)
            return -1;
        if (var_set_array(a->name, v.v, v.n, a->append) < 0)
            return -1;
        if (flags) {
            struct var *var = var_lookup(a->name);
            if (var)
                var->flags |= flags;
        }
        return 0;
    }
    char *val = expand_str(a->value ? a->value : "", EX_TILDE | EX_ASSIGN);
    if (!val)
        return -1;
    if (a->index) {
        char *ix = expand_str(a->index, EX_NOBRACE);
        long long i;
        if (!ix || arith_eval(ix, &i) < 0)
            return -1;
        return var_set_elem(a->name, (long)i, val);
    }
    if (a->append) {
        struct var *v = var_lookup(a->name);
        if (v && (v->flags & V_ARRAY)) {
            char *one[] = { val };
            return var_set_array(a->name, one, 1, 1);
        }
        if (v && (v->flags & V_INTEGER)) {
            long long x, y;
            const char *old = var_get(a->name);
            if (arith_eval(old ? old : "0", &x) < 0 || arith_eval(val, &y) < 0)
                return -1;
            char buf[24];
            snprintf(buf, sizeof(buf), "%lld", x + y);
            return var_set(a->name, buf, flags);
        }
        const char *old = var_get(a->name);
        struct sb b = { 0 };
        sb_puts(&b, old ? old : "");
        sb_puts(&b, val);
        int r = var_set(a->name, sb_str(&b), flags);
        sb_free(&b);
        return r;
    }
    return var_set(a->name, val, flags);
}

/* Temporary assignments for builtins and functions (VAR=x cmd). */
struct tmpvar {
    char *name;
    char *old;
    int had, flags;
};

static int tmp_assign(struct assign *as, struct tmpvar *tv, int max, int export)
{
    int n = 0;
    for (struct assign *a = as; a && n < max; a = a->next) {
        struct var *v = var_lookup(a->name);
        tv[n].name = a->name;
        tv[n].had = v && !(v->flags & V_UNSET) && !(v->flags & V_ARRAY);
        tv[n].old = tv[n].had ? xstrdup(var_get(a->name)) : 0;
        tv[n].flags = v ? v->flags : 0;
        n++;
        if (assign_do(a, export ? V_EXPORT : 0) < 0)
            return -n;
    }
    return n;
}

static void tmp_restore(struct tmpvar *tv, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (tv[i].had) {
            var_set(tv[i].name, tv[i].old, 0);
            struct var *v = var_lookup(tv[i].name);
            if (v)
                v->flags = (v->flags & ~V_EXPORT) | (tv[i].flags & V_EXPORT);
        } else {
            var_unset(tv[i].name);
        }
        free(tv[i].old);
    }
}

/* ---- xtrace ---- */

void xtrace(char **argv, int argc, struct assign *as)
{
    if (!opts[OPT_XTRACE])
        return;
    const char *ps4 = var_get("PS4");
    struct sb b = { 0 };
    int w;
    sb_puts(&b, prompt_expand(ps4 ? ps4 : "+%N:%i> ", &w, 0));
    for (struct assign *a = as; a; a = a->next) {
        const char *v = var_get(a->name);
        sb_printf(&b, "%s=%s ", a->name, quote_word(v ? v : ""));
    }
    for (int i = 0; i < argc; i++)
        sb_printf(&b, "%s%s", i ? " " : "", quote_word(argv[i]));
    sb_putc(&b, '\n');
    out_flush();
    write(2, b.s, b.len);
    sb_free(&b);
}

/* ---- running commands ---- */

static int exec_cond(struct cnode *c);

static int exec_external_now(char **argv, char **env) __attribute__((noreturn));
static int exec_external_now(char **argv, char **env)
{
    const char *cmd = argv[0];
    char *path = path_lookup(cmd);
    if (!path) {
        if (strchr(cmd, '/')) {
            struct stat st;
            if (stat(cmd, &st) < 0)
                sh_error("no such file or directory: %s", cmd);
            else if (S_ISDIR(st.st_mode))
                sh_error("is a directory: %s", cmd);
            else
                sh_error("permission denied: %s", cmd);
            _exit(stat(cmd, &st) < 0 ? 127 : 126);
        }
        sh_error("command not found: %s", cmd);
        _exit(127);
    }
    execve(path, argv, env);
    int e = errno;
    if (e == ENOEXEC) {
        /* a script without #!: run it with this shell */
        int n = 0;
        while (argv[n])
            n++;
        char **nargv = amalloc((n + 2) * sizeof(char *));
        nargv[0] = "esh";
        nargv[1] = path;
        for (int i = 1; i <= n; i++)
            nargv[i + 1] = argv[i];
        execve("/bin/esh", nargv, env);
        execve("/bin/sh", nargv, env);
    }
    sh_error("%s: %s", e == EACCES ? "permission denied" : strerror(e), cmd);
    _exit(e == ENOENT ? 127 : 126);
}

static void child_setup(struct job *j, int bg)
{
    sh.in_child = 1;
    if (opts[OPT_MONITOR] && j) {
        pid_t me = getpid();
        setpgid(0, j->pgid ? j->pgid : me);
        if (!bg && sh.tty_fd >= 0)
            tcsetpgrp(sh.tty_fd, j->pgid ? j->pgid : me);
    }
    child_reset_signals();
    if (bg && !opts[OPT_MONITOR]) {
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
    }
    opts[OPT_MONITOR] = 0;
    sh.interactive = 0;
    opts[OPT_INTERACTIVE] = 0;
    if (zle_active)
        zle_active = 0;
}

static void parent_setpgid(struct job *j, pid_t pid)
{
    if (opts[OPT_MONITOR])
        setpgid(pid, j->pgid ? j->pgid : pid);
}

/* Fork one child for a job that runs fn; returns the status (fg) or 0. */
static int run_in_child(struct node *n, int flags, int (*fn)(struct node *, int), const char *text)
{
    int bg = flags & EF_BG;
    if ((flags & EF_FORKED) && !bg) {
        int st = fn(n, EF_FORKED);
        sh_exit(st);
    }
    struct job *j = job_create(text ? text : n->text);
    if (!j)
        return 1;
    pid_t pid = sh_fork();
    if (pid < 0) {
        sh_error("fork failed: %s", strerror(errno));
        job_free(j);
        return 1;
    }
    if (pid == 0) {
        child_setup(j, bg);
        int st = fn(n, EF_FORKED);
        sh_exit(st);
    }
    parent_setpgid(j, pid);
    job_add_proc(j, pid);
    if (bg) {
        job_launch_done(j, 1);
        return 0;
    }
    if (opts[OPT_MONITOR] && sh.tty_fd >= 0)
        tcsetpgrp(sh.tty_fd, j->pgid);
    return job_wait_fg(j);
}

int run_function(const char *name, char **argv, int argc)
{
    struct func *f = func_lookup(name);
    if (!f)
        return 127;
    if (sh.func_depth >= 100) {
        sh_error("maximum nested function level reached");
        return 1;
    }
    struct posargs saved = pos_save();
    pos_set(argv + 1, argc - 1);
    const char *saved_name = sh.script_name;
    sh.func_depth++;
    int saved_loop = sh.loop_depth;
    sh.loop_depth = 0;
    char *body = xstrdup(f->body);
    int st = run_string(body, 0);
    free(body);
    if (sh.returning) {
        st = sh.exit_status;
        sh.returning = 0;
    }
    sh.breaking = sh.continuing = 0;
    sh.loop_depth = saved_loop;
    vars_pop_level(sh.func_depth);
    sh.func_depth--;
    sh.script_name = saved_name;
    pos_restore(saved);
    return st;
}

static int exec_ext_child(struct node *n, int flags);

static int status_of_builtin(const struct builtin *b, int argc, char **argv)
{
    int st = b->fn(argc, argv);
    out_flush();
    return st;
}

/* Precommand modifiers: command, builtin, exec, noglob, nocorrect, - */
enum { PM_NONE = 0, PM_COMMAND = 1, PM_BUILTIN = 2, PM_EXEC = 4 };

static int is_dir_path(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exec_simple(struct node *n, int flags)
{
    sh.lineno = n->line;
    char **raw = n->words;
    int nraw = n->nwords;
    int exflags = EX_FULL;
    int mods = 0;

    /* noglob / nocorrect / - act on the raw words */
    while (nraw > 0) {
        if (!strcmp(raw[0], "noglob")) {
            exflags &= ~EX_GLOB;
        } else if (!strcmp(raw[0], "nocorrect") || !strcmp(raw[0], "-")) {
        } else {
            break;
        }
        raw++;
        nraw--;
    }

    if (nraw > 0 && is_declare_builtin(raw[0]) && !n->assigns) {
        struct fdsave save = { 0 };
        if (apply_redirs(n->redirs, &save) < 0) {
            restore_fds(&save);
            return 1;
        }
        int st = builtin_declare(raw, nraw);
        restore_fds(&save);
        return st;
    }

    struct strv args = { 0 };
    if (expand_words(raw, nraw, &args, exflags) < 0)
        return 1;
    char **argv = args.v;
    int argc = args.n;

    for (;;) {
        if (argc > 1 && !strcmp(argv[0], "command") && argv[1][0] != '-') {
            mods |= PM_COMMAND;
        } else if (argc > 1 && !strcmp(argv[0], "builtin")) {
            mods |= PM_BUILTIN;
        } else if (argc > 1 && !strcmp(argv[0], "exec") && argv[1][0] != '-') {
            mods |= PM_EXEC;
        } else {
            break;
        }
        argv++;
        argc--;
    }

    if (argc == 0) {
        /* assignments and/or redirections only */
        int st = 0;
        sh.last_status = 0;
        for (struct assign *a = n->assigns; a; a = a->next)
            if (assign_do(a, 0) < 0)
                st = 1;
        if (!st)
            st = sh.last_status;    /* x=$(false) sets $? */
        xtrace(0, 0, n->assigns);
        struct fdsave save = { 0 };
        if (n->redirs) {
            if (apply_redirs(n->redirs, &save) < 0)
                st = 1;
            restore_fds(&save);
        }
        return st;
    }

    xtrace(argv, argc, n->assigns);
    const char *cmd = argv[0];
    if (argc == 1 && !strcmp(cmd, "exec")) {
        /* exec with only redirections: they stay */
        return apply_redirs(n->redirs, 0) < 0 ? 1 : 0;
    }
    const struct builtin *b = builtin_lookup(cmd);
    struct func *f = (mods & (PM_COMMAND | PM_BUILTIN | PM_EXEC)) ? 0 : func_lookup(cmd);
    if (mods & PM_BUILTIN) {
        f = 0;
        if (!b) {
            sh_error("no such builtin: %s", cmd);
            return 1;
        }
    }
    if (mods & PM_EXEC)
        b = 0;

    if (f || b) {
        struct tmpvar tv[16];
        int nt = tmp_assign(n->assigns, tv, 16, f != 0);
        if (nt < 0) {
            tmp_restore(tv, -nt);
            return 1;
        }
        struct fdsave save = { 0 };
        int st;
        if (apply_redirs(n->redirs, &save) < 0) {
            st = 1;
        } else if (f) {
            st = run_function(cmd, argv, argc);
        } else {
            st = status_of_builtin(b, argc, argv);
        }
        restore_fds(&save);
        tmp_restore(tv, nt);
        return st;
    }

    /* external command */
    if (!(mods & PM_EXEC) && !(flags & EF_FORKED)) {
        char *path = path_lookup(cmd);
        if (!path) {
            if (opts[OPT_AUTOCD] && argc == 1 && !n->assigns && !n->redirs && is_dir_path(cmd) &&
                sh.interactive)
                return do_cd(cmd, 0);
            if (!strchr(cmd, '/')) {
                sh_error("command not found: %s", cmd);
                if (opts[OPT_CORRECT] && sh.interactive) {
                    const char *s = suggest_command(cmd);
                    if (s)
                        eprintf("esh: did you mean '%s'?\n", s);
                }
                return 127;
            }
        }
    }
    if ((flags & EF_FORKED) || (mods & PM_EXEC)) {
        /* we're the child already (or exec): replace this process */
        out_flush();
        if (apply_redirs(n->redirs, 0) < 0)
            _exit(1);
        for (struct assign *a = n->assigns; a; a = a->next)
            assign_do(a, V_EXPORT);
        if (mods & PM_EXEC) {
            if (sh.interactive)
                hist_save();
            if (zle_active)
                tty_cooked();
            child_reset_signals();
            term_take();
        }
        exec_external_now(argv, var_environ());
    }
    struct node *x = amalloc(sizeof(*x));   /* the child execs the expanded words */
    *x = *n;
    x->words = argv;
    x->nwords = argc;
    return run_in_child(x, flags, exec_ext_child, n->text);
}

static int exec_ext_child(struct node *n, int flags)
{
    (void)flags;
    if (apply_redirs(n->redirs, 0) < 0)
        _exit(1);
    for (struct assign *a = n->assigns; a; a = a->next)
        assign_do(a, V_EXPORT);
    exec_external_now(n->words, var_environ());
}

/* zsh runs the last stage of a pipeline in the shell itself, so
 * `cmd | read x` and `cmd | while read l; do n=...; done` keep their
 * variables. We do that when the last stage is a builtin, a function or a
 * compound command (an external command would need a fork anyway). */
static int runs_inline(struct node *c)
{
    switch (c->t) {
    case N_GROUP: case N_IF: case N_WHILE: case N_UNTIL: case N_FOR: case N_ARITHFOR:
    case N_CASE: case N_ARITH: case N_COND: case N_REPEAT:
        return 1;
    case N_CMD:
        if (!c->nwords || strchr(c->words[0], '$') || strchr(c->words[0], '`') ||
            !strcmp(c->words[0], "exec"))
            return 0;
        return builtin_lookup(c->words[0]) || func_lookup(c->words[0]);
    default:
        return 0;
    }
}

static int exec_pipeline(struct node *n, int flags)
{
    int bg = flags & EF_BG;
    int inline_last = !bg && runs_inline(n->cmds[n->ncmds - 1]);
    struct job *j = job_create(n->text);
    if (!j)
        return 1;
    int in = -1, nfork = inline_last ? n->ncmds - 1 : n->ncmds;
    for (int i = 0; i < nfork; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < n->ncmds && pipe(fds) < 0) {
            sh_error("pipe: %s", strerror(errno));
            break;
        }
        pid_t pid = sh_fork();
        if (pid < 0) {
            sh_error("fork: %s", strerror(errno));
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }
        if (pid == 0) {
            /* with the last stage in the shell, the others share its group */
            child_setup(inline_last ? 0 : j, bg);
            if (in >= 0)
                move_fd(in, 0);
            if (fds[1] >= 0) {
                close(fds[0]);
                move_fd(fds[1], 1);
                if (n->pipe_stderr & (1 << i))
                    dup2(1, 2);
            }
            int st = exec_node(n->cmds[i], EF_FORKED);
            sh_exit(st);
        }
        if (!inline_last)
            parent_setpgid(j, pid);
        job_add_proc(j, pid);
        if (in >= 0)
            close(in);
        if (fds[1] >= 0)
            close(fds[1]);
        in = fds[0];
    }
    if (inline_last) {
        int st = 1;
        if (in >= 0) {
            out_flush();
            int saved = fcntl(0, F_DUPFD_CLOEXEC, 10);
            move_fd(in, 0);
            st = exec_node(n->cmds[n->ncmds - 1], 0);
            out_flush();
            if (saved >= 0) {
                dup2(saved, 0);
                close(saved);
            } else {
                close(0);
            }
        }
        int saved_inter = sh.interactive;
        sh.interactive = 0;         /* the stages' fates are reported by $pipestatus */
        job_wait_fg(j);
        sh.interactive = saved_inter;
        return st;
    }
    if (in >= 0)
        close(in);
    if (bg) {
        job_launch_done(j, 1);
        return 0;
    }
    if (opts[OPT_MONITOR] && sh.tty_fd >= 0 && j->pgid)
        tcsetpgrp(sh.tty_fd, j->pgid);
    return job_wait_fg(j);
}

static int stop_requested(void)
{
    return sh.breaking || sh.continuing || sh.returning || (got_sigint && sh.interactive);
}

/* Loop control after running a body: 1 = leave the loop. */
static int loop_check(void)
{
    if (got_sigint && sh.interactive)
        return 1;
    if (sh.breaking) {
        sh.breaking--;
        return 1;
    }
    if (sh.continuing) {
        if (--sh.continuing > 0)
            return 1;
        return 0;
    }
    return sh.returning != 0;
}

static int exec_subshell_body(struct node *n, int flags)
{
    (void)flags;
    if (apply_redirs(n->redirs, 0) < 0)
        return 1;
    return n->a ? exec_node(n->a, EF_FORKED) : 0;
}

static int exec_bg_body(struct node *n, int flags)
{
    (void)flags;
    return exec_node(n, EF_FORKED);
}

static int exec_case(struct node *n)
{
    char *subj = expand_str(n->subject, EX_TILDE);
    if (!subj)
        return 1;
    int st = 0, fall = 0;
    for (struct casei *ci = n->cases; ci; ci = ci->next) {
        int hit = fall;
        for (int k = 0; k < ci->npats && !hit; k++) {
            char *pat = expand_str(ci->pats[k], EX_PATTERN | EX_TILDE);
            if (pat && pmatch(pat, subj, 0))
                hit = 1;
        }
        if (!hit)
            continue;
        st = ci->body ? exec_node(ci->body, 0) : 0;
        if (!ci->fallthrough || stop_requested())
            return st;
        fall = 1;
    }
    return st;
}

static int exec_compound(struct node *n, int flags)
{
    int st = 0;
    switch (n->t) {
    case N_GROUP:
        st = n->a ? exec_node(n->a, flags & EF_FORKED) : 0;
        break;
    case N_IF:
        sh.errexit_ok++;
        st = exec_node(n->a, 0);
        sh.errexit_ok--;
        if (stop_requested())
            break;
        if (st == 0)
            st = n->b ? exec_node(n->b, 0) : 0;
        else if (n->c)
            st = exec_node(n->c, 0);
        else
            st = 0;
        break;
    case N_WHILE:
    case N_UNTIL:
        st = 0;
        sh.loop_depth++;
        for (;;) {
            struct amark m = amark();       /* each iteration's words are temporary */
            sh.errexit_ok++;
            int c = exec_node(n->a, 0);
            sh.errexit_ok--;
            if (stop_requested()) {
                arelease(m);
                if (loop_check())
                    break;
                continue;
            }
            if ((n->t == N_WHILE) != (c == 0)) {
                arelease(m);
                break;
            }
            st = exec_node(n->b, 0);
            arelease(m);
            if (loop_check())
                break;
            run_traps();
        }
        sh.loop_depth--;
        break;
    case N_FOR: {
        struct strv items = { 0 };
        if (n->list) {
            if (expand_words(n->list, n->nlist, &items, EX_FULL) < 0)
                return 1;
        } else {
            for (int i = 0; i < pos.n; i++)
                strv_push(&items, pos.v[i]);
        }
        sh.loop_depth++;
        for (int i = 0; i < items.n; i++) {
            if (var_set(n->var, items.v[i], 0) < 0) {
                st = 1;
                break;
            }
            struct amark m = amark();
            st = exec_node(n->b, 0);
            arelease(m);
            if (loop_check())
                break;
        }
        sh.loop_depth--;
        break;
    }
    case N_ARITHFOR: {
        long long v;
        char *e = expand_str(n->init, EX_NOBRACE);
        if (!e || arith_eval(e, &v) < 0)
            return 1;
        sh.loop_depth++;
        for (;;) {
            struct amark m = amark();
            char *c = expand_str(n->cond, EX_NOBRACE);
            if (!c || arith_eval(c, &v) < 0) {
                st = 1;
                arelease(m);
                break;
            }
            if (*n->cond && !v) {
                arelease(m);
                break;
            }
            st = exec_node(n->b, 0);
            int leave = loop_check();
            char *s = leave ? 0 : expand_str(n->step, EX_NOBRACE);
            int bad = !leave && (!s || arith_eval(s, &v) < 0);
            arelease(m);
            if (leave)
                break;
            if (bad) {
                st = 1;
                break;
            }
        }
        sh.loop_depth--;
        break;
    }
    case N_REPEAT: {
        char *c = expand_str(n->var, EX_NOBRACE);
        long long count;
        if (!c || arith_eval(c, &count) < 0)
            return 1;
        sh.loop_depth++;
        for (long long i = 0; i < count; i++) {
            struct amark m = amark();
            st = exec_node(n->b, 0);
            arelease(m);
            if (loop_check())
                break;
        }
        sh.loop_depth--;
        break;
    }
    case N_CASE:
        st = exec_case(n);
        break;
    case N_ARITH: {
        char *e = expand_str(n->var, EX_NOBRACE);
        long long v;
        if (!e || arith_eval(e, &v) < 0)
            st = 2;
        else
            st = v == 0;
        break;
    }
    case N_COND:
        st = exec_cond(n->cn);
        break;
    default:
        break;
    }
    return st;
}

int exec_node(struct node *n, int flags)
{
    if (!n)
        return 0;
    int st = 0;
    switch (n->t) {
    case N_CMD:
        st = exec_simple(n, flags);
        break;
    case N_PIPE:
        if (flags & EF_BG)
            st = exec_pipeline(n, EF_BG);
        else
            st = exec_pipeline(n, 0);
        break;
    case N_SEQ:
        exec_node(n->a, 0);
        if (stop_requested())
            return sh.last_status;
        st = exec_node(n->b, flags & EF_FORKED);
        break;
    case N_AND:
    case N_OR:
        sh.errexit_ok++;
        st = exec_node(n->a, 0);
        sh.errexit_ok--;
        if (stop_requested())
            return st;
        if ((n->t == N_AND) == (st == 0))
            st = exec_node(n->b, flags & EF_FORKED);
        break;
    case N_NOT:
        sh.errexit_ok++;
        st = !exec_node(n->a, flags & EF_FORKED);
        sh.errexit_ok--;
        break;
    case N_BG: {
        struct node *a = n->a;
        if (a->t == N_PIPE)
            st = exec_pipeline(a, EF_BG);
        else
            st = run_in_child(a, EF_BG, exec_bg_body, a->text);
        break;
    }
    case N_SUBSHELL:
        st = run_in_child(n, flags & EF_FORKED, exec_subshell_body, n->text ? n->text : "( ... )");
        break;
    case N_FUNC:
        func_define(n->var, n->init ? n->init : "{ }");
        st = 0;
        break;
    case N_TIME: {
        long long t0 = now_ms();
        st = exec_node(n->a, flags & EF_FORKED);
        long long t = now_ms() - t0;
        eprintf("%s  %lld.%03lld total\n", n->a && n->a->text && *n->a->text ? n->a->text : "shell",
                t / 1000, t % 1000);
        break;
    }
    default: {
        struct fdsave save = { 0 };
        if (n->redirs && apply_redirs(n->redirs, &save) < 0) {
            restore_fds(&save);
            st = 1;
            break;
        }
        st = exec_compound(n, flags);
        restore_fds(&save);
        break;
    }
    }
    sh.last_status = st;
    if (pending_traps)
        run_traps();
    if (st && opts[OPT_ERREXIT] && !sh.errexit_ok && !stop_requested() &&
        (n->t == N_CMD || n->t == N_PIPE || n->t == N_SUBSHELL || n->t == N_COND || n->t == N_ARITH))
        sh_exit(st);
    return st;
}

/* ---- [[ ]] ---- */

static int file_test(const char *op, const char *path)
{
    struct stat st;
    switch (op[1]) {
    case 'e': case 'a': return stat(path, &st) == 0;
    case 'f': return stat(path, &st) == 0 && S_ISREG(st.st_mode);
    case 'd': return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    case 'c': return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
    case 'p': return stat(path, &st) == 0 && S_ISFIFO(st.st_mode);
    case 'b': case 'S': case 'h': case 'L': return 0;
    case 's': return stat(path, &st) == 0 && st.st_size > 0;
    case 'r': return access(path, R_OK) == 0;
    case 'w': return access(path, W_OK) == 0;
    case 'x': return access(path, X_OK) == 0;
    case 'u': return stat(path, &st) == 0 && (st.st_mode & S_ISUID);
    case 'g': return stat(path, &st) == 0 && (st.st_mode & S_ISGID);
    case 'k': return 0;
    case 'O': return stat(path, &st) == 0 && st.st_uid == geteuid();
    case 'G': return stat(path, &st) == 0 && st.st_gid == getegid();
    case 'N': return stat(path, &st) == 0;
    }
    return 0;
}

int test_unary(const char *op, const char *arg)
{
    if (!strcmp(op, "-n"))
        return *arg != 0;
    if (!strcmp(op, "-z"))
        return *arg == 0;
    if (!strcmp(op, "-t"))
        return isatty(atoi(arg));
    if (!strcmp(op, "-o")) {
        int no = !strncasecmp_(arg, "no", 2) && opt_find(arg) < 0;
        int i = opt_find(no ? arg + 2 : arg);
        return i >= 0 && (opts[i] != 0) != no;
    }
    if (!strcmp(op, "-v"))
        return var_get(arg) != 0;
    return file_test(op, arg);
}

int test_binary(const char *l, const char *op, const char *r, int pattern)
{
    if (!strcmp(op, "=") || !strcmp(op, "=="))
        return pattern ? pmatch(r, l, 0) : !strcmp(l, r);
    if (!strcmp(op, "!="))
        return pattern ? !pmatch(r, l, 0) : strcmp(l, r) != 0;
    if (!strcmp(op, "<"))
        return strcmp(l, r) < 0;
    if (!strcmp(op, ">"))
        return strcmp(l, r) > 0;
    if (!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
        struct stat a, b;
        int ha = stat(l, &a) == 0, hb = stat(r, &b) == 0;
        long long ta = ha ? a.st_mtim.tv_sec * 1000000000LL + a.st_mtim.tv_nsec : 0;
        long long tb = hb ? b.st_mtim.tv_sec * 1000000000LL + b.st_mtim.tv_nsec : 0;
        if (op[1] == 'n')
            return ha && (!hb || ta > tb);
        if (op[1] == 'o')
            return hb && (!ha || ta < tb);
        return ha && hb && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    }
    long long x, y;
    if (pattern) {
        if (arith_eval(l, &x) < 0 || arith_eval(r, &y) < 0)
            return -1;
    } else {
        if (!is_number(l) || !is_number(r)) {
            sh_error("integer expression expected");
            return -1;
        }
        x = strtol(l, 0, 10);
        y = strtol(r, 0, 10);
    }
    if (!strcmp(op, "-eq")) return x == y;
    if (!strcmp(op, "-ne")) return x != y;
    if (!strcmp(op, "-lt")) return x < y;
    if (!strcmp(op, "-le")) return x <= y;
    if (!strcmp(op, "-gt")) return x > y;
    if (!strcmp(op, "-ge")) return x >= y;
    sh_error("unknown condition: %s", op);
    return -1;
}

/* Returns 1 true, 0 false, -1 error. */
static int cond_eval(struct cnode *c)
{
    switch (c->op) {
    case C_AND: {
        int a = cond_eval(c->a);
        return a <= 0 ? a : cond_eval(c->b);
    }
    case C_OR: {
        int a = cond_eval(c->a);
        return a != 0 ? a : cond_eval(c->b);
    }
    case C_NOT: {
        int a = cond_eval(c->a);
        return a < 0 ? a : !a;
    }
    case C_WORD: {
        char *w = expand_str(c->l, EX_TILDE);
        return w ? *w != 0 : -1;
    }
    case C_UNARY: {
        char *w = expand_str(c->l, EX_TILDE);
        return w ? test_unary(c->opname, w) : -1;
    }
    case C_BINARY: {
        char *l = expand_str(c->l, EX_TILDE);
        if (!l)
            return -1;
        if (!strcmp(c->opname, "=~")) {
            sh_error("=~: regular expressions are not supported; use == with a pattern");
            return -1;
        }
        int pat = !strcmp(c->opname, "=") || !strcmp(c->opname, "==") || !strcmp(c->opname, "!=");
        char *r = expand_str(c->r, pat ? EX_PATTERN | EX_TILDE : EX_TILDE);
        if (!r)
            return -1;
        return test_binary(l, c->opname, r, 1);
    }
    }
    return -1;
}

static int exec_cond(struct cnode *c)
{
    int r = cond_eval(c);
    return r < 0 ? 2 : !r;
}

/* ---- running source text ---- */

int run_string(const char *src, const char *name)
{
    struct parser p;
    parser_init(&p, src);
    p.final = 1;
    int st = sh.last_status;
    for (;;) {
        struct amark m = amark();
        struct node *n = parse_command(&p);
        if (!n) {
            if (p.status == P_ERROR || p.status == P_INCOMPLETE) {
                sh.lineno = p.line;
                sh_error("%s", p.err[0] ? p.err : "parse error");
                st = sh.last_status = 1;
                if (name)
                    st = 1;
            }
            arelease(m);
            break;
        }
        if (opts[OPT_VERBOSE] && n->text) {
            eprintf("%s\n", n->text);
        }
        st = exec_node(n, 0);
        arelease(m);
        if (sh.returning || sh.breaking || sh.continuing)
            break;
        if (sh.interactive && got_sigint && !sh.in_child)
            break;
    }
    parser_free(&p);
    return st;
}

int run_file(const char *path, int must_exist)
{
    int len;
    char *text = read_file(path, &len);
    if (!text) {
        if (must_exist) {
            sh_error("%s: %s", strerror(errno), path);
            return 127;
        }
        return 0;
    }
    if ((int)strlen(text) != len) {
        sh_error("%s: cannot execute binary file", path);
        free(text);
        return 126;
    }
    const char *saved = sh.script_name;
    int saved_line = sh.lineno;
    sh.script_name = path;
    int st = run_string(text, path);
    if (sh.returning) {
        st = sh.exit_status;
        sh.returning = 0;
    }
    sh.script_name = saved;
    sh.lineno = saved_line;
    free(text);
    return st;
}
