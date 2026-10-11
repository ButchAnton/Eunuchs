/*
 * esh: completion.
 *
 * Commands in command position; files elsewhere; and context-specific
 * candidates: directories for cd, jobs for fg/bg/kill %, signal names,
 * options (with descriptions) for builtins and Eunuchs programs, variable
 * names after $, user names after ~, shell options for setopt. Matching is
 * by prefix, falling back to case-insensitive matching when nothing
 * matches exactly.
 */
#include "esh.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- option tables ---- */

struct optdesc {
    const char *cmd;
    const char *opt;
    const char *desc;
};

static const struct optdesc options[] = {
    { "ls", "-l", "long listing" },
    { "ls", "-a", "include . files" },
    { "ls", "-A", "include . files except . and .." },
    { "ls", "-d", "directories themselves" },
    { "ls", "-R", "recursive" },
    { "ls", "-1", "one per line" },
    { "ls", "-t", "newest first" },
    { "ls", "-S", "largest first" },
    { "ls", "-r", "reverse order" },
    { "ls", "-h", "human-readable sizes" },
    { "ls", "-i", "inode numbers" },
    { "ls", "-s", "sizes in blocks" },
    { "ls", "-F", "append / * | by type" },
    { "ls", "-v", "natural order of numbers" },
    { "ls", "--color", "colors (always, auto, never)" },
    { "ls", "--full-time", "complete times" },
    { "ls", "--group-directories-first", "directories first" },
    { "ls", "--help", "all options" },
    { "grep", "-v", "print lines that do not match" },
    { "grep", "-n", "number the lines" },
    { "grep", "-i", "ignore case" },
    { "grep", "-c", "count matching lines" },
    { "nc", "-l", "listen for a connection" },
    { "nc", "-u", "UDP instead of TCP" },
    { "nc", "-k", "keep listening after a connection ends" },
    { "nc", "-v", "verbose" },
    { "ping", "-c", "number of echo requests" },
    { "ping", "-i", "interval between requests (ms)" },
    { "ping", "-s", "payload size (bytes)" },
    { "ping", "-W", "time to wait for a reply (ms)" },
    { "netstat", "-t", "TCP sockets" },
    { "netstat", "-u", "UDP sockets" },
    { "netstat", "-s", "protocol statistics" },
    { "netstat", "-i", "interfaces" },
    { "netstat", "-a", "everything" },
    { "wc", "-l", "lines" },
    { "wc", "-w", "words" },
    { "wc", "-c", "bytes" },
    { "head", "-n", "number of lines" },
    { "tail", "-n", "number of lines" },
    { "httpd", "-p", "port to listen on" },
    { "httpd", "-r", "document root" },
    { "stress", "-j", "number of worker processes" },
    { "stress", "-t", "run time (seconds)" },
    { "stress", "-n", "iterations per worker" },
    { "stress", "-v", "verbose" },
    { "ps", "-e", "every process" },
    { "ps", "-f", "full format (UID PPID C STIME, arguments)" },
    { "ps", "-F", "extra full (adds SZ RSS PSR)" },
    { "ps", "-l", "long format (F S PRI NI SZ WCHAN)" },
    { "ps", "-j", "jobs format (PGID SID)" },
    { "ps", "-H", "show the process tree" },
    { "ps", "-o", "choose columns (ps -o help lists them)" },
    { "ps", "-O", "default columns plus these" },
    { "ps", "-p", "by process id" },
    { "ps", "-u", "by effective user" },
    { "ps", "-C", "by command name" },
    { "ps", "-t", "by terminal" },
    { "ps", "-N", "invert the selection" },
    { "ps", "-w", "wide output" },
    { "ps", "--sort", "sort by [+-]column,..." },
    { "ps", "--forest", "tree with \\_ lines" },
    { "ps", "--no-headers", "no header line" },
    { "ps", "--help", "summary of options" },
    { "more", "-N", "line numbers" },
    { "more", "-i", "ignore case in searches" },
    { "more", "-R", "show colors" },
    { "more", "-s", "squeeze blank lines" },
    { "more", "-F", "quit if it fits on one screen" },
    { "more", "-E", "quit at the end" },
    { "less", "-N", "line numbers" },
    { "less", "-i", "ignore case in searches" },
    { "less", "-R", "show colors" },
    { "less", "-s", "squeeze blank lines" },
    { "less", "-F", "quit if it fits on one screen" },
    { "less", "-E", "quit at the end" },
    { "kill", "-l", "list signal names" },
    { "kill", "-s", "signal to send" },
    { "echo", "-n", "no trailing newline" },
    { "echo", "-e", "interpret escapes (the default)" },
    { "echo", "-E", "don't interpret escapes" },
    { "print", "-n", "no trailing newline" },
    { "print", "-r", "raw: no escapes" },
    { "print", "-l", "one argument per line" },
    { "print", "-N", "NUL-separated" },
    { "print", "-P", "prompt escapes (%F{red} ...)" },
    { "print", "-s", "add to the history instead" },
    { "print", "-u", "output to a file descriptor" },
    { "read", "-r", "raw: backslashes are literal" },
    { "read", "-s", "silent: don't echo" },
    { "read", "-p", "prompt" },
    { "read", "-t", "timeout (seconds)" },
    { "read", "-n", "read this many characters" },
    { "read", "-A", "into an array" },
    { "read", "-d", "delimiter instead of newline" },
    { "jobs", "-l", "with process ids" },
    { "jobs", "-p", "process group ids only" },
    { "set", "-e", "exit on error" },
    { "set", "-u", "error on unset variables" },
    { "set", "-x", "trace commands" },
    { "set", "-v", "print input as it is read" },
    { "set", "-f", "no filename generation" },
    { "set", "-o", "set an option by name" },
    { "set", "-A", "assign an array" },
    { "history", "-c", "clear the history" },
    { "history", "-n", "without numbers" },
    { "typeset", "-x", "export" },
    { "typeset", "-r", "read-only" },
    { "typeset", "-i", "integer" },
    { "typeset", "-a", "array" },
    { "typeset", "-l", "lower case" },
    { "typeset", "-u", "upper case" },
    { "typeset", "-g", "global (inside a function)" },
    { "typeset", "-f", "functions" },
    { "typeset", "-p", "print" },
    { "local", "-x", "export" },
    { "local", "-r", "read-only" },
    { "local", "-i", "integer" },
    { "local", "-a", "array" },
    { "export", "-n", "remove the export attribute" },
    { "export", "-p", "list exported variables" },
    { "type", "-a", "all the ways a name can be run" },
    { "whence", "-v", "verbose (like type)" },
    { "whence", "-c", "csh style (like which)" },
    { "whence", "-p", "path only" },
    { "whence", "-a", "all matches" },
    { "which", "-a", "all matches" },
    { "umask", "-S", "symbolic output" },
    { "dirs", "-v", "numbered, one per line" },
    { "dirs", "-c", "clear the stack" },
    { "unset", "-f", "functions" },
    { "unset", "-v", "variables" },
    { "trap", "-l", "list signals" },
    { "trap", "-p", "print traps" },
    { "command", "-v", "describe briefly" },
    { "command", "-V", "describe verbosely" },
    { "profile", "start", "start sampling [hz]" },
    { "profile", "stop", "stop sampling" },
    { "profile", "report", "print a report" },
    { "profile", "clear", "clear the samples" },
    { "profile", "dump", "write the samples to the host" },
    { "profile", "run", "profile a command" },
    { "dbg", "break", "stop in the kernel monitor" },
    { "dbg", "mark", "trace event + log line" },
    { "dbg", "vm", "print my memory map" },
    { "dbg", "files", "print my open files" },
    { "dbg", "ps", "print the process table" },
    { "dbg", "fscrash", "crash at a filesystem commit point" },
    { 0, 0, 0 },
};

