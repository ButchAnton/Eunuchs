/*
 * esh: start-up and the read-eval loop.
 *
 *   esh                      interactive (if on a terminal)
 *   esh -c 'cmds' [$0 args]  run a string
 *   esh file [args]          run a script
 *   esh -i                   prompts even without a terminal (rshd)
 *   esh -l                   login shell (also when argv[0] starts with -)
 *   -x -e -u -f, -o option   as with set
 *
 * Interactive shells read /etc/eshrc and then ~/.eshrc. Invoked as "sh",
 * esh splits unquoted parameter expansions on IFS, as POSIX scripts expect.
 */
#include "esh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>

struct sh sh;

void out_flush(void)
{
    fflush_stdout();
}

const char *err_prefix(void)
{
    static char buf[160];
    if (sh.interactive || !sh.script_name)
        return "esh";
    snprintf(buf, sizeof(buf), "%s:%d", sh.script_name, sh.lineno);
    return buf;
}

void sh_error(const char *fmt, ...)
{
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    out_flush();
    eprintf("%s: %s\n", err_prefix(), msg);
}

void sh_exit(int status)
{
    static int exiting;
    if (!exiting && !sh.in_child) {
        exiting = 1;
        const char *t = trap_get(0);
        if (t && *t) {
            char *code = xstrdup(t);
            trap_set(0, "-");
            run_string(code, 0);
            free(code);
        }
        if (sh.interactive) {
            hist_save();
            if (opts[OPT_HUP])
                jobs_hup_all();
        }
    }
    out_flush();
    _exit(status & 0xff);
}

/* ---- reading lines without the editor ---- */

static char *read_plain_line(int fd)
{
    struct sb b = { 0 };
    for (;;) {
        char c;
        long n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                if (got_sigint && sh.interactive) {
                    got_sigint = 0;
                    sb_free(&b);
                    return xstrdup("");
                }
                if (got_sigchld)
                    jobs_reap(0);
                continue;
            }
            break;
        }
        if (n == 0) {
            if (!b.len) {
                sb_free(&b);
                return 0;
            }
            break;
        }
        if (c == '\n')
            break;
        if (c == '\r')
            continue;       /* lines from the network (rshd, nc) */
        sb_putc(&b, c);
    }
    char *r = xstrdup(sb_str(&b));
    sb_free(&b);
    return r;
}

static int use_zle;

