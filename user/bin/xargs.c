/*
 * xargs: build command lines from standard input and run them.
 *
 *   xargs [-0] [-d DELIM] [-n MAX] [-L LINES] [-I STR] [-s SIZE] [-P N]
 *         [-r] [-t] [-p] [-x] [COMMAND [ARG...]]
 *
 * Items are separated by blanks and newlines, with '...', "..." and \x
 * quoting (or by NUL with -0, by DELIM with -d, by lines with -I and -L).
 * The default command is echo. Exit status: 0, 123 if a command failed,
 * 124 if one exited with 255, 125 if one was killed, 126/127 if one could
 * not be run.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int nul, delim = -1, max_args, max_lines, trace, prompt, no_empty, exact, max_procs = 1;
static long max_chars = 64 * 1024;
static const char *replace;
static char **cmd;
static int ncmd;
static int status, running;

static void usage(void)
{
    eprintf("usage: xargs [-0] [-d DELIM] [-n MAX] [-L LINES] [-I STR] [-s SIZE] [-P N] [-rtpx] "
            "[COMMAND [ARG...]]\n");
    exit(1);
}

/* Read the next item; 1 if got one. *eol: it ended a line (for -L). */
static int next_item(char **out, int *eol)
{
    static char *b;
    static size_t cap;
    size_t n = 0;
    int c, quote = 0, got = 0;
    *eol = 0;
#define PUT(ch) do { if (n + 2 > cap) b = realloc(b, cap = cap ? cap * 2 : 256); b[n++] = (ch); } while (0)
    if (nul || delim >= 0) {
        int d = nul ? 0 : delim;
        while ((c = getchar()) != EOF && c != d)
            { PUT(c); got = 1; }
        if (c == EOF && !got)
            return 0;
        *eol = 1;
    } else if (replace) {
        /* -I: one item per line, leading blanks dropped */
        while ((c = getchar()) == ' ' || c == '\t')
            ;
        for (; c != EOF && c != '\n'; c = getchar())
            { PUT(c); got = 1; }
        if (c == EOF && !got)
            return 0;
        *eol = 1;
        if (!got)
            return next_item(out, eol);
    } else {
        while ((c = getchar()) == ' ' || c == '\t' || c == '\n')
            ;
        if (c == EOF)
            return 0;
        for (; c != EOF; c = getchar()) {
            if (quote) {
                if (c == quote)
                    quote = 0;
                else if (c == '\n') {
                    eprintf("xargs: unmatched %s quote\n", quote == '"' ? "double" : "single");
                    exit(1);
                } else
                    PUT(c);
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                got = 1;
                continue;
            }
            if (c == '\\') {
                c = getchar();
                if (c == EOF)
                    break;
                PUT(c);
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n') {
                *eol = c == '\n';
                break;
            }
            PUT(c);
        }
        if (quote) {
            eprintf("xargs: unmatched %s quote\n", quote == '"' ? "double" : "single");
            exit(1);
        }
    }
    PUT(0);
#undef PUT
    *out = strdup(b);
    return 1;
}

static void reap(int block)
{
    int st;
    pid_t pid;
    while (running && (pid = waitpid(-1, &st, block ? 0 : WNOHANG)) > 0) {
        running--;
        if (WIFEXITED(st)) {
            int code = WEXITSTATUS(st);
            if (code == 255) {
                eprintf("xargs: %s: exited with status 255; aborting\n", cmd[0]);
                exit(124);
            }
            if (code == 126 || code == 127)
                status = code;
            else if (code && !status)
                status = 123;
        } else {
            eprintf("xargs: %s: terminated by signal %d\n", cmd[0], WTERMSIG(st));
            exit(125);
        }
        block = 0;
    }
}

static int confirm(void)
{
    eprintf("?...");
    int fd = open("/dev/tty", O_RDONLY);
    char line[64];
    long r = read(fd >= 0 ? fd : 2, line, sizeof(line));
    if (fd >= 0)
        close(fd);
    return r > 0 && (line[0] == 'y' || line[0] == 'Y');
}