/* Commands whose first argument is a subcommand from the table. */
static int is_subcommand_cmd(const char *cmd)
{
    return !strcmp(cmd, "profile") || !strcmp(cmd, "dbg");
}

/* ---- candidates ---- */

static void add(struct cands *c, const char *word, const char *disp, const char *desc, int kind,
                int suffix)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i].word, word))
            return;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        struct cand *v = amalloc(c->cap * sizeof(*v));
        if (c->n)
            memcpy(v, c->v, c->n * sizeof(*v));
        c->v = v;
    }
    struct cand *k = &c->v[c->n++];
    k->word = astrdup(word);
    k->disp = astrdup(disp ? disp : word);
    k->desc = desc ? astrdup(desc) : 0;
    k->kind = kind;
    k->suffix = suffix;
}

static int match(const char *name, const char *prefix, int fold)
{
    return fold ? !strncasecmp_(name, prefix, strlen(prefix)) : !strncmp(name, prefix, strlen(prefix));
}

static void sort_cands(struct cands *c)
{
    for (int i = 1; i < c->n; i++)
        for (int j = i; j > 0 && strcmp(c->v[j - 1].disp, c->v[j].disp) > 0; j--) {
            struct cand t = c->v[j];
            c->v[j] = c->v[j - 1];
            c->v[j - 1] = t;
        }
}