static char *read_line(const char *ps_var, const char *dflt, const char *ctx)
{
    const char *fmt = var_get(ps_var);
    if (!fmt && !strcmp(ps_var, "PS1"))
        fmt = var_get("PROMPT");
    if (!fmt)
        fmt = dflt;
    /* %_ in PS2: the open construct (zsh) */
    struct sb f = { 0 };
    for (const char *p = fmt; *p; p++) {
        if (p[0] == '%' && p[1] == '_') {
            sb_puts(&f, ctx && *ctx ? ctx : "");
            p++;
        } else {
            sb_putc(&f, *p);
        }
    }
    int w;
    if (use_zle) {
        char *ps = prompt_expand(sb_str(&f), &w, 1);
        sb_free(&f);
        const char *rfmt = !strcmp(ps_var, "PS1") ? var_get("RPROMPT") : var_get("RPROMPT2");
        if (!rfmt && !strcmp(ps_var, "PS1"))
            rfmt = var_get("RPS1");
        int rw = 0;
        char *rps = rfmt && *rfmt ? prompt_expand(rfmt, &rw, 1) : 0;
        return zle_read(ps, w, rps, rw);
    }
    if (opts[OPT_INTERACTIVE]) {
        char *ps = prompt_expand(sb_str(&f), &w, 0);
        printf("%s", ps);
        out_flush();
    }
    sb_free(&f);
    return read_plain_line(0);
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

/* ---- the interactive loop ---- */

static void repl(void)
{
    for (;;) {
        struct amark m = amark();
        got_sigint = 0;
        if (opts[OPT_MONITOR])
            jobs_reap(1);
        run_traps();
        run_hook("precmd", 0);
        out_flush();

        char *line = read_line("PS1", "%n@%m:%~%# ", 0);
        if (!line) {
            if (opts[OPT_CHECKJOBS] && jobs_any_stopped()) {
                static int warned;
                if (!warned) {
                    eprintf("esh: you have suspended jobs.\n");
                    warned = 1;
                    arelease(m);
                    continue;
                }
            }
            arelease(m);
            break;
        }
        /* more lines until the command is complete */
        struct sb cmd = { 0 };
        sb_puts(&cmd, line);
        free(line);
        int broken = 0;
        for (;;) {
            const char *what = 0;
            char err[160];
            enum pstatus st = parse_check(sb_str(&cmd), &what, err, sizeof(err));
            if (st != P_INCOMPLETE)
                break;
            char *more = read_line("PS2", "%_> ", what);
            if (!more || got_sigint) {
                free(more);
                broken = 1;
                break;
            }
            sb_putc(&cmd, '\n');
            sb_puts(&cmd, more);
            free(more);
        }
        if (broken) {
            sb_free(&cmd);
            arelease(m);
            continue;
        }
        char *text = xstrdup(sb_str(&cmd));
        sb_free(&cmd);
        int blank = 1;
        for (char *p = text; *p; p++)
            if (!is_space(*p))
                blank = 0;
        if (blank) {
            free(text);
            arelease(m);
            continue;
        }
        if (opts[OPT_BANGHIST]) {
            char *ex;
            int r = hist_expand(text, &ex);
            if (r < 0) {
                hist_add(text);
                free(text);
                sh.last_status = 1;
                arelease(m);
                continue;
            }
            if (r > 0) {
                free(text);
                text = ex;
                printf("%s\n", text);       /* show what will run */
                out_flush();
            }
        }
        hist_add(text);
        run_hook("preexec", text);
        zle_cmd_ran = 1;
        got_sigint = 0;
        run_string(text, 0);
        free(text);
        out_flush();
        arelease(m);
    }
}

/* Non-interactive input from stdin (esh < file, cmd | esh): line by line, so
 * commands that read stdin themselves see the rest. */
static void run_stdin(void)
{
    struct sb cmd = { 0 };
    for (;;) {
        char *line = read_plain_line(0);
        if (!line) {
            if (cmd.len)
                run_string(sb_str(&cmd), 0);
            break;
        }
        if (cmd.len)
            sb_putc(&cmd, '\n');
        sb_puts(&cmd, line);
        free(line);
        const char *what;
        char err[160];
        if (parse_check(sb_str(&cmd), &what, err, sizeof(err)) == P_INCOMPLETE)
            continue;
        struct amark m = amark();
        sh.lineno++;
        run_string(sb_str(&cmd), 0);
        arelease(m);
        sb_reset(&cmd);
    }
    sb_free(&cmd);
}

/* ---- start-up ---- */

static void set_defaults(void)
{
    opts[OPT_NOMATCH] = 1;
    opts[OPT_GLOB] = 1;
    opts[OPT_NOTIFY] = 1;
    opts[OPT_INTERACTIVECOMMENTS] = 1;
    opts[OPT_PROMPTSP] = 1;
    opts[OPT_AUTOSUGGEST] = 1;
    opts[OPT_HIGHLIGHT] = 1;
    opts[OPT_PROMPTMARKS] = 1;
    opts[OPT_BEEP] = 1;
    opts[OPT_CHECKJOBS] = 1;
    opts[OPT_HUP] = 1;
    opts[OPT_AUTOLIST] = 1;
    opts[OPT_AUTOMENU] = 1;
    opts[OPT_BANGHIST] = 1;
    sh.cols = 80;
    sh.rows = 24;
    sh.tty_fd = -1;
}

static void init_vars(const char *argv0)
{
    vars_init(environ);
    const char *lvl = var_get("SHLVL");
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (lvl ? atoi(lvl) : 0) + 1);
    var_set("SHLVL", buf, V_EXPORT);
    const char *user = user_name();
    if (!var_get("USER"))
        var_set("USER", user, V_EXPORT);
    if (!var_get("LOGNAME"))
        var_set("LOGNAME", user, V_EXPORT);
    if (!var_get("HOME")) {
        char home[256];
        if (user_home(user, home, sizeof(home)) == 0)
            var_set("HOME", home, V_EXPORT);
        else
            var_set("HOME", "/", V_EXPORT);
    }
    if (!var_get("SHELL"))
        var_set("SHELL", "/bin/esh", V_EXPORT);
    var_set("HOST", host_name(), 0);
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)))
        var_set("PWD", cwd, V_EXPORT);
    snprintf(buf, sizeof(buf), "%d", getppid());
    var_set("PPID", buf, 0);
    snprintf(buf, sizeof(buf), "%d", getuid());
    var_set("UID", buf, 0);
    snprintf(buf, sizeof(buf), "%d", geteuid());
    var_set("EUID", buf, 0);
    var_set("ESH_VERSION", ESH_VERSION, 0);
    var_set("ZSH_NAME", "esh", 0);
    if (!var_get("PS2"))
        var_set("PS2", "%_> ", 0);
    if (!var_get("PS4"))
        var_set("PS4", "+%N:%i> ", 0);
    if (!var_get("TERM"))
        var_set("TERM", "xterm-256color", V_EXPORT);
    (void)argv0;
}