static void run(char **items, int nitems)
{
    char **argv;
    int argc = 0;
    if (replace) {
        argv = calloc(ncmd + 1, sizeof(char *));
        for (int i = 0; i < ncmd; i++) {
            const char *a = cmd[i], *hit = strstr(a, replace);
            if (!hit) {
                argv[argc++] = strdup(a);
                continue;
            }
            /* every occurrence of STR in the argument */
            size_t rl = strlen(replace), il = strlen(items[0]);
            char *o = malloc(strlen(a) * (il + 1) + 1), *q = o;
            while ((hit = strstr(a, replace))) {
                memcpy(q, a, hit - a);
                q += hit - a;
                memcpy(q, items[0], il);
                q += il;
                a = hit + rl;
            }
            strcpy(q, a);
            argv[argc++] = o;
        }
    } else {
        argv = calloc(ncmd + nitems + 1, sizeof(char *));
        for (int i = 0; i < ncmd; i++)
            argv[argc++] = cmd[i];
        for (int i = 0; i < nitems; i++)
            argv[argc++] = items[i];
    }
    argv[argc] = 0;
    if (trace || prompt) {
        for (int i = 0; i < argc; i++)
            eprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
        if (prompt) {
            eprintf(" ");
            if (!confirm())
                goto out;
        } else {
            eprintf("\n");
        }
    }
    while (running >= max_procs)
        reap(1);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        eprintf("xargs: fork: %s\n", strerror(errno));
        exit(1);
    }
    if (pid == 0) {
        /* commands read the terminal, not our input */
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, 0);
            close(fd);
        }
        execvp(argv[0], argv);
        int e = errno;
        eprintf("xargs: %s: %s\n", argv[0], strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
    running++;
    if (max_procs == 1)
        reap(1);
out:
    if (replace)
        for (int i = 0; i < argc; i++)
            free(argv[i]);
    free(argv);
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "0d:n:L:l:I:i:s:P:rtpxE:e:")) != -1) {
        switch (c) {
        case '0': nul = 1; break;
        case 'd':
            delim = optarg[0] == '\\' && optarg[1] == 'n' ? '\n' : optarg[0] == '\\' && optarg[1] == 't' ? '\t'
                  : optarg[0] == '\\' && optarg[1] == '0' ? 0 : (unsigned char)optarg[0];
            break;
        case 'n': max_args = atoi(optarg); break;
        case 'L':
        case 'l': max_lines = atoi(optarg); break;
        case 'I':
        case 'i': replace = optarg; break;
        case 's': max_chars = atol(optarg); break;
        case 'P': max_procs = atoi(optarg) > 0 ? atoi(optarg) : 64; break;
        case 'r': no_empty = 1; break;
        case 't': trace = 1; break;
        case 'p': prompt = 1; break;
        case 'x': exact = 1; break;
        case 'E':
        case 'e': break;                /* end-of-file strings: not supported, ignored */
        default: usage();
        }
    }
    static char *echo[] = { "echo" };
    cmd = optind < argc ? argv + optind : echo;
    ncmd = optind < argc ? argc - optind : 1;
    if (replace)
        max_args = 1, max_lines = 0;
    size_t base = 0;
    for (int i = 0; i < ncmd; i++)
        base += strlen(cmd[i]) + 1;

    char **items = 0;
    int n = 0, cap = 0, lines = 0, ran = 0;
    size_t chars = base;
    char *it;
    int eol;
    while (next_item(&it, &eol)) {
        size_t len = strlen(it) + 1;
        if (n && chars + len > (size_t)max_chars) {
            if (exact) {
                eprintf("xargs: argument line too long\n");
                exit(1);
            }
            run(items, n);
            ran = 1;
            for (int i = 0; i < n; i++)
                free(items[i]);
            n = 0, chars = base, lines = 0;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            items = realloc(items, cap * sizeof(char *));
        }
        items[n++] = it;
        chars += len;
        lines += eol;
        if ((max_args && n >= max_args) || (max_lines && lines >= max_lines)) {
            run(items, n);
            ran = 1;
            for (int i = 0; i < n; i++)
                free(items[i]);
            n = 0, chars = base, lines = 0;
        }
    }
    if (n || (!ran && !no_empty && !replace))
        run(items, n);
    while (running)
        reap(1);
    return status;
}