/* Unescape a typed word: backslashes and quotes removed. */
static char *unescape(const char *s, int n)
{
    char *d = amalloc(n + 1), *o = d;
    char q = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!q && (c == '\'' || c == '"')) {
            q = c;
            continue;
        }
        if (q && c == q) {
            q = 0;
            continue;
        }
        if (c == '\\' && q != '\'' && i + 1 < n) {
            *o++ = s[++i];
            continue;
        }
        *o++ = c;
    }
    *o = 0;
    return d;
}

static void files(struct cands *c, const char *prefix, int dirs_only, int exec_only, int fold)
{
    const char *slash = strrchr(prefix, '/');
    char *dirpart = slash ? astrndup(prefix, slash - prefix + 1) : astrdup("");
    const char *base = slash ? slash + 1 : prefix;
    /* the directory to read: ~ expanded */
    char *dir = dirpart;
    if (dirpart[0] == '~') {
        const char *rest = strchr(dirpart, '/');
        char *user = astrndup(dirpart + 1, rest ? rest - dirpart - 1 : (long)strlen(dirpart) - 1);
        char *home = tilde_expand_name(user);
        if (home) {
            struct sb b = { 0 };
            sb_puts(&b, home);
            sb_puts(&b, rest ? rest : "");
            dir = sb_adup(&b);
            sb_free(&b);
        }
    }
    DIR *d = opendir(*dir ? dir : ".");
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (name[0] == '.' && base[0] != '.')
            continue;
        if (!match(name, base, fold))
            continue;
        struct sb full = { 0 };
        sb_puts(&full, *dir ? dir : "");
        sb_puts(&full, name);
        struct stat st;
        int isdir = stat(sb_str(&full), &st) == 0 && S_ISDIR(st.st_mode);
        int isexec = !isdir && stat(sb_str(&full), &st) == 0 && (st.st_mode & 0111);
        sb_free(&full);
        if (dirs_only && !isdir)
            continue;
        if (exec_only && !isdir && !isexec)
            continue;
        struct sb w = { 0 };
        sb_puts(&w, dirpart);
        sb_puts(&w, name);
        char *disp = isdir ? astrdup(name) : astrdup(name);
        if (isdir) {
            struct sb ds = { 0 };
            sb_puts(&ds, name);
            sb_putc(&ds, '/');
            disp = sb_adup(&ds);
            sb_free(&ds);
        }
        /* keep a typed ~ as is; escape the rest */
        char *word = sb_adup(&w);
        sb_free(&w);
        char *esc;
        if (word[0] == '~') {
            const char *s = strchr(word, '/');
            struct sb e = { 0 };
            sb_putn(&e, word, s ? s - word : (int)strlen(word));
            if (s)
                sb_puts(&e, escape_for_completion(s));
            esc = sb_adup(&e);
            sb_free(&e);
        } else {
            esc = escape_for_completion(word);
        }
        add(c, esc, disp, 0, isdir ? K_DIR : isexec ? K_EXEC : K_PLAIN, isdir ? '/' : ' ');
    }
    closedir(d);
}