static void take_terminal(void)
{
    sh.tty_fd = 0;
    /* wait until we're in the foreground */
    for (;;) {
        pid_t fg = tcgetpgrp(0);
        if (fg < 0 || fg == getpgrp())
            break;
        kill(-getpgrp(), SIGTTIN);
    }
    signals_init_interactive();
    setpgid(0, 0);
    sh.pgid = getpgrp();
    tcsetpgrp(0, sh.pgid);
    term_save_shell_modes();
}

static int usage(void)
{
    eprintf("usage: esh [-ilxeuf] [-o option] [-c command [name args...] | script [args...]]\n");
    return 2;
}

int main(int argc, char **argv)
{
    set_defaults();
    sh.start_ms = now_ms();
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (base[0] == '-') {
        opts[OPT_LOGIN] = 1;
        base++;
    }
    if (!strcmp(base, "sh"))
        opts[OPT_SHWORDSPLIT] = 1;
    sh.script_name = 0;

    const char *command = 0;
    int force_interactive = 0, read_stdin = 0, i = 1;
    for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("esh %s (Eunuchs)\n", ESH_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help"))
            return usage();
        int on = argv[i][0] == '-';
        for (const char *c = argv[i] + 1; *c; c++) {
            switch (*c) {
            case 'c':
                if (i + 1 >= argc)
                    return usage();
                command = argv[++i];
                goto opts_done_char;
            case 'i': force_interactive = on; break;
            case 'l': opts[OPT_LOGIN] = on; break;
            case 's': read_stdin = 1; break;
            case 'x': opts[OPT_XTRACE] = on; break;
            case 'e': opts[OPT_ERREXIT] = on; break;
            case 'u': opts[OPT_NOUNSET] = on; break;
            case 'v': opts[OPT_VERBOSE] = on; break;
            case 'f': opts[OPT_GLOB] = !on; break;
            case 'o': {
                if (i + 1 >= argc)
                    return usage();
                int k = opt_find(argv[++i]);
                if (k < 0) {
                    eprintf("esh: no such option: %s\n", argv[i]);
                    return 2;
                }
                opts[k] = on;
                goto opts_done_char;
            }
            default:
                return usage();
            }
        }
    opts_done_char:;
    }

    init_vars(argv[0]);

    if (command) {
        sh.script_name = i < argc ? argv[i] : "esh";
        if (i + 1 < argc)
            pos_set(argv + i + 1, argc - i - 1);
        int st = run_string(command, 0);
        sh_exit(sh.returning ? sh.exit_status : st);
    }
    if (i < argc && !read_stdin) {
        sh.script_name = argv[i];
        pos_set(argv + i + 1, argc - i - 1);
        int st = run_file(argv[i], 1);
        sh_exit(st);
    }
    if (i < argc)
        pos_set(argv + i, argc - i);

    int tty = isatty(0) && isatty(2);
    sh.interactive = force_interactive || (tty && !read_stdin);
    opts[OPT_INTERACTIVE] = sh.interactive;
    if (!sh.interactive) {
        run_stdin();
        sh_exit(sh.last_status);
    }
    if (tty) {
        opts[OPT_MONITOR] = 1;
        take_terminal();
    } else {
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
    }
    use_zle = tty && isatty(1) && zle_init();

    if (opts[OPT_LOGIN]) {
        run_file("/etc/esh_profile", 0);
        const char *home = var_get("HOME");
        if (home) {
            struct sb b = { 0 };
            sb_puts(&b, home);
            sb_puts(&b, "/.esh_profile");
            run_file(sb_str(&b), 0);
            sb_free(&b);
        }
    }
    run_file("/etc/eshrc", 0);
    const char *home = var_get("HOME");
    if (home) {
        struct sb b = { 0 };
        sb_puts(&b, strcmp(home, "/") ? home : "");
        sb_puts(&b, "/.eshrc");
        run_file(sb_str(&b), 0);
        sb_free(&b);
    }
    hist_init();
    repl();
    if (!use_zle && tty)
        printf("\n");
    sh_exit(sh.last_status);
}