static void commands(struct cands *c, const char *prefix, int fold)
{
    struct strv v = { 0 };
    int kinds[5];
    int start[5];
    start[0] = v.n; builtin_names(fold ? "" : prefix, &v); kinds[0] = K_BUILTIN;
    start[1] = v.n; alias_names(fold ? "" : prefix, &v); kinds[1] = K_ALIAS;
    start[2] = v.n; func_names(fold ? "" : prefix, &v); kinds[2] = K_FUNC;
    start[3] = v.n; path_commands(fold ? "" : prefix, &v); kinds[3] = K_EXEC;
    start[4] = v.n;
    for (int k = 0; k < 4; k++)
        for (int i = start[k]; i < start[k + 1]; i++)
            if (match(v.v[i], prefix, fold) && strcmp(v.v[i], "[") && strcmp(v.v[i], ".") &&
                strcmp(v.v[i], ":"))
                add(c, escape_for_completion(v.v[i]), v.v[i], 0, kinds[k], ' ');
    static const char *const kw[] = { "if", "while", "until", "for", "case", "function",
                                      "repeat", "time", 0 };
    for (int i = 0; kw[i]; i++)
        if (match(kw[i], prefix, fold))
            add(c, kw[i], kw[i], 0, K_KEYWORD, ' ');
    if (opts[OPT_AUTOCD] && *prefix)
        files(c, prefix, 1, 0, fold);
}

static void jobs_cands(struct cands *c, const char *prefix)
{
    for (int i = 0; i < MAXJOBS; i++) {
        struct job *j = job_by_index(i);
        if (!j)
            continue;
        char w[16];
        snprintf(w, sizeof(w), "%%%d", j->id);
        if (match(w, prefix, 0)) {
            char desc[160];
            snprintf(desc, sizeof(desc), "%s  %s", job_state_text(j), j->text);
            add(c, w, w, desc, K_JOB, ' ');
        }
    }
}

static void signal_cands(struct cands *c, const char *prefix, const char *lead)
{
    for (int s = 1; s < 32; s++) {
        const char *n = signame(s);
        if (!strcmp(n, "?"))
            continue;
        char w[24];
        snprintf(w, sizeof(w), "%s%s", lead, n);
        if (match(w, prefix, 1))
            add(c, w, w, sigdesc(s), K_PLAIN, ' ');
    }
}

static int collect(struct cands *c, const char *buf, int start, int end, const char *prefix,
                   int cmdpos, const char *cmd, int argi, int fold)
{
    /* $variables */
    if (prefix[0] == '$') {
        int brace = prefix[1] == '{';
        const char *p = prefix + 1 + brace;
        struct strv v = { 0 };
        var_names(fold ? "" : p, &v);
        for (int i = 0; i < v.n; i++)
            if (match(v.v[i], p, fold)) {
                char w[96];
                snprintf(w, sizeof(w), brace ? "${%s}" : "$%s", v.v[i]);
                add(c, w, v.v[i], 0, K_VAR, 0);
            }
        return c->n;
    }
    /* ~users */
    if (prefix[0] == '~' && !strchr(prefix, '/')) {
        struct strv v = { 0 };
        passwd_users(fold ? "" : prefix + 1, &v);
        for (int i = 0; i < v.n; i++)
            if (match(v.v[i], prefix + 1, fold)) {
                char w[64];
                snprintf(w, sizeof(w), "~%s", v.v[i]);
                add(c, w, w, 0, K_USER, '/');
            }
        return c->n;
    }
    if (argi < 0) {
        files(c, prefix, 0, 0, fold);
        return c->n;
    }
    if (cmdpos) {
        if (strchr(prefix, '/'))
            files(c, prefix, 0, 1, fold);
        else
            commands(c, prefix, fold);
        return c->n;
    }
    if (!cmd)
        cmd = "";
    /* options and subcommands */
    if ((prefix[0] == '-' && strcmp(cmd, "kill")) || (is_subcommand_cmd(cmd) && argi == 1)) {
        for (int i = 0; options[i].cmd; i++)
            if (!strcmp(options[i].cmd, cmd) && match(options[i].opt, prefix, fold))
                add(c, options[i].opt, options[i].opt, options[i].desc, K_OPT, ' ');
        if (c->n || prefix[0] == '-')
            return c->n;
    }
    if (!strcmp(cmd, "cd") || !strcmp(cmd, "pushd") || !strcmp(cmd, "rmdir") ||
        !strcmp(cmd, "chdir")) {
        files(c, prefix, 1, 0, fold);
        return c->n;
    }
    if (!strcmp(cmd, "fg") || !strcmp(cmd, "bg") || !strcmp(cmd, "disown") || !strcmp(cmd, "wait") ||
        (!strcmp(cmd, "kill") && prefix[0] == '%')) {
        jobs_cands(c, prefix);
        return c->n;
    }
    if (!strcmp(cmd, "kill") && prefix[0] == '-') {
        signal_cands(c, prefix, "-");
        return c->n;
    }
    if (!strcmp(cmd, "trap") && argi >= 2) {
        signal_cands(c, prefix, "");
        add(c, "EXIT", "EXIT", "when the shell exits", K_PLAIN, ' ');
        return c->n;
    }
    if (!strcmp(cmd, "setopt") || !strcmp(cmd, "unsetopt")) {
        int want = cmd[0] == 's' ? 0 : 1;
        for (int i = 0; i < NOPTS; i++)
            if (opts[i] == want && match(opt_names[i], prefix, fold))
                add(c, opt_names[i], opt_names[i], 0, K_PLAIN, ' ');
        return c->n;
    }
    if (!strcmp(cmd, "unset") || !strcmp(cmd, "export") || !strcmp(cmd, "local") ||
        !strcmp(cmd, "typeset") || !strcmp(cmd, "readonly") || !strcmp(cmd, "integer") ||
        !strcmp(cmd, "declare") || !strcmp(cmd, "read")) {
        struct strv v = { 0 };
        var_names(fold ? "" : prefix, &v);
        for (int i = 0; i < v.n; i++)
            if (match(v.v[i], prefix, fold))
                add(c, v.v[i], v.v[i], 0, K_VAR, ' ');
        return c->n;
    }
    if (!strcmp(cmd, "unalias")) {
        struct strv v = { 0 };
        alias_names(prefix, &v);
        for (int i = 0; i < v.n; i++)
            add(c, v.v[i], v.v[i], alias_get(v.v[i]), K_ALIAS, ' ');
        return c->n;
    }
    if (!strcmp(cmd, "unfunction") || !strcmp(cmd, "functions")) {
        struct strv v = { 0 };
        func_names(prefix, &v);
        for (int i = 0; i < v.n; i++)
            add(c, v.v[i], v.v[i], 0, K_FUNC, ' ');
        return c->n;
    }
    if (!strcmp(cmd, "type") || !strcmp(cmd, "which") || !strcmp(cmd, "whence") ||
        !strcmp(cmd, "help") || (!strcmp(cmd, "strace") && argi == 1)) {
        if (!strcmp(cmd, "help")) {
            struct strv v = { 0 };
            builtin_names(prefix, &v);
            for (int i = 0; i < v.n; i++)
                add(c, v.v[i], v.v[i], 0, K_BUILTIN, ' ');
        } else {
            commands(c, prefix, fold);
        }
        return c->n;
    }
    if (!strcmp(cmd, "su")) {
        struct strv v = { 0 };
        passwd_users(prefix, &v);
        for (int i = 0; i < v.n; i++)
            add(c, v.v[i], v.v[i], 0, K_USER, ' ');
        return c->n;
    }
    (void)buf;
    (void)start;
    (void)end;
    files(c, prefix, 0, 0, fold);
    return c->n;
}

void complete_collect(const char *buf, int len, int cursor, struct cands *out)
{
    /* the word around the cursor */
    int start = cursor, end = cursor;
    while (start > 0) {
        char c = buf[start - 1];
        if (strchr(" \t\n;&|<>()", c) && !(start >= 2 && buf[start - 2] == '\\'))
            break;
        start--;
    }
    while (end < len && !strchr(" \t\n;&|<>()", buf[end]))
        end++;
    /* a redirection operator glued to the word: 2>file, >file */
    char *cmd = 0;
    int argi = 0;
    int cmdpos = command_context(buf, start, &cmd, &argi);
    char *prefix = unescape(buf + start, end - start);
    out->start = start;
    out->end = end;
    out->prefix = prefix;
    out->n = 0;
    if (!collect(out, buf, start, end, prefix, cmdpos, cmd, argi, 0))
        collect(out, buf, start, end, prefix, cmdpos, cmd, argi, 1);
    sort_cands(out);
}
